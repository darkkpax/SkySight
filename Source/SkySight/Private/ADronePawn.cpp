#include "ADronePawn.h"
#include "AFireSourceActor.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/PointLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "UDroneRouteFollowerComponent.h"
#include "UDroneTelemetryComponent.h"
#include "UDroneSensorComponent.h"
#include "CameraVideoStreamerComponent.h"
#include "ImageUtils.h"
#include "UUnrealBridgeProtocol.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"
#include "TimerManager.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "EngineUtils.h"
#include "Kismet/KismetMathLibrary.h"
#include "PixelFormat.h"
#include "Runtime/Launch/Resources/Version.h"

namespace
{
	static ESceneCaptureSource GetPreferredCaptureSource()
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		return ESceneCaptureSource::SCS_FinalToneCurveHDR;
#else
		return ESceneCaptureSource::SCS_FinalColorLDR;
#endif
	}

	static bool UsesHdrCaptureTarget(const ESceneCaptureSource CaptureSource)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		return CaptureSource == ESceneCaptureSource::SCS_FinalToneCurveHDR;
#else
		return false;
#endif
	}

	static void SpawnTemporaryCaptureDiagnosticCube(UWorld* World)
	{
		if (!World)
		{
			return;
		}

		AFireSourceActor* FirstFire = nullptr;
		for (TActorIterator<AFireSourceActor> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				FirstFire = *It;
				break;
			}
		}

		if (!FirstFire)
		{
			UE_LOG(LogTemp, Warning, TEXT("Capture diagnostic: no AFireSourceActor found for temporary marker cube"));
			return;
		}

		const FVector MarkerLocation = FirstFire->GetActorLocation() + FVector(0.0f, 0.0f, 100.0f);
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AStaticMeshActor* Marker = World->SpawnActor<AStaticMeshActor>(MarkerLocation, FRotator::ZeroRotator, SpawnParams);
		if (!Marker)
		{
			UE_LOG(LogTemp, Warning, TEXT("Capture diagnostic: failed to spawn temporary cube near fire %s"), *FirstFire->GetName());
			return;
		}

		Marker->SetLifeSpan(3.0f);
		Marker->SetActorScale3D(FVector(0.75f));

		if (UStaticMeshComponent* MarkerMesh = Marker->GetStaticMeshComponent())
		{
			if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
			{
				MarkerMesh->SetStaticMesh(CubeMesh);
			}

			MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			MarkerMesh->SetCastShadow(false);
			MarkerMesh->SetHiddenInGame(false, true);
			MarkerMesh->SetVisibility(true, true);

			if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
			{
				MarkerMesh->SetMaterial(0, BaseMaterial);
				if (UMaterialInstanceDynamic* DynMaterial = MarkerMesh->CreateDynamicMaterialInstance(0))
				{
					DynMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(6.0f, 1.4f, 0.1f, 1.0f));
				}
			}
		}

		UPointLightComponent* MarkerLight = NewObject<UPointLightComponent>(Marker, TEXT("CaptureDiagnosticLight"));
		if (MarkerLight)
		{
			MarkerLight->SetupAttachment(Marker->GetRootComponent());
			MarkerLight->SetLightColor(FLinearColor(1.0f, 0.35f, 0.05f));
			MarkerLight->Intensity = 30000.0f;
			MarkerLight->AttenuationRadius = 400.0f;
			MarkerLight->SetCastShadows(false);
			MarkerLight->RegisterComponent();
		}

		UE_LOG(LogTemp, Log, TEXT("Capture diagnostic: spawned temporary marker cube near fire %s for 3s at %s"),
			*FirstFire->GetName(),
			*MarkerLocation.ToString());
	}
}

ADronePawn::ADronePawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	MotionRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MotionRoot"));
	MotionRoot->SetupAttachment(Root);

	VisualRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VisualRoot"));
	VisualRoot->SetupAttachment(MotionRoot);

	ThirdPersonMount = CreateDefaultSubobject<USceneComponent>(TEXT("ThirdPersonMount"));
	ThirdPersonMount->SetupAttachment(Root);

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(VisualRoot);
	BodyMesh->SetCastShadow(false);

	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("MovementComponent"));
	MovementComponent->UpdatedComponent = Root;

	RouteFollower = CreateDefaultSubobject<UDroneRouteFollowerComponent>(TEXT("RouteFollower"));
	TelemetryComponent = CreateDefaultSubobject<UDroneTelemetryComponent>(TEXT("TelemetryComponent"));
	SensorComponent = CreateDefaultSubobject<UDroneSensorComponent>(TEXT("SensorComponent"));

	SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SceneCapture->SetupAttachment(MotionRoot);
	SceneCapture->CaptureSource = GetPreferredCaptureSource();
	SceneCapture->bCaptureEveryFrame = false;
	SceneCapture->bCaptureOnMovement = false;
	SceneCapture->bAutoActivate = true;
	SceneCapture->FOVAngle = 90.0f;
	SceneCapture->SetRelativeLocation(CameraRelLocation);
	SceneCapture->SetRelativeRotation(CameraRelRotation);

	VideoStreamer = CreateDefaultSubobject<UCameraVideoStreamerComponent>(TEXT("VideoStreamer"));

	// Do not allocate render target in ctor; create it in BeginPlay instead.
	CaptureTarget = nullptr;
}

void ADronePawn::BeginPlay()
{
	Super::BeginPlay();

	// Keep actor level: only mesh can bank.
	{
		FRotator R = GetActorRotation();
		R.Pitch = 0.0f;
		R.Roll = 0.0f;
		SetActorRotation(R);
	}

	if (bUseFixedWorldAltitude)
	{
		FVector StartLocation = GetActorLocation();
		StartLocation.Z = FixedWorldAltitudeCm;
		SetActorLocation(StartLocation, false);
	}

	HomeLocationCm = GetActorLocation();
	ApplyMovementSettings();
	PrevLocation = HomeLocationCm;
	bHasPrevLocation = true;
	LastFacingRotation = FRotator(0.0f, GetActorRotation().Yaw, 0.0f);
	bHasFacingRotation = true;
	CurrentBankRoll = 0.0f;

	if (BodyMesh)
	{
		// Keep authored mesh orientation and apply optional forward-axis correction on visual only.
		BodyMeshBaseRotation = BodyMesh->GetRelativeRotation();
		BodyMesh->SetRelativeRotation(BodyMeshBaseRotation + FRotator(0.0f, MeshYawOffsetDeg, 0.0f));
	}

	if (bDisableCollisionForSim)
	{
		SetActorEnableCollision(false);
		if (BodyMesh)
		{
			BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	if (!CaptureTarget)
	{
		CaptureTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CaptureTarget_RT"));
	}

	if (CaptureTarget)
	{
		const ESceneCaptureSource PreferredCaptureSource =
			bCameraUseFinalColorLdr ? ESceneCaptureSource::SCS_FinalColorLDR : GetPreferredCaptureSource();
		const bool bUseHdrTarget = !bCameraUseFinalColorLdr && UsesHdrCaptureTarget(PreferredCaptureSource);

		if (bUseHdrTarget)
		{
			CaptureTarget->RenderTargetFormat = RTF_RGBA16f;
			CaptureTarget->TargetGamma = 1.0f;
			CaptureTarget->InitCustomFormat(CameraWidth, CameraHeight, PF_FloatRGBA, true);
		}
		else
		{
			CaptureTarget->RenderTargetFormat = RTF_RGBA8;
			CaptureTarget->TargetGamma = CameraTargetGamma;
			CaptureTarget->InitCustomFormat(CameraWidth, CameraHeight, PF_B8G8R8A8, false);
		}

		CaptureTarget->ClearColor = FLinearColor::Black;
		CaptureTarget->UpdateResourceImmediate(true);
		CameraReadbackScratch.Reserve(FMath::Max(1, CameraWidth * CameraHeight));

		if (SceneCapture)
		{
			SceneCapture->TextureTarget = CaptureTarget;
			SceneCapture->CaptureSource = PreferredCaptureSource;
			SceneCapture->bAlwaysPersistRenderingState = true;

			// hide ALL components of this actor from the capture, including components added in BP
			SceneCapture->HideActorComponents(this, true);

			TArray<AActor*> Attached;
			GetAttachedActors(Attached);
			for (AActor* A : Attached)
			{
				if (A)
				{
					SceneCapture->HideActorComponents(A, true);
				}
			}

			SceneCapture->ShowFlags = FEngineShowFlags(ESFIM_Game);
			SceneCapture->ShowFlags.SetOnScreenDebug(false);
			SceneCapture->ShowFlags.SetBloom(!bCameraDisableBloom);
			SceneCapture->ShowFlags.SetMotionBlur(!bCameraDisableMotionBlur);
			SceneCapture->ShowFlags.SetAmbientOcclusion(!bCameraDisableAmbientOcclusion);
			SceneCapture->ShowFlags.SetDynamicShadows(false);
			SceneCapture->ShowFlags.SetPostProcessing(false);
			SceneCapture->ShowFlags.SetScreenSpaceReflections(false);
			SceneCapture->ShowFlags.SetVolumetricFog(false);
			SceneCapture->ShowFlags.SetAmbientOcclusion(false);
			SceneCapture->ShowFlags.SetParticles(true);
			SceneCapture->ShowFlags.SetTranslucency(true);
			SceneCapture->ShowFlags.SetSeparateTranslucency(true);
			SceneCapture->LODDistanceFactor = 2.5f;
			SceneCapture->MaxViewDistanceOverride = 30000.0f;
		}
	}

	if (VideoStreamer)
	{
		VideoStreamer->InitializeStreaming(SceneCapture, CaptureTarget);
		UE_LOG(LogTemp, Log, TEXT("Camera JPEG producer active: VideoStreamerComponent"));
	}
	else
	{
		RuntimeCameraJpegQuality = FMath::Clamp(CameraJpegQuality, 1, 100);
		const float EffectiveFps = FMath::Max(1.0f, CameraFps);
		const float Interval = 1.0f / EffectiveFps;
		UpdateCameraCache(); // Prime first frame immediately to avoid startup black response.
		GetWorldTimerManager().SetTimer(CameraTimerHandle, this, &ADronePawn::UpdateCameraCache, Interval, true);
		UE_LOG(LogTemp, Log, TEXT("Camera JPEG producer active: ADronePawn::UpdateCameraCache (%.2f fps, %.3f s)"), EffectiveFps, Interval);
	}

	if (SceneCapture)
	{
		SyncAuthoredCameraMountFromSceneCapture();

		const int32 RtWidth = CaptureTarget ? CaptureTarget->SizeX : 0;
		const int32 RtHeight = CaptureTarget ? CaptureTarget->SizeY : 0;
		const int32 RtRenderTargetFormat = CaptureTarget ? static_cast<int32>(CaptureTarget->RenderTargetFormat) : -1;
		const int32 RtPixelFormat = CaptureTarget ? static_cast<int32>(CaptureTarget->GetFormat()) : -1;
		const float RtGamma = CaptureTarget ? CaptureTarget->TargetGamma : 0.0f;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		const int32 NiagaraFlag = SceneCapture->ShowFlags.Niagara ? 1 : 0;
#else
		const int32 NiagaraFlag = -1;
#endif
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Drone SceneCapture config: CaptureSource=%d RT=%dx%d PixelFormat=%d RtFormat=%d Gamma=%.2f Flags[Mats=%d Light=%d DefLight=%d Trans=%d SepTrans=%d Particles=%d Niagara=%d Post=%d Tone=%d Bloom=%d]"),
			static_cast<int32>(SceneCapture->CaptureSource),
			RtWidth,
			RtHeight,
			RtPixelFormat,
			RtRenderTargetFormat,
			RtGamma,
			SceneCapture->ShowFlags.Materials ? 1 : 0,
			SceneCapture->ShowFlags.Lighting ? 1 : 0,
			SceneCapture->ShowFlags.DeferredLighting ? 1 : 0,
			SceneCapture->ShowFlags.Translucency ? 1 : 0,
			SceneCapture->ShowFlags.SeparateTranslucency ? 1 : 0,
			SceneCapture->ShowFlags.Particles ? 1 : 0,
			NiagaraFlag,
			SceneCapture->ShowFlags.PostProcessing ? 1 : 0,
			SceneCapture->ShowFlags.Tonemapper ? 1 : 0,
			SceneCapture->ShowFlags.Bloom ? 1 : 0
		);
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Drone camera pose: fov_deg=%.2f mount_pitch_deg=%.2f mount_yaw_deg=%.2f mount_roll_deg=%.2f rel_location=%s"),
			SceneCapture->FOVAngle,
			CameraMountPitchDeg,
			CameraMountYawDeg,
			CameraMountRollDeg,
			*CameraRelLocation.ToString()
		);
	}

	SpawnTemporaryCaptureDiagnosticCube(GetWorld());
}

void ADronePawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (RouteFollower)
	{
		CurrentRoute.ActiveIndex = RouteFollower->GetActiveIndex();
	}

	UpdateFacingRotation(DeltaTime);
	MaintainAltitude(DeltaTime);
	if (RouteFollower && RouteFollower->IsRouteComplete())
	{
		FlightMode = TEXT("MISSION_COMPLETE");
	}
	if (bCameraTrackTarget && RouteFollower && RouteFollower->IsRouteComplete())
	{
		ClearCameraTrack();
	}
	if (bCameraTrackTarget && SceneCapture)
	{
		const FVector CameraWorldLocation = SceneCapture->GetComponentLocation();
		const FRotator DesiredWorldRotation = UKismetMathLibrary::FindLookAtRotation(CameraWorldLocation, CameraTrackWorldPoint);
		const FRotator CurrentWorldRotation = SceneCapture->GetComponentRotation();
		const FRotator NewWorldRotation = FMath::RInterpTo(CurrentWorldRotation, DesiredWorldRotation, DeltaTime, CameraTrackInterpSpeed);
		SceneCapture->SetWorldRotation(NewWorldRotation);
	}
	PrevLocation = GetActorLocation();
	bHasPrevLocation = true;
}

void ADronePawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CameraTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void ADronePawn::InitializeForSimulation(const FSkysightGeoReference& Reference, float TelemetryRate, float DetectionRate)
{
	(void)DetectionRate;

	GeoReference = Reference;

	if (TelemetryComponent)
	{
		TelemetryComponent->SetTelemetryRate(TelemetryRate);
		TelemetryComponent->SetUavId(UavId);
	}

	if (SensorComponent)
	{
		SensorComponent->SetComponentTickEnabled(false);
		SensorComponent->Deactivate();
	}
}

void ADronePawn::ConfigureAltitudeHold(float InFixedWorldAltitudeCm, bool bDisableTerrainFollow)
{
	bUseRouteWaypointAltitude = false;
	FixedWorldAltitudeCm = InFixedWorldAltitudeCm;
	bUseFixedWorldAltitude = true;
	if (bDisableTerrainFollow)
	{
		bFollowTerrain = false;
	}

	MaintainAltitude(0.0f);
}

void ADronePawn::SetTerrainFollowEnabled(bool bEnabled)
{
	bUseRouteWaypointAltitude = false;
	bFollowTerrain = bEnabled;
	if (bEnabled)
	{
		bUseFixedWorldAltitude = false;
	}
	MaintainAltitude(0.0f);
}

void ADronePawn::SetDesiredAGLMeters(float InDesiredAGLMeters)
{
	DesiredAGLMeters = FMath::Max(0.0f, InDesiredAGLMeters);
	MaintainAltitude(0.0f);
}

float ADronePawn::GetAltitudeAglMeters() const
{
	float GroundZ = 0.0f;
	const FVector CurrentLocation = GetActorLocation();
	if (TraceGroundZ(CurrentLocation.X, CurrentLocation.Y, GroundZ))
	{
		return FMath::Max(0.0f, (CurrentLocation.Z - GroundZ) / 100.0f);
	}
	return FMath::Max(0.0f, DesiredAGLMeters);
}

void ADronePawn::SetCameraTrackPoint(const FVector& WorldPoint)
{
	if (SceneCapture && !bCameraTrackTarget)
	{
		SyncAuthoredCameraMountFromSceneCapture();
	}
	bCameraTrackTarget = true;
	CameraTrackWorldPoint = WorldPoint;
	UE_LOG(LogTemp, Log, TEXT("[CameraTrack] Enabled. Target=%s"), *CameraTrackWorldPoint.ToString());
}

void ADronePawn::ClearCameraTrack()
{
	bCameraTrackTarget = false;
	if (SceneCapture)
	{
		SceneCapture->SetRelativeRotation(DefaultCameraRelRotation);
	}
	UE_LOG(LogTemp, Log, TEXT("[CameraTrack] Cleared. Restored relative rotation=%s"), *DefaultCameraRelRotation.ToString());
}

void ADronePawn::ApplyMovementSettings()
{
	const float EffectiveMaxSpeedCmPerSec = MaxSpeedCmPerSec * FMath::Max(SpeedScale, 0.01f);

	if (MovementComponent)
	{
		MovementComponent->MaxSpeed = EffectiveMaxSpeedCmPerSec;
		MovementComponent->Acceleration = AccelerationCmPerSec2;
		MovementComponent->Deceleration = DecelerationCmPerSec2;
	}

	if (RouteFollower)
	{
		RouteFollower->MovementSpeedCmPerSec = EffectiveMaxSpeedCmPerSec;
		RouteFollower->AccelerationCmPerSec2 = AccelerationCmPerSec2;
		RouteFollower->DecelerationCmPerSec2 = DecelerationCmPerSec2;
		RouteFollower->bOrientToMovement = false;
		RouteFollower->bSweepMoves = bSweepMoves;
	}
}

void ADronePawn::UpdateFacingRotation(float DeltaTime)
{
	const float PreviousYaw = GetActorRotation().Yaw;
	FVector Velocity = FVector::ZeroVector;

	if (RouteFollower)
	{
		Velocity = RouteFollower->GetLastVelocity();
	}
	else if (MovementComponent)
	{
		Velocity = MovementComponent->Velocity;
	}
	else if (bHasPrevLocation && DeltaTime > KINDA_SMALL_NUMBER)
	{
		Velocity = (GetActorLocation() - PrevLocation) / DeltaTime;
	}

	const float Speed = Velocity.Size();
	float NewYaw = PreviousYaw;
	const FVector FlatVelocity(Velocity.X, Velocity.Y, 0.0f);

	if (DeltaTime > KINDA_SMALL_NUMBER && !FlatVelocity.IsNearlyZero() && Speed >= MinTurnSpeedCmPerSec)
	{
		const float DesiredYaw = FMath::RadiansToDegrees(FMath::Atan2(FlatVelocity.Y, FlatVelocity.X));
		const FRotator Current = GetActorRotation();
		const FRotator Target(0.0f, DesiredYaw, 0.0f);
		const FRotator NewActorRot = FMath::RInterpTo(Current, Target, DeltaTime, TurnInterpSpeed);
		SetActorRotation(FRotator(0.0f, NewActorRot.Yaw, 0.0f));
		LastFacingRotation = FRotator(0.0f, NewActorRot.Yaw, 0.0f);
		bHasFacingRotation = true;
		NewYaw = NewActorRot.Yaw;
	}
	else if (bHasFacingRotation)
	{
		const FRotator CurrentYawOnly(0.0f, GetActorRotation().Yaw, 0.0f);
		SetActorRotation(CurrentYawOnly);
		LastFacingRotation = CurrentYawOnly;
		NewYaw = CurrentYawOnly.Yaw;
	}
	else
	{
		const FRotator CurrentYawOnly(0.0f, GetActorRotation().Yaw, 0.0f);
		SetActorRotation(CurrentYawOnly);
		NewYaw = CurrentYawOnly.Yaw;
	}

	const float SafeDeltaTime = DeltaTime > KINDA_SMALL_NUMBER ? DeltaTime : 1.0f;
	const float YawRateDegPerSec = FMath::FindDeltaAngleDegrees(PreviousYaw, NewYaw) / SafeDeltaTime;
	UpdateBanking(DeltaTime, YawRateDegPerSec);
}

void ADronePawn::UpdateBanking(float DeltaTime, float YawRateDegPerSec)
{
	if (!bEnableBanking || !BodyMesh)
	{
		return;
	}

	const float TargetRoll = FMath::Clamp(YawRateDegPerSec * BankYawRateScale, -MaxBankAngleDeg, MaxBankAngleDeg);

	if (DeltaTime <= KINDA_SMALL_NUMBER || BankInterpSpeed <= KINDA_SMALL_NUMBER)
	{
		CurrentBankRoll = TargetRoll;
	}
	else
	{
		CurrentBankRoll = FMath::FInterpTo(CurrentBankRoll, TargetRoll, DeltaTime, BankInterpSpeed);
	}

	if (bBankMeshOnly)
	{
		const FRotator MeshRot = BodyMeshBaseRotation + FRotator(0.0f, MeshYawOffsetDeg, CurrentBankRoll);
		BodyMesh->SetRelativeRotation(MeshRot);
	}
	else
	{
		const FRotator MeshRot = BodyMeshBaseRotation + FRotator(0.0f, MeshYawOffsetDeg, 0.0f);
		BodyMesh->SetRelativeRotation(MeshRot);
	}
}

void ADronePawn::ApplyRoute(const FSkysightRoute& Route)
{
	ClearCameraTrack();

	CurrentRoute = Route;
	UavId = CurrentRoute.UavId.IsEmpty() ? TEXT("sim") : CurrentRoute.UavId;
	FlightMode = TEXT("IN_FLIGHT");
	bUseRouteWaypointAltitude = CurrentRoute.Waypoints.Num() > 0;

	if (TelemetryComponent)
	{
		TelemetryComponent->SetUavId(UavId);
	}

	if (SensorComponent)
	{
		SensorComponent->SetUavId(UavId);
	}

	TArray<FVector> PointsCm;
	PointsCm.Reserve(CurrentRoute.Waypoints.Num());

	for (const FSkysightWaypoint& Waypoint : CurrentRoute.Waypoints)
	{
		const FVector Base = UUnrealBridgeProtocol::GeoToUnrealCm(
			GeoReference,
			Waypoint.LatitudeDeg,
			Waypoint.LongitudeDeg,
			Waypoint.AltitudeMeters
		);

		FVector Final = Base;

		if (bUseRouteWaypointAltitude)
		{
			Final.Z = Base.Z;
		}
		else if (bUseFixedWorldAltitude)
		{
			Final.Z = FixedWorldAltitudeCm;
		}
		else
		{
			Final.Z = Base.Z + DesiredAGLMeters * 100.0f;
		}

		PointsCm.Add(Final);
	}

	if (RouteFollower)
	{
		RouteFollower->SetRoute(PointsCm, CurrentRoute.ActiveIndex);
		RouteFollower->SetPaused(false);
	}

	ApplyMovementSettings();

	if (PointsCm.Num() > 0)
	{
		const FVector& FirstPoint = PointsCm[0];
		const FVector& LastPoint = PointsCm[PointsCm.Num() - 1];
		UE_LOG(LogTemp, Log, TEXT("[RouteApply] ApplyRoute started: points=%d first=%s last=%s"),
			PointsCm.Num(), *FirstPoint.ToString(), *LastPoint.ToString());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[RouteApply] ApplyRoute received empty route; follower has no points"));
	}
}

bool ADronePawn::TraceGroundZ(float X, float Y, float& OutGroundZ) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const FVector TraceStart(X, Y, 500000.0f);
	const FVector TraceEnd(X, Y, -500000.0f);

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(DroneTerrainFollow), false);
	TraceParams.AddIgnoredActor(this);

	TArray<FHitResult> Hits;
	bool bHit = World->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd, ECC_WorldStatic, TraceParams);
	if (!bHit || Hits.Num() == 0)
	{
		Hits.Reset();
		bHit = World->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd, ECC_Visibility, TraceParams);
		if (!bHit || Hits.Num() == 0)
		{
			return false;
		}
	}

	auto IsLandscapeHit = [](const FHitResult& H) -> bool
	{
		if (H.Component.IsValid())
		{
			const FString CompClass = H.Component->GetClass()->GetName();
			if (CompClass.Contains(TEXT("Landscape"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		if (AActor* A = H.GetActor())
		{
			const FString ActorClass = A->GetClass()->GetName();
			if (ActorClass.Contains(TEXT("Landscape"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	};

	for (const FHitResult& H : Hits)
	{
		if (!H.bBlockingHit)
		{
			continue;
		}
		if (IsLandscapeHit(H))
		{
			OutGroundZ = H.ImpactPoint.Z;
			return true;
		}
	}

	OutGroundZ = Hits.Last().ImpactPoint.Z;
	return true;
}

void ADronePawn::MaintainAltitude(float DeltaTime)
{
	if (bUseRouteWaypointAltitude)
	{
		return;
	}
	const FVector CurrentLocation = GetActorLocation();

	if (bUseFixedWorldAltitude)
	{
		const float TargetZ = FixedWorldAltitudeCm;
		const float NewZ = (DeltaTime <= KINDA_SMALL_NUMBER || TerrainFollowInterpSpeed <= 0.0f)
			? TargetZ
			: FMath::FInterpTo(CurrentLocation.Z, TargetZ, DeltaTime, TerrainFollowInterpSpeed);

		const float Dz = NewZ - CurrentLocation.Z;
		if (FMath::Abs(Dz) < 0.1f)
		{
			return;
		}

		AddActorWorldOffset(FVector(0.0f, 0.0f, Dz), false);
		return;
	}

	if (!bFollowTerrain)
	{
		return;
	}

	float GroundZ = 0.0f;
	if (!TraceGroundZ(CurrentLocation.X, CurrentLocation.Y, GroundZ))
	{
		return;
	}

	const float TargetZ = GroundZ + DesiredAGLMeters * 100.0f;
	const float NewZ = (DeltaTime <= KINDA_SMALL_NUMBER || TerrainFollowInterpSpeed <= 0.0f)
		? TargetZ
		: FMath::FInterpTo(CurrentLocation.Z, TargetZ, DeltaTime, TerrainFollowInterpSpeed);

	const float Dz = NewZ - CurrentLocation.Z;
	if (FMath::Abs(Dz) < 0.1f)
	{
		return;
	}

	AddActorWorldOffset(FVector(0.0f, 0.0f, Dz), false);
}

void ADronePawn::ApplyCommand(const FSkysightCommand& Command)
{
	if (!RouteFollower)
	{
		return;
	}

	switch (Command.Type)
	{
	case ESkysightCommandType::RESET:
		ClearCameraTrack();
		if (RouteFollower->GetWaypoints().Num() > 0)
		{
			RouteFollower->ResetToStart();
			SetActorLocation(RouteFollower->GetWaypoints()[0]);
			CurrentRoute.ActiveIndex = 0;
			FlightMode = TEXT("RESET");
		}
		else
		{
			SetActorLocation(HomeLocationCm);
		}
		break;

	case ESkysightCommandType::HOLD:
		RouteFollower->Pause();
		FlightMode = TEXT("HOLD");
		break;

	case ESkysightCommandType::RESUME:
		RouteFollower->Resume();
		FlightMode = TEXT("IN_FLIGHT");
		break;

	case ESkysightCommandType::RTL:
	{
		ClearCameraTrack();
		double Lat, Lon, Alt;
		UUnrealBridgeProtocol::UnrealCmToGeo(GeoReference, HomeLocationCm, Lat, Lon, Alt);

		FSkysightRoute RtlRoute;
		RtlRoute.UavId = UavId;

		FSkysightWaypoint HomeWaypoint;
		HomeWaypoint.LatitudeDeg = Lat;
		HomeWaypoint.LongitudeDeg = Lon;
		HomeWaypoint.AltitudeMeters = Alt;

		RtlRoute.Waypoints.Add(HomeWaypoint);
		RtlRoute.ActiveIndex = 0;

		ApplyRoute(RtlRoute);
		FlightMode = TEXT("RTL");
		break;
	}

	case ESkysightCommandType::SET_VELOCITY:
	{
		const FVector VelocityCm = FVector(Command.VxMps, Command.VyMps, Command.VzMps) * 100.0f;
		RouteFollower->SetVelocityOverride(VelocityCm);
		FlightMode = TEXT("SET_VELOCITY");
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Velocity override set (m/s): vx=%.3f vy=%.3f vz=%.3f"),
			Command.VxMps,
			Command.VyMps,
			Command.VzMps
		);
		break;
	}

	case ESkysightCommandType::SET_SPEED:
	{
		const float SpeedMps = FMath::Max(0.0f, Command.SpeedMps);
		const FVector Forward = GetActorForwardVector();
		const FVector VelocityCm = Forward * (SpeedMps * 100.0f);
		RouteFollower->SetVelocityOverride(VelocityCm);
		FlightMode = TEXT("SET_SPEED");
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Speed override set (m/s): speed=%.3f forward=(%.3f, %.3f, %.3f)"),
			SpeedMps,
			Forward.X,
			Forward.Y,
			Forward.Z
		);
		break;
	}

	case ESkysightCommandType::CLEAR_VELOCITY_OVERRIDE:
	{
		RouteFollower->ClearVelocityOverride();
		FlightMode = TEXT("IN_FLIGHT");
		UE_LOG(LogTemp, Log, TEXT("Velocity override cleared"));
		break;
	}

	case ESkysightCommandType::ORBIT_START:
		if (Command.bHasTargetWorldPoint)
		{
			SetCameraTrackPoint(Command.TargetWorldPointCm);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraTrack] ORBIT_START received without target point."));
		}
		FlightMode = TEXT("ORBIT");
		break;

	case ESkysightCommandType::ORBIT_STOP:
		ClearCameraTrack();
		FlightMode = TEXT("IN_FLIGHT");
		break;

	case ESkysightCommandType::SET_MODE:
		if (Command.bClearCameraTrack)
		{
			ClearCameraTrack();
		}
		if (Command.bHasTargetWorldPoint)
		{
			SetCameraTrackPoint(Command.TargetWorldPointCm);
		}
		FlightMode = Command.ModeName.IsEmpty() ? TEXT("MODE") : Command.ModeName;
		break;

	default:
		break;
	}

	UE_LOG(LogTemp, Log, TEXT("Applied command %d for drone %s"), static_cast<int32>(Command.Type), *UavId);
}

bool ADronePawn::IsRouteComplete() const
{
	return RouteFollower ? RouteFollower->IsRouteComplete() : false;
}

const FSkysightTelemetry& ADronePawn::GetLatestTelemetry() const
{
	static const FSkysightTelemetry EmptyTelemetry;
	return TelemetryComponent ? TelemetryComponent->GetLatestTelemetry() : EmptyTelemetry;
}

const FDetectionsBatch& ADronePawn::GetLatestDetections() const
{
	static const FDetectionsBatch EmptyBatch;
	return EmptyBatch;
}

bool ADronePawn::GetCameraFrameBytes(TArray<uint8>& OutBytes) const
{
	if (VideoStreamer)
	{
		return VideoStreamer->GetLatestJpeg(OutBytes);
	}

	FScopeLock Lock(&CameraMutex);
	if (CachedJpeg.Num() == 0)
	{
		return false;
	}

	OutBytes = CachedJpeg;
	return true;
}

float ADronePawn::GetCameraFovDeg() const
{
	return SceneCapture ? SceneCapture->FOVAngle : 0.0f;
}

float ADronePawn::GetCameraMountPitchDeg() const
{
	return GetCameraRelativeRotation().Pitch;
}

float ADronePawn::GetCameraMountYawDeg() const
{
	return GetCameraRelativeRotation().Yaw;
}

float ADronePawn::GetCameraMountRollDeg() const
{
	return GetCameraRelativeRotation().Roll;
}

FRotator ADronePawn::GetCameraRelativeRotation() const
{
	return SceneCapture ? SceneCapture->GetRelativeRotation() : CameraRelRotation;
}

FRotator ADronePawn::GetCameraBaseRelativeRotation() const
{
	return DefaultCameraRelRotation;
}

FVector ADronePawn::GetCameraRelativeLocation() const
{
	return SceneCapture ? SceneCapture->GetRelativeLocation() : CameraRelLocation;
}

FIntPoint ADronePawn::GetCameraResolution() const
{
	if (CaptureTarget)
	{
		return FIntPoint(CaptureTarget->SizeX, CaptureTarget->SizeY);
	}

	return FIntPoint(CameraWidth, CameraHeight);
}

float ADronePawn::GetCameraAspectRatio() const
{
	const FIntPoint Resolution = GetCameraResolution();
	return Resolution.Y > 0 ? static_cast<float>(Resolution.X) / static_cast<float>(Resolution.Y) : 0.0f;
}

void ADronePawn::SyncAuthoredCameraMountFromSceneCapture()
{
	if (!SceneCapture)
	{
		return;
	}

	CameraRelLocation = SceneCapture->GetRelativeLocation();
	CameraRelRotation = SceneCapture->GetRelativeRotation();
	DefaultCameraRelRotation = CameraRelRotation;
	CameraMountPitchDeg = CameraRelRotation.Pitch;
	CameraMountYawDeg = CameraRelRotation.Yaw;
	CameraMountRollDeg = CameraRelRotation.Roll;
}

void ADronePawn::UpdateCameraCache()
{
	if (!SceneCapture || !CaptureTarget)
	{
		return;
	}

	if (RuntimeCameraJpegQuality <= 0)
	{
		RuntimeCameraJpegQuality = FMath::Clamp(CameraJpegQuality, 1, 100);
	}

	const double FrameStartSec = FPlatformTime::Seconds();
	SceneCapture->CaptureScene();
	const double AfterCaptureSec = FPlatformTime::Seconds();

	FTextureRenderTargetResource* RenderTargetResource = CaptureTarget->GameThread_GetRenderTargetResource();
	if (!RenderTargetResource)
	{
		return;
	}

	CameraReadbackScratch.Reset();
	if (!RenderTargetResource->ReadPixels(CameraReadbackScratch) || CameraReadbackScratch.Num() == 0)
	{
		return;
	}
	const double AfterReadbackSec = FPlatformTime::Seconds();

	IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = WrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!ImageWrapper.IsValid())
	{
		return;
	}

	const int32 Width = CaptureTarget->SizeX;
	const int32 Height = CaptureTarget->SizeY;
	const int32 RawBytes = CameraReadbackScratch.Num() * sizeof(FColor);

	if (!ImageWrapper->SetRaw(CameraReadbackScratch.GetData(), RawBytes, Width, Height, ERGBFormat::BGRA, 8))
	{
		return;
	}

	const int32 EffectiveJpegQuality = FMath::Clamp(RuntimeCameraJpegQuality, 1, 100);
	const TArray64<uint8> Compressed64 = ImageWrapper->GetCompressed(EffectiveJpegQuality);
	if (Compressed64.Num() == 0)
	{
		return;
	}

	TArray<uint8> NewJpeg;
	NewJpeg.SetNumUninitialized(static_cast<int32>(Compressed64.Num()));
	FMemory::Memcpy(NewJpeg.GetData(), Compressed64.GetData(), Compressed64.Num());
	const double AfterJpegSec = FPlatformTime::Seconds();

	bool bLogFirstFrame = false;
	int32 CachedSize = 0;
	{
		FScopeLock Lock(&CameraMutex);
		bLogFirstFrame = CachedJpeg.Num() == 0;
		CachedJpeg = MoveTemp(NewJpeg);
		CachedSize = CachedJpeg.Num();
	}

	if (bLogFirstFrame)
	{
		UE_LOG(LogTemp, Log, TEXT("Camera cache primed (%d bytes)"), CachedSize);
	}

	const double CaptureMs = (AfterCaptureSec - FrameStartSec) * 1000.0;
	const double ReadbackMs = (AfterReadbackSec - AfterCaptureSec) * 1000.0;
	const double JpegMs = (AfterJpegSec - AfterReadbackSec) * 1000.0;
	const double TotalMs = (AfterJpegSec - FrameStartSec) * 1000.0;
	const double TargetFrameMs = 1000.0 / FMath::Max(1.0f, CameraFps);

	CameraPerfAccumCaptureMs += CaptureMs;
	CameraPerfAccumReadbackMs += ReadbackMs;
	CameraPerfAccumJpegMs += JpegMs;
	CameraPerfAccumTotalMs += TotalMs;
	++CameraPerfAccumFrames;

	if (bCameraAutoAdjustJpegQuality)
	{
		const int32 BaseQuality = FMath::Clamp(CameraJpegQuality, 1, 100);
		const int32 MinQuality = FMath::Min(BaseQuality, FMath::Clamp(CameraMinJpegQuality, 1, 100));
		const bool bOverBudget = TotalMs > (TargetFrameMs * 1.10);
		const bool bUnderBudget = TotalMs < (TargetFrameMs * 0.75);

		if (bOverBudget)
		{
			++CameraOverBudgetFrameCount;
			CameraUnderBudgetFrameCount = 0;

			if (CameraOverBudgetFrameCount >= 12 && RuntimeCameraJpegQuality > MinQuality)
			{
				const int32 PreviousQuality = RuntimeCameraJpegQuality;
				RuntimeCameraJpegQuality = FMath::Max(MinQuality, RuntimeCameraJpegQuality - 5);
				CameraOverBudgetFrameCount = 0;
				UE_LOG(LogTemp, Warning,
					TEXT("Camera JPEG quality reduced %d -> %d to sustain %.1f fps (frame %.2f ms, budget %.2f ms, %dx%d)"),
					PreviousQuality, RuntimeCameraJpegQuality, CameraFps, TotalMs, TargetFrameMs, Width, Height);
			}
		}
		else if (bUnderBudget)
		{
			++CameraUnderBudgetFrameCount;
			CameraOverBudgetFrameCount = 0;

			if (CameraUnderBudgetFrameCount >= 60 && RuntimeCameraJpegQuality < BaseQuality)
			{
				const int32 PreviousQuality = RuntimeCameraJpegQuality;
				RuntimeCameraJpegQuality = FMath::Min(BaseQuality, RuntimeCameraJpegQuality + 5);
				CameraUnderBudgetFrameCount = 0;
				UE_LOG(LogTemp, Log,
					TEXT("Camera JPEG quality restored %d -> %d (frame %.2f ms, budget %.2f ms)"),
					PreviousQuality, RuntimeCameraJpegQuality, TotalMs, TargetFrameMs);
			}
		}
		else
		{
			CameraOverBudgetFrameCount = 0;
			CameraUnderBudgetFrameCount = 0;
		}
	}

	const double NowSec = AfterJpegSec;
	if (CameraPerfLogIntervalSec > 0.0 && (NowSec - CameraPerfLastLogSeconds) >= CameraPerfLogIntervalSec && CameraPerfAccumFrames > 0)
	{
		const double InvCount = 1.0 / static_cast<double>(CameraPerfAccumFrames);
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Camera JPEG perf avg over %d frames: capture=%.2fms readback=%.2fms jpeg=%.2fms total=%.2fms (target %.2fms @ %.1f fps, q=%d, %dx%d)"),
			CameraPerfAccumFrames,
			CameraPerfAccumCaptureMs * InvCount,
			CameraPerfAccumReadbackMs * InvCount,
			CameraPerfAccumJpegMs * InvCount,
			CameraPerfAccumTotalMs * InvCount,
			TargetFrameMs,
			CameraFps,
			EffectiveJpegQuality,
			Width,
			Height);

		CameraPerfLastLogSeconds = NowSec;
		CameraPerfAccumCaptureMs = 0.0;
		CameraPerfAccumReadbackMs = 0.0;
		CameraPerfAccumJpegMs = 0.0;
		CameraPerfAccumTotalMs = 0.0;
		CameraPerfAccumFrames = 0;
	}
}
