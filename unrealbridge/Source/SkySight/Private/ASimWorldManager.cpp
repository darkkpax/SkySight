#include "ASimWorldManager.h"

#include "ADronePawn.h"
#include "AFireSourceActor.h"
#include "UUnrealBridgeProtocol.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Math/UnrealMathUtility.h"
#include "TimerManager.h"

ASimWorldManager::ASimWorldManager()
{
	PrimaryActorTick.bCanEverTick = true;
	DroneClass = ADronePawn::StaticClass();
	FireSourceClass = AFireSourceActor::StaticClass();
}

const TArray<AFireSourceActor*>& ASimWorldManager::GetActiveFires() const
{
	ActiveFireSourcesCache.Reset();
	ActiveFireSourcesCache.Reserve(ActiveFires.Num());

	for (const TWeakObjectPtr<AActor>& FireActorPtr : ActiveFires)
	{
		if (AFireSourceActor* FireSource = Cast<AFireSourceActor>(FireActorPtr.Get()))
		{
			ActiveFireSourcesCache.Add(FireSource);
		}
	}

	return ActiveFireSourcesCache;
}

void ASimWorldManager::BeginPlay()
{
	Super::BeginPlay();

	GeoReference.OriginLatDeg = OriginLatDeg;
	GeoReference.OriginLonDeg = OriginLonDeg;
	GeoReference.OriginAltMeters = OriginAltMeters;
	GeoReference.MetersToUnrealCm = MetersToUnrealCm;
	GeoReference.OriginWorldCm = GetActorLocation();

	const bool bLatitudeValid = OriginLatDeg >= -90.0 && OriginLatDeg <= 90.0;
	const bool bLongitudeValid = OriginLonDeg >= -180.0 && OriginLonDeg <= 180.0;
	if (!bLatitudeValid || !bLongitudeValid)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("Invalid geo origin (Lat=%f Lon=%f). Resetting to Seattle defaults (Lat=47.606200 Lon=-122.332100)."),
			OriginLatDeg,
			OriginLonDeg
		);

		OriginLatDeg = 47.6062;
		OriginLonDeg = -122.3321;
		GeoReference.OriginLatDeg = OriginLatDeg;
		GeoReference.OriginLonDeg = OriginLonDeg;
	}

	UE_LOG(LogTemp, Log, TEXT("World ready; waiting for route to spawn drone"));

	static const TCHAR* SimScalabilityCommands[] =
	{
		TEXT("sg.ShadowQuality 0"),
		TEXT("sg.PostProcessQuality 1"),
		TEXT("sg.EffectsQuality 1"),
		TEXT("sg.FoliageQuality 0"),
		TEXT("r.ScreenPercentage 80"),
		TEXT("r.Lumen.GlobalIllumination 0"),
		TEXT("r.Lumen.Reflections 0")
	};

	for (const TCHAR* Command : SimScalabilityCommands)
	{
		UKismetSystemLibrary::ExecuteConsoleCommand(this, Command, nullptr);
	}
	UE_LOG(LogTemp, Log, TEXT("Sim scalability applied: shadows=0 post=1 effects=1 foliage=0 screen%%=80 lumenGI/reflections=0"));

	if (bAutoSpawnFires && FireSpawnIntervalSec > 0.1f)
	{
		GetWorldTimerManager().SetTimer(
			FireSpawnTimerHandle,
			this,
			&ASimWorldManager::SpawnFireInCameraFov,
			FireSpawnIntervalSec,
			true,
			FireSpawnIntervalSec
		);
	}

	if (bEnableFireSpread && FireSpreadUpdateIntervalSec > 0.0f)
	{
		GetWorldTimerManager().SetTimer(
			FireSpreadTimerHandle,
			this,
			&ASimWorldManager::UpdateFireSpread,
			FireSpreadUpdateIntervalSec,
			true,
			FireSpreadUpdateIntervalSec
		);
	}
}

void ASimWorldManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	CleanupFires();
}

void ASimWorldManager::SpawnDrone()
{
	SpawnDroneAtWorldXY(FVector2D(GetActorLocation().X, GetActorLocation().Y));
}

bool ASimWorldManager::SpawnDroneAtGeo(double LatitudeDeg, double LongitudeDeg, double AltitudeMeters)
{
	const double GeoAltMeters = FMath::IsFinite(AltitudeMeters) ? AltitudeMeters : GeoReference.OriginAltMeters;
	const FVector WorldCm = UUnrealBridgeProtocol::GeoToUnrealCm(GeoReference, LatitudeDeg, LongitudeDeg, GeoAltMeters);

	UE_LOG(LogTemp, Log, TEXT("[DroneSpawn] Geo request lat=%.8f lon=%.8f alt=%.2fm -> worldXY=(%.2f, %.2f)"),
		LatitudeDeg, LongitudeDeg, GeoAltMeters, WorldCm.X, WorldCm.Y);

	const bool bSpawned = SpawnDroneAtWorldXY(FVector2D(WorldCm.X, WorldCm.Y));
	if (bSpawned && IsValid(DronePawn))
	{
		FVector SpawnLocation = DronePawn->GetActorLocation();
		SpawnLocation.Z = WorldCm.Z;
		DronePawn->SetActorLocation(SpawnLocation, false);
		DronePawn->ConfigureAltitudeHold(WorldCm.Z, /*bDisableTerrainFollow*/ true);
	}
	return bSpawned;
}

bool ASimWorldManager::SpawnDroneAtWorldXY(const FVector2D& SpawnXYCm)
{
	if (!GetWorld() || !DroneClass)
	{
		return false;
	}

	if (IsValid(DronePawn))
	{
		UE_LOG(LogTemp, Log, TEXT("[DroneSpawn] Drone already exists at %s; spawn request ignored (requested XY=%.2f, %.2f)"),
			*DronePawn->GetActorLocation().ToString(), SpawnXYCm.X, SpawnXYCm.Y);
		return true;
	}

	FVector SpawnLocation(SpawnXYCm.X, SpawnXYCm.Y, GetActorLocation().Z);

	// Trace ground at requested XY and keep current fixed-altitude-above-ground spawn behavior.
	float GroundZ = SpawnLocation.Z;
	const bool bGroundTraceOk = TraceGroundZ(SpawnXYCm, GroundZ);
	if (!bGroundTraceOk)
	{
		GroundZ = DroneSpawnWorldZ;
	}

	const float FixedWorldAltitudeCm = GroundZ + SpawnAGLMeters * 100.0f;
	SpawnLocation.Z = FixedWorldAltitudeCm;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	DronePawn = GetWorld()->SpawnActor<ADronePawn>(DroneClass, SpawnLocation, FRotator(0.0f, 0.0f, 0.0f), SpawnParams);

	if (DronePawn)
	{
		DronePawn->ConfigureAltitudeHold(FixedWorldAltitudeCm, /*bDisableTerrainFollow*/ true);
		DronePawn->InitializeForSimulation(GeoReference, TelemetryHz, DetectionHz);

		// Keep sim movement smooth around foliage/trees.
		DronePawn->SetActorEnableCollision(false);

		UE_LOG(LogTemp, Log, TEXT("[DroneSpawn] Spawned at %s (requestedXY=%.2f, %.2f, groundZ=%.2f, trace=%s, fixedZ=%.2fcm, AGL=%.1fm)"),
			*SpawnLocation.ToString(),
			SpawnXYCm.X,
			SpawnXYCm.Y,
			GroundZ,
			bGroundTraceOk ? TEXT("true") : TEXT("false"),
			FixedWorldAltitudeCm,
			SpawnAGLMeters);
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[DroneSpawn] Failed to spawn drone at XY=(%.2f, %.2f)"), SpawnXYCm.X, SpawnXYCm.Y);
		return false;
	}
}

bool ASimWorldManager::ApplyRouteToDrone(const FSkysightRoute& Route)
{
	if (!GetWorld())
	{
		UE_LOG(LogTemp, Warning, TEXT("[RouteApply] World is null; cannot apply route"));
		return false;
	}

	if (!IsValid(DronePawn))
	{
		for (TActorIterator<ADronePawn> It(GetWorld()); It; ++It)
		{
			DronePawn = *It;
			break;
		}
	}

	if (!IsValid(DronePawn))
	{
		bool bSpawnedFromRouteStart = false;
		if (Route.Waypoints.Num() > 0)
		{
			const FSkysightWaypoint& StartWp = Route.Waypoints[0];
			const double SpawnAltMeters = StartWp.AltitudeMeters;
			UE_LOG(LogTemp, Log, TEXT("[RouteApply] Spawning drone at waypoint[0]: lat=%.8f lon=%.8f alt=%.2f"),
				StartWp.LatitudeDeg, StartWp.LongitudeDeg, StartWp.AltitudeMeters);
			bSpawnedFromRouteStart = SpawnDroneAtGeo(StartWp.LatitudeDeg, StartWp.LongitudeDeg, SpawnAltMeters);
		}

		if (!bSpawnedFromRouteStart)
		{
			UE_LOG(LogTemp, Log, TEXT("[RouteApply] Fallback spawn at world manager location"));
			SpawnDrone();
		}
	}

	if (!IsValid(DronePawn))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RouteApply] Drone unavailable after spawn attempt; waypoints=%d"), Route.Waypoints.Num());
		return false;
	}

	DronePawn->ApplyRoute(Route);
	UE_LOG(LogTemp, Log, TEXT("[RouteApply] Route applied to drone %s (%d waypoints)"),
		*GetNameSafe(DronePawn), Route.Waypoints.Num());
	return true;
}

bool ASimWorldManager::DespawnDrone()
{
	if (!IsValid(DronePawn))
	{
		return true;
	}

	AActor* DroneToDestroy = DronePawn;
	DronePawn = nullptr;
	if (IsValid(DroneToDestroy))
	{
		UE_LOG(LogTemp, Log, TEXT("[DroneSpawn] Despawning drone %s"), *GetNameSafe(DroneToDestroy));
		DroneToDestroy->Destroy();
	}
	return true;
}

AActor* ASimWorldManager::GetDroneActor() const
{
	if (IsValid(DronePawn))
	{
		return DronePawn;
	}

	if (!GetWorld())
	{
		return nullptr;
	}

	for (TActorIterator<ADronePawn> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

USceneCaptureComponent2D* ASimWorldManager::GetDroneSceneCapture() const
{
	if (AActor* DroneActor = GetDroneActor())
	{
		return DroneActor->FindComponentByClass<USceneCaptureComponent2D>();
	}

	return nullptr;
}

bool ASimWorldManager::SphereTraceToGroundFromCamera(FHitResult& OutHit, FVector& OutCamLoc, FVector& OutStart, FVector& OutEnd) const
{
	UWorld* World = GetWorld();
	const USceneCaptureComponent2D* Capture = GetDroneSceneCapture();
	if (!World || !Capture)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[FireSpawn][SphereSweep] Cannot trace: World=%s Capture=%s"),
			World ? TEXT("valid") : TEXT("null"),
			Capture ? TEXT("valid") : TEXT("null")
		);
		return false;
	}

	const FVector Forward = Capture->GetForwardVector().GetSafeNormal();
	if (Forward.IsNearlyZero())
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn][SphereSweep] Cannot trace: camera forward vector is nearly zero."));
		return false;
	}

	const float MinDistanceCm = FireSpawnMinDistanceMeters * 100.0f;
	const float MaxDistanceCm = FireSpawnMaxDistanceMeters * 100.0f;
	const FVector CaptureLocation = Capture->GetComponentLocation();
	const FVector Start = CaptureLocation + Forward * MinDistanceCm;
	const FVector End = CaptureLocation + Forward * MaxDistanceCm;
	OutCamLoc = CaptureLocation;
	OutStart = Start;
	OutEnd = End;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FireSpawnSphereTrace), false);
	Params.AddIgnoredActor(const_cast<ASimWorldManager*>(this));
	if (AActor* DroneActor = GetDroneActor())
	{
		Params.AddIgnoredActor(DroneActor);
	}

	const FCollisionShape Shape = FCollisionShape::MakeSphere(FireSpawnSphereRadius);
	bool bHit = World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_Visibility, Shape, Params);
	if (!bHit)
	{
		bHit = World->SweepSingleByChannel(OutHit, Start, End, FQuat::Identity, ECC_WorldStatic, Shape, Params);
	}
	if (!bHit)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("[FireSpawn][SphereSweep] No hit along trace. Cam=%s Start=%s End=%s Radius=%.1f"),
			*CaptureLocation.ToString(),
			*Start.ToString(),
			*End.ToString(),
			FireSpawnSphereRadius
		);
	}

	return bHit;
}

bool ASimWorldManager::IsDroneOverSpawnSphere(const FVector& SphereCenter, FHitResult* OutDroneDownHit) const
{
	UWorld* World = GetWorld();
	AActor* DroneActor = GetDroneActor();
	if (!World || !DroneActor)
	{
		return false;
	}

	const FVector DroneStart = DroneActor->GetActorLocation();
	const FVector DroneEnd = DroneStart - FVector(0.0f, 0.0f, DroneDownTraceLength);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FireSpawnDroneDownTrace), false);
	Params.AddIgnoredActor(const_cast<ASimWorldManager*>(this));
	Params.AddIgnoredActor(DroneActor);

	FHitResult Hit;
	bool bHit = World->LineTraceSingleByChannel(Hit, DroneStart, DroneEnd, ECC_Visibility, Params);
	if (!bHit)
	{
		bHit = World->LineTraceSingleByChannel(Hit, DroneStart, DroneEnd, ECC_WorldStatic, Params);
	}
	if (OutDroneDownHit)
	{
		*OutDroneDownHit = Hit;
	}

	if (!bHit)
	{
		return false;
	}

	return FVector::Dist2D(Hit.ImpactPoint, SphereCenter) <= FireSpawnSphereRadius;
}

bool ASimWorldManager::FindPointInsideSpawnSphereOnGround(const FVector& SphereCenter, FVector& OutLocation) const
{
	UWorld* World = GetWorld();
	AActor* DroneActor = GetDroneActor();
	if (!World || !DroneActor)
	{
		return false;
	}

	const USceneCaptureComponent2D* SceneCapture = GetDroneSceneCapture();
	const FVector ViewOrigin = SceneCapture ? SceneCapture->GetComponentLocation() : DroneActor->GetActorLocation();
	const FVector ViewForward = SceneCapture ? SceneCapture->GetForwardVector().GetSafeNormal() : DroneActor->GetActorForwardVector().GetSafeNormal();
	if (ViewForward.IsNearlyZero())
	{
		return false;
	}

	const float MinDistanceCm = FireSpawnMinDistanceMeters * 100.0f;
	const float MaxDistanceCm = FireSpawnMaxDistanceMeters * 100.0f;
	const float HalfFovDeg = FMath::Clamp(FireSpawnHalfFovDeg, 1.0f, 89.0f);
	const float CosHalfFov = FMath::Cos(FMath::DegreesToRadians(HalfFovDeg));

	const float MinSeparationCm = FireSpawnMinSeparation * 100.0f;

	for (int32 Attempt = 0; Attempt < MaxPointAttemptsInSphere; ++Attempt)
	{
		const float R = FireSpawnSphereRadius * FMath::Sqrt(FMath::FRand());
		const float Ang = FMath::FRand() * 2.0f * PI;
		const FVector Offset(R * FMath::Cos(Ang), R * FMath::Sin(Ang), 0.0f);
		const FVector CandidateXY = SphereCenter + Offset;

		float GroundZ = 0.0f;
		const bool bGroundTraceOk = TraceGroundZ(FVector2D(CandidateXY.X, CandidateXY.Y), GroundZ);
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[FireSpawn][TraceGroundZ] Attempt=%d XY=%s Success=%s GroundZ=%.2f"),
			Attempt + 1,
			*FVector(CandidateXY.X, CandidateXY.Y, 0.0f).ToString(),
			bGroundTraceOk ? TEXT("true") : TEXT("false"),
			GroundZ
		);
		if (!bGroundTraceOk)
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireSpawn][TraceGroundZ] Failed to resolve ground Z. Candidate rejected, no spawn with default Z."));
			continue;
		}

		const FVector CandidateLocation(CandidateXY.X, CandidateXY.Y, GroundZ);

		const float DistToViewCm = FVector::Distance(ViewOrigin, CandidateLocation);
		if (DistToViewCm < MinDistanceCm || DistToViewCm > MaxDistanceCm)
		{
			continue;
		}

		const FVector ToCandidate = (CandidateLocation - ViewOrigin).GetSafeNormal();
		const float Dot = (!ToCandidate.IsNearlyZero()) ? FVector::DotProduct(ViewForward, ToCandidate) : -1.0f;
		if (Dot < CosHalfFov)
		{
			continue;
		}

		bool bTooCloseToFire = false;
		for (const TWeakObjectPtr<AActor>& FirePtr : ActiveFires)
		{
			if (const AActor* FireActor = FirePtr.Get())
			{
				if (FVector::Distance(FireActor->GetActorLocation(), CandidateLocation) < MinSeparationCm)
				{
					bTooCloseToFire = true;
					break;
				}
			}
		}
		if (bTooCloseToFire)
		{
			continue;
		}

		OutLocation = CandidateLocation;
		return true;
	}

	return false;
}

void ASimWorldManager::DrawFireSpawnDebug(const FVector& CamLoc, const FVector& Start, const FVector& End, const FHitResult& SphereHit, const FVector& SphereCenter, const FVector& ChosenSpawn, bool bDroneBlocks, const FHitResult& DroneDownHit) const
{
	// Fire spawn visual debug is disabled; keep function to preserve call sites and logs.
	(void)CamLoc;
	(void)Start;
	(void)End;
	(void)SphereHit;
	(void)SphereCenter;
	(void)ChosenSpawn;
	(void)bDroneBlocks;
	(void)DroneDownHit;
}

void ASimWorldManager::SpawnFireInCameraFov()
{
	UWorld* World = GetWorld();
	AActor* DroneActor = GetDroneActor();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] Aborted: world is null."));
		return;
	}
	if (!DroneActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] Aborted: DroneActor is null; spawn drone first."));
		return;
	}

	CleanupFires();

	if (MaxActiveFires <= 0)
	{
		return;
	}

	if (bPersistentFires && ActiveFires.Num() >= MaxActiveFires)
	{
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[FireSpawn] Fire cap reached (%d/%d) with bPersistentFires=true; skipping oldest-fire destruction."),
			ActiveFires.Num(),
			MaxActiveFires
		);
	}
	else
	{
		while (ActiveFires.Num() >= MaxActiveFires)
		{
			AActor* OldestFire = nullptr;
			for (const TWeakObjectPtr<AActor>& FirePtr : ActiveFires)
			{
				if (FirePtr.IsValid())
				{
					OldestFire = FirePtr.Get();
					break;
				}
			}

			if (!OldestFire)
			{
				CleanupFires();
				break;
			}

			OldestFire->Destroy();
			CleanupFires();
		}
	}

	if (ActiveFires.Num() >= MaxActiveFires)
	{
		return;
	}

	FVector SpawnLocation = FVector::ZeroVector;
	bool bFoundSpawn = false;
	for (int32 SphereAttempt = 0; SphereAttempt < MaxSphereCenterAttempts; ++SphereAttempt)
	{
		FHitResult SphereHit;
		FVector CamLoc = FVector::ZeroVector;
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		if (!SphereTraceToGroundFromCamera(SphereHit, CamLoc, Start, End))
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireSpawn][SphereSweep] Attempt=%d failed: no blocking hit"), SphereAttempt + 1);
			continue;
		}

		const FVector SphereCenter = SphereHit.ImpactPoint;
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[FireSpawn][SphereSweep] Attempt=%d BlockingHit=%s ImpactPoint=%s Actor=%s"),
			SphereAttempt + 1,
			SphereHit.bBlockingHit ? TEXT("true") : TEXT("false"),
			*SphereHit.ImpactPoint.ToString(),
			*GetNameSafe(SphereHit.GetActor())
		);

		FHitResult DroneDownHit;
		DrawFireSpawnDebug(CamLoc, Start, End, SphereHit, SphereCenter, FVector::ZeroVector, false, DroneDownHit);

		const bool bDroneBlocks = IsDroneOverSpawnSphere(SphereCenter, &DroneDownHit);
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[FireSpawn][DroneCheck] Attempt=%d Blocks=%s DroneDownHit=%s DroneDownActor=%s"),
			SphereAttempt + 1,
			bDroneBlocks ? TEXT("true") : TEXT("false"),
			DroneDownHit.bBlockingHit ? *DroneDownHit.ImpactPoint.ToString() : TEXT("None"),
			*GetNameSafe(DroneDownHit.GetActor())
		);
		if (bDroneBlocks)
		{
			DrawFireSpawnDebug(CamLoc, Start, End, SphereHit, SphereCenter, FVector::ZeroVector, true, DroneDownHit);
			UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] Rejected: drone down-trace is inside spawn sphere."));
			continue;
		}

		if (FindPointInsideSpawnSphereOnGround(SphereCenter, SpawnLocation))
		{
			UE_LOG(LogTemp, Log, TEXT("[FireSpawn][ChoosePoint] Attempt=%d SpawnLocation=%s"), SphereAttempt + 1, *SpawnLocation.ToString());
			DrawFireSpawnDebug(CamLoc, Start, End, SphereHit, SphereCenter, SpawnLocation, false, DroneDownHit);
			bFoundSpawn = true;
			break;
		}

		DrawFireSpawnDebug(CamLoc, Start, End, SphereHit, SphereCenter, FVector::ZeroVector, false, DroneDownHit);
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn][ChoosePoint] Attempt=%d failed: no valid point in sphere."), SphereAttempt + 1);
	}

	if (!bFoundSpawn)
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to find fire spawn point in drone camera FOV."));
		return;
	}

	UClass* SpawnClass = FireActorClassOverride ? FireActorClassOverride.Get() : nullptr;
	if (!SpawnClass && FireSourceClass)
	{
		SpawnClass = FireSourceClass.Get();
	}
	if (!SpawnClass)
	{
		SpawnClass = AFireSourceActor::StaticClass();
	}

	if (!SpawnClass->IsChildOf(AFireSourceActor::StaticClass()))
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("FireActorClassOverride (%s) is not a child of AFireSourceActor. Falling back to AFireSourceActor."),
			*GetNameSafe(SpawnClass)
		);
		SpawnClass = AFireSourceActor::StaticClass();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	SpawnLocation.Z += 50.0f;
	AActor* SpawnedFire = World->SpawnActor<AActor>(SpawnClass, SpawnLocation, FRotator::ZeroRotator, SpawnParams);
	if (!SpawnedFire)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("Failed to spawn fire actor: Class=%s Abstract=%s Collision=%s Loc=%s"),
			*GetNameSafe(SpawnClass),
			(SpawnClass && SpawnClass->HasAnyClassFlags(CLASS_Abstract)) ? TEXT("true") : TEXT("false"),
			TEXT("AdjustIfPossibleButAlwaysSpawn"),
			*SpawnLocation.ToString()
		);
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("spawned fire class = %s"), *GetNameSafe(SpawnedFire->GetClass()));

	SpawnedFire->SetActorHiddenInGame(false);
	SpawnedFire->SetActorEnableCollision(true);

	const USceneComponent* FireRootComponent = SpawnedFire->GetRootComponent();
	UE_LOG(
		LogTemp,
		Log,
		TEXT("FireSpawnActorState: Actor=%s Hidden=%s RootVisible=%s Loc=%s"),
		*GetNameSafe(SpawnedFire),
		SpawnedFire->IsHidden() ? TEXT("true") : TEXT("false"),
		FireRootComponent ? (FireRootComponent->IsVisible() ? TEXT("true") : TEXT("false")) : TEXT("none"),
		*SpawnedFire->GetActorLocation().ToString()
	);

	SpawnedFire->OnDestroyed.AddDynamic(this, &ASimWorldManager::OnFireDestroyed);
	ActiveFires.Add(SpawnedFire);

	if (AFireSourceActor* FireSource = Cast<AFireSourceActor>(SpawnedFire))
	{
		FireSource->SetFireId(FString::Printf(TEXT("fire-%d"), FireIndexCounter++));
	}

	const USceneCaptureComponent2D* SceneCapture = GetDroneSceneCapture();
	const FVector ViewOrigin = SceneCapture ? SceneCapture->GetComponentLocation() : DroneActor->GetActorLocation();
	const FVector ViewForward = SceneCapture ? SceneCapture->GetForwardVector().GetSafeNormal() : DroneActor->GetActorForwardVector().GetSafeNormal();
	const FVector ToSpawn = (SpawnLocation - ViewOrigin).GetSafeNormal();
	const float Dot = (!ViewForward.IsNearlyZero() && !ToSpawn.IsNearlyZero()) ? FVector::DotProduct(ViewForward, ToSpawn) : -1.0f;
	const float DistanceMeters = FVector::Distance(DroneActor->GetActorLocation(), SpawnLocation) / 100.0f;
	const bool bInFov = Dot > FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(FireSpawnHalfFovDeg, 1.0f, 89.0f)));

	double Lat = 0.0;
	double Lon = 0.0;
	double Alt = 0.0;
	UUnrealBridgeProtocol::UnrealCmToGeo(GeoReference, SpawnLocation, Lat, Lon, Alt);

	UE_LOG(
		LogTemp,
		Log,
		TEXT("Fire spawned: Actor=%s Loc=%s Dist=%.1fm Dot=%.3f InFOV=%s Active=%d (Lat=%.6f Lon=%.6f)"),
		*GetNameSafe(SpawnedFire),
		*SpawnLocation.ToString(),
		DistanceMeters,
		Dot,
		bInFov ? TEXT("true") : TEXT("false"),
		ActiveFires.Num(),
		Lat,
		Lon
	);

}

AActor* ASimWorldManager::SpawnFireAtLocation(const FVector& Location, const TCHAR* Reason)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] SpawnFireAtLocation failed: world is null. Reason=%s"),
			Reason ? Reason : TEXT("unknown"));
		return nullptr;
	}

	if (MaxActiveFires > 0 && ActiveFires.Num() >= MaxActiveFires)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] SpawnFireAtLocation failed: fire cap reached (%d/%d). Reason=%s"),
			ActiveFires.Num(), MaxActiveFires, Reason ? Reason : TEXT("unknown"));
		return nullptr;
	}

	UClass* SpawnClass = FireActorClassOverride ? FireActorClassOverride.Get() : nullptr;
	if (!SpawnClass && FireSourceClass)
	{
		SpawnClass = FireSourceClass.Get();
	}
	if (!SpawnClass || !SpawnClass->IsChildOf(AFireSourceActor::StaticClass()))
	{
		SpawnClass = AFireSourceActor::StaticClass();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	FVector SpawnLocation = Location;
	SpawnLocation.Z += SpreadSpawnZOffset;

	AActor* SpawnedFire = World->SpawnActor<AActor>(SpawnClass, SpawnLocation, FRotator::ZeroRotator, SpawnParams);
	if (!SpawnedFire)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawn] SpawnFireAtLocation failed: SpawnActor returned null. Class=%s Loc=%s Reason=%s"),
			*GetNameSafe(SpawnClass), *SpawnLocation.ToString(), Reason ? Reason : TEXT("unknown"));
		return nullptr;
	}

	SpawnedFire->SetActorHiddenInGame(false);
	SpawnedFire->SetActorEnableCollision(true);
	SpawnedFire->OnDestroyed.AddDynamic(this, &ASimWorldManager::OnFireDestroyed);
	ActiveFires.Add(SpawnedFire);

	UE_LOG(
		LogTemp,
		Warning,
		TEXT("[FireSpread] Spawned secondary fire. Reason=%s Class=%s Loc=%s"),
		Reason ? Reason : TEXT("unknown"),
		*GetNameSafe(SpawnedFire->GetClass()),
		*SpawnedFire->GetActorLocation().ToString()
	);

	if (AFireSourceActor* FireSource = Cast<AFireSourceActor>(SpawnedFire))
	{
		FireSource->SetFireId(FString::Printf(TEXT("fire-%d"), FireIndexCounter++));
	}

	return SpawnedFire;
}

bool ASimWorldManager::IsComponentBurnable(const UPrimitiveComponent* PrimitiveComponent) const
{
	if (!PrimitiveComponent)
	{
		return false;
	}

	const AActor* OwnerActor = PrimitiveComponent->GetOwner();
	if (!OwnerActor || OwnerActor == this || OwnerActor->IsA<AFireSourceActor>())
	{
		return false;
	}
	if (OwnerActor == GetDroneActor())
	{
		return false;
	}

	if (bRequireBurnableTag)
	{
		const bool bActorTagged = OwnerActor->ActorHasTag(BurnableTag);
		const bool bComponentTagged = PrimitiveComponent->ComponentHasTag(BurnableTag);
		if (!bActorTagged && !bComponentTagged)
		{
			return false;
		}
	}

	return PrimitiveComponent->GetNumMaterials() > 0;
}

void ASimWorldManager::ApplyBurnToComponent(UPrimitiveComponent* PrimitiveComponent, float BurnAmount)
{
	if (!PrimitiveComponent || BurnAmountParameterName.IsNone())
	{
		return;
	}

	TArray<TObjectPtr<UMaterialInstanceDynamic>>& CachedMIDs = BurnMIDsByComponent.FindOrAdd(PrimitiveComponent);
	if (CachedMIDs.Num() == 0)
	{
		const int32 MaterialCount = PrimitiveComponent->GetNumMaterials();
		CachedMIDs.Reserve(MaterialCount);
		for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
		{
			UMaterialInstanceDynamic* MID = PrimitiveComponent->CreateDynamicMaterialInstance(MaterialIndex);
			if (MID)
			{
				CachedMIDs.Add(MID);
			}
		}
	}

	const float ClampedBurnAmount = FMath::Clamp(BurnAmount, 0.0f, 1.0f);
	for (UMaterialInstanceDynamic* MID : CachedMIDs)
	{
		if (MID)
		{
			MID->SetScalarParameterValue(BurnAmountParameterName, ClampedBurnAmount);
		}
	}
}

void ASimWorldManager::UpdateGroundBurnMPC(const FVector& FireLocation, float RadiusCm, float Intensity) const
{
	if (!GroundBurnMPC)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	UMaterialParameterCollectionInstance* MPC = World->GetParameterCollectionInstance(GroundBurnMPC);
	if (!MPC)
	{
		return;
	}

	MPC->SetScalarParameterValue(GroundBurnCenterXParam, FireLocation.X);
	MPC->SetScalarParameterValue(GroundBurnCenterYParam, FireLocation.Y);
	MPC->SetScalarParameterValue(GroundBurnRadiusParam, RadiusCm);
	MPC->SetScalarParameterValue(GroundBurnIntensityParam, FMath::Clamp(Intensity, 0.0f, 1.0f));
}

void ASimWorldManager::UpdateFireSpread()
{
	UWorld* World = GetWorld();
	if (!World || !bEnableFireSpread)
	{
		return;
	}

	const double SpreadStartSec = FPlatformTime::Seconds();

	CleanupFires();
	if (ActiveFires.Num() == 0)
	{
		return;
	}

	const float RadiusCm = FireAffectRadiusMeters * 100.0f;
	const float DeltaBurn = BurnAccumulationPerSecond * FireSpreadUpdateIntervalSec;
	const FCollisionShape SpreadShape = FCollisionShape::MakeSphere(RadiusCm);
	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldStatic);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

	int32 UpdatedComponents = 0;
	++SpreadTickCounter;

	for (const TWeakObjectPtr<AActor>& FirePtr : ActiveFires)
	{
		const AActor* FireActor = FirePtr.Get();
		if (!FireActor)
		{
			continue;
		}

		const FVector FireLocation = FireActor->GetActorLocation();
		UpdateGroundBurnMPC(FireLocation, RadiusCm, 1.0f);

		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(FireSpreadOverlap), false);
		QueryParams.AddIgnoredActor(this);
		QueryParams.AddIgnoredActor(const_cast<AActor*>(FireActor));

		const bool bAnyOverlap = World->OverlapMultiByObjectType(
			Overlaps,
			FireLocation,
			FQuat::Identity,
			ObjectQueryParams,
			SpreadShape,
			QueryParams
		);

		if (!bAnyOverlap)
		{
			continue;
		}

		for (const FOverlapResult& Overlap : Overlaps)
		{
			UPrimitiveComponent* PrimitiveComponent = Overlap.GetComponent();
			if (!IsComponentBurnable(PrimitiveComponent))
			{
				continue;
			}

			AActor* BurnActor = PrimitiveComponent->GetOwner();
			if (!BurnActor)
			{
				continue;
			}

			const float DistanceCm = FVector::Distance(BurnActor->GetActorLocation(), FireLocation);
			const float DistanceFactor = FMath::Clamp(1.0f - (DistanceCm / RadiusCm), 0.0f, 1.0f);
			if (DistanceFactor <= 0.0f)
			{
				continue;
			}

			float& BurnProgress = BurnProgressByActor.FindOrAdd(BurnActor);
			BurnProgress = FMath::Clamp(BurnProgress + DeltaBurn * DistanceFactor, 0.0f, 1.0f);

			ApplyBurnToComponent(PrimitiveComponent, BurnProgress);
			++UpdatedComponents;

			if (bLogSpreadEvents && (SpreadTickCounter % 5 == 0))
			{
				UE_LOG(
					LogTemp,
					Log,
					TEXT("[FireSpread] Actor=%s Component=%s Burn=%.2f Dist=%.1fm"),
					*GetNameSafe(BurnActor),
					*GetNameSafe(PrimitiveComponent),
					BurnProgress,
					DistanceCm / 100.0f
				);
			}

			if (BurnProgress >= IgniteThreshold && !IgnitedActors.Contains(BurnActor))
			{
				if (FMath::FRand() <= IgniteSpawnChance)
				{
					FVector IgniteLocation = PrimitiveComponent->GetComponentLocation();
					FVector ClosestPoint = FVector::ZeroVector;
					if (PrimitiveComponent->GetClosestPointOnCollision(FireLocation, ClosestPoint) > 0.0f)
					{
						IgniteLocation = ClosestPoint;
					}
					else
					{
						IgniteLocation = PrimitiveComponent->Bounds.GetBox().GetClosestPointTo(FireLocation);
					}
					if (SpawnFireAtLocation(IgniteLocation, TEXT("SpreadIgnite")))
					{
						IgnitedActors.Add(BurnActor);
					}
				}
				else
				{
					IgnitedActors.Add(BurnActor);
				}
			}

			if (UpdatedComponents >= MaxBurnedComponentsPerUpdate)
			{
				const double SpreadMs = (FPlatformTime::Seconds() - SpreadStartSec) * 1000.0;
				static double LastSlowSpreadLogSec = 0.0;
				const double NowSec = FPlatformTime::Seconds();
				if (SpreadMs > 25.0 && (NowSec - LastSlowSpreadLogSec) > 2.0)
				{
					LastSlowSpreadLogSec = NowSec;
					UE_LOG(LogTemp, Warning, TEXT("[FireSpread] Slow tick: %.1f ms (ActiveFires=%d UpdatedComponents=%d MaxPerTick=%d)"),
						SpreadMs, ActiveFires.Num(), UpdatedComponents, MaxBurnedComponentsPerUpdate);
				}
				return;
			}
		}
	}
	{
		const double SpreadMs = (FPlatformTime::Seconds() - SpreadStartSec) * 1000.0;
		static double LastSlowSpreadLogSec = 0.0;
		const double NowSec = FPlatformTime::Seconds();
		if (SpreadMs > 25.0 && (NowSec - LastSlowSpreadLogSec) > 2.0)
		{
			LastSlowSpreadLogSec = NowSec;
			UE_LOG(LogTemp, Warning, TEXT("[FireSpread] Slow tick: %.1f ms (ActiveFires=%d UpdatedComponents=%d MaxPerTick=%d)"),
				SpreadMs, ActiveFires.Num(), UpdatedComponents, MaxBurnedComponentsPerUpdate);
		}
	}
}

bool ASimWorldManager::TraceGroundZ(const FVector2D& XY, float& OutGroundZ) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector TraceStart(XY.X, XY.Y, 500000.0f);
	const FVector TraceEnd(XY.X, XY.Y, -500000.0f);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SimWorldGroundTrace), false);
	Params.AddIgnoredActor(const_cast<ASimWorldManager*>(this));
	if (DronePawn)
	{
		Params.AddIgnoredActor(DronePawn);
	}

	FHitResult HitResult;
	const bool bHit =
		World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_Visibility, Params) ||
		World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_WorldStatic, Params);

	if (!bHit)
	{
		return false;
	}

	OutGroundZ = HitResult.ImpactPoint.Z;
	return true;
}

void ASimWorldManager::ForceSpawnFireNow()
{
	UE_LOG(LogTemp, Log, TEXT("[FireSpawn] ForceSpawnFireNow() requested."));
	SpawnFireInCameraFov();
}

bool ASimWorldManager::SpawnFireDebugInFrontOfPlayerOrDrone()
{
	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] F pressed."));

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: world is null."));
		return false;
	}

	CleanupFires();

	AActor* DroneActor = GetDroneActor();
	USceneCaptureComponent2D* DroneCapture = GetDroneSceneCapture();
	const bool bHasDrone = DroneActor != nullptr;
	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Drone exists: %s (Actor=%s Capture=%s)"),
		bHasDrone ? TEXT("true") : TEXT("false"),
		*GetNameSafe(DroneActor),
		*GetNameSafe(DroneCapture));

	FVector TraceStart = FVector::ZeroVector;
	FVector Forward = FVector::ZeroVector;
	FString ViewSource = TEXT("none");

	if (DroneCapture)
	{
		TraceStart = DroneCapture->GetComponentLocation();
		Forward = DroneCapture->GetForwardVector().GetSafeNormal();
		ViewSource = TEXT("Drone.SceneCapture");
	}
	else if (APlayerController* PC = World->GetFirstPlayerController())
	{
		FRotator ViewRotation = FRotator::ZeroRotator;
		PC->GetPlayerViewPoint(TraceStart, ViewRotation);
		Forward = ViewRotation.Vector().GetSafeNormal();
		ViewSource = TEXT("PlayerController.ViewPoint");
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: no SceneCapture and no PlayerController."));
		return false;
	}

	if (Forward.IsNearlyZero())
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: forward vector is nearly zero. Source=%s"), *ViewSource);
		return false;
	}

	if (MaxActiveFires > 0 && ActiveFires.Num() >= MaxActiveFires)
	{
		while (ActiveFires.Num() >= MaxActiveFires)
		{
			AActor* OldestFire = nullptr;
			for (const TWeakObjectPtr<AActor>& FirePtr : ActiveFires)
			{
				if (FirePtr.IsValid())
				{
					OldestFire = FirePtr.Get();
					break;
				}
			}

			if (!OldestFire)
			{
				CleanupFires();
				break;
			}

			UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Fire cap reached (%d). Destroying oldest fire %s for reliable debug spawn."),
				ActiveFires.Num(), *GetNameSafe(OldestFire));
			OldestFire->Destroy();
			CleanupFires();
		}
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FireSpawnDebugLineTrace), false);
	Params.AddIgnoredActor(this);
	if (DroneActor)
	{
		Params.AddIgnoredActor(DroneActor);
	}

	constexpr float ForwardTraceDistanceCm = 200000.0f;
	const FVector TraceEnd = TraceStart + Forward * ForwardTraceDistanceCm;
	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Trace Source=%s Start=%s End=%s Channel=ECC_WorldStatic"),
		*ViewSource, *TraceStart.ToString(), *TraceEnd.ToString());

	FHitResult ForwardHit;
	const bool bForwardHit = World->LineTraceSingleByChannel(ForwardHit, TraceStart, TraceEnd, ECC_WorldStatic, Params);
	if (bForwardHit)
	{
		UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Forward trace hit: Actor=%s Component=%s Impact=%s"),
			*GetNameSafe(ForwardHit.GetActor()),
			*GetNameSafe(ForwardHit.GetComponent()),
			*ForwardHit.ImpactPoint.ToString());

		if (AActor* Spawned = SpawnFireAtLocation(ForwardHit.ImpactPoint, TEXT("DebugF.ForwardLineTraceHit")))
		{
			UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Spawn success: %s at %s"),
				*GetNameSafe(Spawned), *Spawned->GetActorLocation().ToString());
			return true;
		}

		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: SpawnFireAtLocation failed after forward trace hit."));
		return false;
	}

	UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Forward trace miss: no WorldStatic hit. Start=%s End=%s"),
		*TraceStart.ToString(), *TraceEnd.ToString());

	constexpr float FallbackForwardDistanceCm = 2000.0f;
	const FVector FallbackCenter = TraceStart + Forward * FallbackForwardDistanceCm;
	const FVector DownTraceStart(FallbackCenter.X, FallbackCenter.Y, FallbackCenter.Z + 500000.0f);
	const FVector DownTraceEnd(FallbackCenter.X, FallbackCenter.Y, FallbackCenter.Z - 500000.0f);
	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Fallback down trace Start=%s End=%s Channel=ECC_WorldStatic"),
		*DownTraceStart.ToString(), *DownTraceEnd.ToString());

	FHitResult DownHit;
	const bool bDownHit = World->LineTraceSingleByChannel(DownHit, DownTraceStart, DownTraceEnd, ECC_WorldStatic, Params);
	if (!bDownHit)
	{
		UE_LOG(LogTemp, Error, TEXT("[FireSpawnDebug] Failed: fallback down trace missed. Check that landscape/ground blocks WorldStatic."));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Fallback down trace hit: Actor=%s Component=%s Impact=%s"),
		*GetNameSafe(DownHit.GetActor()),
		*GetNameSafe(DownHit.GetComponent()),
		*DownHit.ImpactPoint.ToString());

	if (AActor* Spawned = SpawnFireAtLocation(DownHit.ImpactPoint, TEXT("DebugF.FallbackDownTraceHit")))
	{
		UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] Spawn success (fallback): %s at %s"),
			*GetNameSafe(Spawned), *Spawned->GetActorLocation().ToString());
		return true;
	}

	UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: SpawnFireAtLocation failed after fallback down trace hit."));
	return false;
}

void ASimWorldManager::DebugSpawnFireNow()
{
	ForceSpawnFireNow();
}

void ASimWorldManager::CleanupFires()
{
	ActiveFires.RemoveAll([](const TWeakObjectPtr<AActor>& FirePtr) {
		return !FirePtr.IsValid();
	});

	for (auto It = BurnProgressByActor.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	for (auto It = IgnitedActors.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}

	for (auto It = BurnMIDsByComponent.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void ASimWorldManager::OnFireDestroyed(AActor* DestroyedActor)
{
	if (!DestroyedActor)
	{
		return;
	}

	ActiveFires.RemoveAll([DestroyedActor](const TWeakObjectPtr<AActor>& FirePtr) {
		return !FirePtr.IsValid() || FirePtr.Get() == DestroyedActor;
	});

	if (const AFireSourceActor* Fire = Cast<AFireSourceActor>(DestroyedActor))
	{
		UE_LOG(LogTemp, Log, TEXT("Fire %s extinguished"), *Fire->GetFireId());
	}
}

