#include "UDroneRouteFollowerComponent.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"

UDroneRouteFollowerComponent::UDroneRouteFollowerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UDroneRouteFollowerComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UDroneRouteFollowerComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AActor* Owner = GetOwner();
	if (!Owner || Waypoints.Num() == 0)
	{
		LastVelocity = FVector::ZeroVector;
		SmoothedMoveDirection = FVector::ZeroVector;
		CurrentSpeedCmPerSec = 0.0f;
		return;
	}

	if (bPaused)
	{
		LastVelocity = FVector::ZeroVector;
		SmoothedMoveDirection = FVector::ZeroVector;
		CurrentSpeedCmPerSec = 0.0f;
		return;
	}

	// HOLD/paused state is checked above and always wins over overrides.
	// When not paused, velocity override takes precedence over route-following velocity.
	if (bUseVelocityOverride)
	{
		const FVector Move = VelocityOverride * DeltaTime;
		const FVector BeforeMove = Owner->GetActorLocation();
		Owner->AddActorWorldOffset(Move, bSweepMoves);
		const FVector ActualMove = Owner->GetActorLocation() - BeforeMove;
		LastVelocity = DeltaTime > KINDA_SMALL_NUMBER ? ActualMove / DeltaTime : FVector::ZeroVector;
		CurrentSpeedCmPerSec = VelocityOverride.Size();
		SmoothedMoveDirection = LastVelocity.GetSafeNormal();

		if (bOrientToMovement && LastVelocity.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			const FVector Dir = LastVelocity.GetSafeNormal();
			const FRotator TargetRot = Dir.Rotation();
			const FRotator NewRot = FMath::RInterpTo(Owner->GetActorRotation(), TargetRot, DeltaTime, RotationInterpSpeed);
			Owner->SetActorRotation(NewRot);
		}
		return;
	}

	const FVector CurrentLocation = Owner->GetActorLocation();
	FVector Target = GetCurrentTarget();
	FVector Direction = Target - CurrentLocation;
	float Distance = Direction.Size();

	if (Distance <= AcceptanceRadiusCm)
	{
		AdvanceToNextWaypoint();
		if (bPaused)
		{
			LastVelocity = FVector::ZeroVector;
			SmoothedMoveDirection = FVector::ZeroVector;
			CurrentSpeedCmPerSec = 0.0f;
			return;
		}

		const FVector UpdatedLocation = Owner->GetActorLocation();
		Target = GetCurrentTarget();
		Direction = Target - UpdatedLocation;
		Distance = Direction.Size();
	}

	if (Distance <= KINDA_SMALL_NUMBER || bPaused)
	{
		LastVelocity = FVector::ZeroVector;
		SmoothedMoveDirection = FVector::ZeroVector;
		CurrentSpeedCmPerSec = 0.0f;
		return;
	}

	const FVector DirNorm = Direction.GetSafeNormal();
	if (SmoothedMoveDirection.IsNearlyZero())
	{
		SmoothedMoveDirection = DirNorm;
	}
	else if (DirectionSmoothingSpeed > KINDA_SMALL_NUMBER)
	{
		const float BlendAlpha = FMath::Clamp(DirectionSmoothingSpeed * DeltaTime, 0.0f, 1.0f);
		SmoothedMoveDirection = FMath::Lerp(SmoothedMoveDirection, DirNorm, BlendAlpha).GetSafeNormal();
	}
	else
	{
		SmoothedMoveDirection = DirNorm;
	}

	const float MaxSpeed = FMath::Max(0.0f, MovementSpeedCmPerSec);
	const float Accel = FMath::Max(0.0f, AccelerationCmPerSec2);
	const float Decel = FMath::Max(0.0f, DecelerationCmPerSec2);

	float TargetSpeed = MaxSpeed;
	if (Decel > KINDA_SMALL_NUMBER)
	{
		const float MaxSpeedForDistance = FMath::Sqrt(2.0f * Decel * Distance);
		TargetSpeed = FMath::Min(TargetSpeed, MaxSpeedForDistance);
	}

	if (CurrentSpeedCmPerSec < TargetSpeed)
	{
		CurrentSpeedCmPerSec = FMath::Min(CurrentSpeedCmPerSec + Accel * DeltaTime, TargetSpeed);
	}
	else
	{
		CurrentSpeedCmPerSec = FMath::Max(CurrentSpeedCmPerSec - Decel * DeltaTime, TargetSpeed);
	}

	const float StepDistance = FMath::Min(CurrentSpeedCmPerSec * DeltaTime, Distance);
	FVector Step = SmoothedMoveDirection * StepDistance;

	// sub-stepping to avoid visible "teleport" jumps
	if (MaxStepPerTickCm > 0.0f)
	{
		const float MaxStep = FMath::Max(1.0f, MaxStepPerTickCm);
		const int32 NumSub = FMath::Clamp((int32)FMath::CeilToInt(StepDistance / MaxStep), 1, 20);
		const FVector SubStep = Step / (float)NumSub;

		const FVector BeforeMove = Owner->GetActorLocation();
		for (int32 i = 0; i < NumSub; ++i)
		{
			Owner->AddActorWorldOffset(SubStep, bSweepMoves);
		}
		const FVector ActualMove = Owner->GetActorLocation() - BeforeMove;
		LastVelocity = DeltaTime > KINDA_SMALL_NUMBER ? ActualMove / DeltaTime : FVector::ZeroVector;
	}
	else
	{
		const FVector BeforeMove = Owner->GetActorLocation();
		Owner->AddActorWorldOffset(Step, bSweepMoves);
		const FVector ActualMove = Owner->GetActorLocation() - BeforeMove;
		LastVelocity = DeltaTime > KINDA_SMALL_NUMBER ? ActualMove / DeltaTime : FVector::ZeroVector;
	}

	if (bOrientToMovement && LastVelocity.SizeSquared() > KINDA_SMALL_NUMBER)
	{
		const FVector Dir = LastVelocity.GetSafeNormal();
		const FRotator TargetRot = Dir.Rotation();
		const FRotator NewRot = FMath::RInterpTo(Owner->GetActorRotation(), TargetRot, DeltaTime, RotationInterpSpeed);
		Owner->SetActorRotation(NewRot);
	}
}

void UDroneRouteFollowerComponent::SetRoute(const TArray<FVector>& InRoute, int32 StartIndex)
{
	Waypoints = InRoute;
	bRouteComplete = false;
	if (Waypoints.Num() == 0)
	{
		bPaused = true;
		ActiveIndex = 0;
		LastVelocity = FVector::ZeroVector;
		SmoothedMoveDirection = FVector::ZeroVector;
		CurrentSpeedCmPerSec = 0.0f;
		return;
	}

	ActiveIndex = FMath::Clamp(StartIndex, 0, Waypoints.Num() - 1);
	bPaused = false;
	bUseVelocityOverride = false;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::ClearRoute()
{
	Waypoints.Empty();
	ActiveIndex = 0;
	bPaused = true;
	bUseVelocityOverride = false;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::Pause()
{
	bPaused = true;
	LastVelocity = FVector::ZeroVector;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::Resume()
{
	if (Waypoints.Num() > 0)
	{
		bPaused = false;
		SmoothedMoveDirection = FVector::ZeroVector;
	}
}

void UDroneRouteFollowerComponent::SetPaused(bool bInPaused)
{
	if (bInPaused)
	{
		Pause();
	}
	else
	{
		Resume();
	}
}

bool UDroneRouteFollowerComponent::IsPaused() const
{
	return bPaused;
}

bool UDroneRouteFollowerComponent::IsRouteComplete() const
{
	return bRouteComplete;
}

int32 UDroneRouteFollowerComponent::GetActiveIndex() const
{
	return ActiveIndex;
}

FVector UDroneRouteFollowerComponent::GetCurrentTarget() const
{
	if (Waypoints.IsValidIndex(ActiveIndex))
	{
		return Waypoints[ActiveIndex];
	}

	return FVector::ZeroVector;
}

void UDroneRouteFollowerComponent::ResetToStart()
{
	if (Waypoints.Num() == 0)
	{
		return;
	}

	ActiveIndex = 0;
	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(Waypoints[0]);
	}

	bPaused = false;
	bRouteComplete = false;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::ResetToIndex(int32 Index)
{
	if (!Waypoints.IsValidIndex(Index))
	{
		return;
	}

	ActiveIndex = Index;
	if (AActor* Owner = GetOwner())
	{
		Owner->SetActorLocation(Waypoints[Index]);
	}

	bPaused = false;
	bRouteComplete = false;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::SetVelocityOverride(const FVector& VelocityCmPerSec)
{
	VelocityOverride = VelocityCmPerSec;
	bUseVelocityOverride = true;
	SmoothedMoveDirection = VelocityOverride.GetSafeNormal();
	CurrentSpeedCmPerSec = VelocityOverride.Size();
}

void UDroneRouteFollowerComponent::ClearVelocityOverride()
{
	bUseVelocityOverride = false;
	LastVelocity = FVector::ZeroVector;
	SmoothedMoveDirection = FVector::ZeroVector;
	CurrentSpeedCmPerSec = 0.0f;
}

void UDroneRouteFollowerComponent::AdvanceToNextWaypoint()
{
	if (ActiveIndex + 1 < Waypoints.Num())
	{
		ActiveIndex++;
		bRouteComplete = false;
	}
	else
	{
		bPaused = true;
		bRouteComplete = true;
		SmoothedMoveDirection = FVector::ZeroVector;
		CurrentSpeedCmPerSec = 0.0f;
	}
}
