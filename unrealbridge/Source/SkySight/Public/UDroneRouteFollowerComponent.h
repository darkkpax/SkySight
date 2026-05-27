#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UDroneRouteFollowerComponent.generated.h"

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SKYSIGHT_API UDroneRouteFollowerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDroneRouteFollowerComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void SetRoute(const TArray<FVector>& InRoute, int32 StartIndex = 0);
	void ClearRoute();
	void Pause();
	void Resume();
	void SetPaused(bool bInPaused);

	bool IsPaused() const;
	bool IsRouteComplete() const;
	int32 GetActiveIndex() const;
	FVector GetCurrentTarget() const;
	void ResetToStart();
	void ResetToIndex(int32 Index);

	const TArray<FVector>& GetWaypoints() const { return Waypoints; }
	FVector GetLastVelocity() const { return LastVelocity; }

	void SetVelocityOverride(const FVector& InVelocityCm);
	void ClearVelocityOverride();

public:
	UPROPERTY(EditAnywhere, Category="Route")
	float MovementSpeedCmPerSec = 1200.0f;

	UPROPERTY(EditAnywhere, Category="Route")
	float AccelerationCmPerSec2 = 1500.0f;

	UPROPERTY(EditAnywhere, Category="Route")
	float DecelerationCmPerSec2 = 1800.0f;

	UPROPERTY(EditAnywhere, Category="Route")
	float AcceptanceRadiusCm = 60.0f;

	UPROPERTY(EditAnywhere, Category="Route")
	float DirectionSmoothingSpeed = 5.0f;

	UPROPERTY(EditAnywhere, Category="Route")
	float MaxStepPerTickCm = 50.0f; // 0 = disable

	// Actor rotation is handled in ADronePawn::UpdateFacingRotation.
	UPROPERTY(EditAnywhere, Category="Route")
	bool bOrientToMovement = false;

	// Sweep can cause jitter against trees/foliage in sim mode.
	UPROPERTY(EditAnywhere, Category="Route")
	bool bSweepMoves = false;

	UPROPERTY(EditAnywhere, Category="Route")
	float RotationInterpSpeed = 4.0f;

private:
	void AdvanceToNextWaypoint();

private:
	UPROPERTY()
	TArray<FVector> Waypoints;

	UPROPERTY()
	int32 ActiveIndex = 0;

	UPROPERTY()
	bool bPaused = true;

	UPROPERTY()
	FVector LastVelocity = FVector::ZeroVector;

	UPROPERTY()
	FVector SmoothedMoveDirection = FVector::ZeroVector;

	UPROPERTY()
	float CurrentSpeedCmPerSec = 0.0f;

	// velocity override
	UPROPERTY()
	bool bUseVelocityOverride = false;

	UPROPERTY()
	FVector VelocityOverride = FVector::ZeroVector;

	UPROPERTY()
	bool bRouteComplete = false;
};
