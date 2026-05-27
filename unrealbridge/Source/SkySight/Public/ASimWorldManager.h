#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UUnrealBridgeProtocol.h"
#include "ASimWorldManager.generated.h"

class ADronePawn;
class AFireSourceActor;
class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UPrimitiveComponent;
class USceneCaptureComponent2D;

UCLASS()
class SKYSIGHT_API ASimWorldManager : public AActor
{
	GENERATED_BODY()

public:
	ASimWorldManager();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	const FSkysightGeoReference& GetGeoReference() const { return GeoReference; }
	const TArray<AFireSourceActor*>& GetActiveFires() const;
	ADronePawn* GetDronePawn() const { return DronePawn; }
	void SpawnDrone();
	bool SpawnDroneAtWorldXY(const FVector2D& SpawnXYCm);
	bool SpawnDroneAtGeo(double LatitudeDeg, double LongitudeDeg, double AltitudeMeters = 0.0);
	bool ApplyRouteToDrone(const FSkysightRoute& Route);
	bool DespawnDrone();
	UFUNCTION(BlueprintCallable, Category="Fire|Spawn")
	void ForceSpawnFireNow();

	UFUNCTION(BlueprintCallable, Category="Fire|Spawn")
	bool SpawnFireDebugInFrontOfPlayerOrDrone();

	UFUNCTION(Exec)
	void DebugSpawnFireNow();

	UPROPERTY(EditAnywhere, Category="Geo")
	double OriginLatDeg = 47.6062;

	UPROPERTY(EditAnywhere, Category="Geo")
	double OriginLonDeg = -122.3321;

	UPROPERTY(EditAnywhere, Category="Geo")
	double OriginAltMeters = 0.0;

	UPROPERTY(EditAnywhere, Category="Geo")
	float MetersToUnrealCm = 100.0f;

	UPROPERTY(EditAnywhere, Category="Sim")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, Category="Sim")
	float TelemetryHz = 10.0f;

	UPROPERTY(EditAnywhere, Category="Sim")
	float DetectionHz = 2.0f;

	UPROPERTY(EditAnywhere, Category="Sim|Drone")
	float DroneSpawnWorldZ = -4000.0f;

	// Desired altitude above ground at spawn (meters). Used to compute a fixed world-Z above canopy.
	UPROPERTY(EditAnywhere, Category="Sim|Drone")
	float SpawnAGLMeters = 60.0f;

	UPROPERTY(EditAnywhere, Category="Sim|Drone|Altitude")
	bool bUseFixedWorldAltitude = false;

	UPROPERTY(EditAnywhere, Category="Sim|Drone|Altitude", meta = (ClampMin = "0.0"))
	float DesiredAGLMeters = 30.0f;

	// Fire spawning
	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	bool bAutoSpawnFires = true;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	float FireSpawnIntervalSec = 3.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	int32 MaxActiveFires = 5;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	bool bPersistentFires = true;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	float FireSpawnMinDistanceMeters = 150.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	float FireSpawnMaxDistanceMeters = 350.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	float FireSpawnHalfFovDeg = 25.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	float FireSpawnMinSeparation = 150.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	TSubclassOf<AActor> FireActorClassOverride;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	bool bDebugFireSpawns = false;

	UPROPERTY(EditAnywhere, Category="Debug|Fire")
	bool bDebugFireSpawn = true;

	UPROPERTY(EditAnywhere, Category="Debug|Fire")
	float DebugDrawTime = 8.0f;

	UPROPERTY(EditAnywhere, Category="Debug|Fire")
	float DebugLineThickness = 5.0f;

	UPROPERTY(EditAnywhere, Category="Debug|Fire")
	bool bDebugPersistent = false;

	UPROPERTY(EditAnywhere, Category="Fire Spawning")
	float FireSpawnSphereRadius = 2500.0f;

	UPROPERTY(EditAnywhere, Category="Fire Spawning")
	float DroneDownTraceLength = 200000.0f;

	UPROPERTY(EditAnywhere, Category="Fire Spawning")
	int32 MaxSphereCenterAttempts = 3;

	UPROPERTY(EditAnywhere, Category="Fire Spawning")
	int32 MaxPointAttemptsInSphere = 12;

	UPROPERTY(EditAnywhere, Category="Sim")
	TSubclassOf<ADronePawn> DroneClass;

	UPROPERTY(EditAnywhere, Category="Fire|Spawn")
	TSubclassOf<AFireSourceActor> FireSourceClass;

	// Fire spread and burn visuals (C++ logic, assets assigned in editor)
	UPROPERTY(EditAnywhere, Category="Fire|Spread")
	bool bEnableFireSpread = true;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="0.1"))
	float FireSpreadUpdateIntervalSec = 1.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="1.0"))
	float FireAffectRadiusMeters = 20.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="0.0"))
	float BurnAccumulationPerSecond = 0.35f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="0.01"))
	float IgniteThreshold = 1.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="0.0", ClampMax="1.0"))
	float IgniteSpawnChance = 0.65f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="0.0"))
	float SpreadSpawnZOffset = 50.0f;

	UPROPERTY(EditAnywhere, Category="Fire|Spread")
	bool bRequireBurnableTag = false;

	UPROPERTY(EditAnywhere, Category="Fire|Spread")
	FName BurnableTag = TEXT("Burnable");

	UPROPERTY(EditAnywhere, Category="Fire|Spread", meta=(ClampMin="1"))
	int32 MaxBurnedComponentsPerUpdate = 64;

	UPROPERTY(EditAnywhere, Category="Fire|Spread")
	FName BurnAmountParameterName = TEXT("BurnAmount");

	UPROPERTY(EditAnywhere, Category="Fire|Spread")
	bool bLogSpreadEvents = false;

	// Optional: if your terrain shader uses MPC for burn masks, assign it here.
	UPROPERTY(EditAnywhere, Category="Fire|Spread|Ground")
	UMaterialParameterCollection* GroundBurnMPC = nullptr;

	UPROPERTY(EditAnywhere, Category="Fire|Spread|Ground")
	FName GroundBurnCenterXParam = TEXT("BurnCenterX");

	UPROPERTY(EditAnywhere, Category="Fire|Spread|Ground")
	FName GroundBurnCenterYParam = TEXT("BurnCenterY");

	UPROPERTY(EditAnywhere, Category="Fire|Spread|Ground")
	FName GroundBurnRadiusParam = TEXT("BurnRadius");

	UPROPERTY(EditAnywhere, Category="Fire|Spread|Ground")
	FName GroundBurnIntensityParam = TEXT("BurnIntensity");

protected:
	FSkysightGeoReference GeoReference;
	ADronePawn* DronePawn = nullptr;
	TArray<TWeakObjectPtr<AActor>> ActiveFires;
	mutable TArray<AFireSourceActor*> ActiveFireSourcesCache;
	FTimerHandle FireSpawnTimerHandle;
	FTimerHandle FireSpreadTimerHandle;
	int32 FireIndexCounter = 1;
	TMap<TWeakObjectPtr<AActor>, float> BurnProgressByActor;
	TSet<TWeakObjectPtr<AActor>> IgnitedActors;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TArray<TObjectPtr<UMaterialInstanceDynamic>>> BurnMIDsByComponent;
	int32 SpreadTickCounter = 0;

	void CleanupFires();
	void SpawnFireInCameraFov();
	void UpdateFireSpread();
	AActor* SpawnFireAtLocation(const FVector& Location, const TCHAR* Reason);
	void ApplyBurnToComponent(UPrimitiveComponent* PrimitiveComponent, float BurnAmount);
	bool IsComponentBurnable(const UPrimitiveComponent* PrimitiveComponent) const;
	void UpdateGroundBurnMPC(const FVector& FireLocation, float RadiusCm, float Intensity) const;
	AActor* GetDroneActor() const;
	USceneCaptureComponent2D* GetDroneSceneCapture() const;
	bool TraceGroundZ(const FVector2D& XY, float& OutGroundZ) const;
	bool SphereTraceToGroundFromCamera(FHitResult& OutHit, FVector& OutCamLoc, FVector& OutStart, FVector& OutEnd) const;
	bool IsDroneOverSpawnSphere(const FVector& SphereCenter, FHitResult* OutDroneDownHit) const;
	bool FindPointInsideSpawnSphereOnGround(const FVector& SphereCenter, FVector& OutLocation) const;
	void DrawFireSpawnDebug(const FVector& CamLoc, const FVector& Start, const FVector& End, const FHitResult& SphereHit, const FVector& SphereCenter, const FVector& ChosenSpawn, bool bDroneBlocks, const FHitResult& DroneDownHit) const;

	UFUNCTION()
	void OnFireDestroyed(AActor* DestroyedActor);
};
