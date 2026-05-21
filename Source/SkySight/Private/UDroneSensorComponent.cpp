#include "UDroneSensorComponent.h"
#include "ADronePawn.h"
#include "ASimWorldManager.h"
#include "AFireSourceActor.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	bool bLoggedUnrealSensorDisabled = false;
}

UDroneSensorComponent::UDroneSensorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UDroneSensorComponent::BeginPlay()
{
	Super::BeginPlay();

	// HARD DISABLE: Unreal-side detections are not used in this project.
	// Detections must come from Python ML model only.
	SetComponentTickEnabled(false);
	Deactivate();
	DetectionHz = 0.0f;
	SamplingTimer = 0.0f;
	LatestBatch.Detections.Empty();

	if (!bLoggedUnrealSensorDisabled)
	{
		UE_LOG(LogTemp, Warning, TEXT("Unreal sensor disabled by build"));
		bLoggedUnrealSensorDisabled = true;
	}
	return;
}

void UDroneSensorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	(void)DeltaTime;
	(void)TickType;
	(void)ThisTickFunction;
	return;
}

void UDroneSensorComponent::SetUavId(const FString& InId)
{
	UavId = InId;
	LatestBatch.UavId = UavId;
}

ADronePawn* UDroneSensorComponent::GetDroneOwner() const
{
	if (AActor* Owner = GetOwner())
	{
		return Cast<ADronePawn>(Owner);
	}

	return nullptr;
}

ASimWorldManager* UDroneSensorComponent::FindSimWorldManager() const
{
	if (GetWorld())
	{
		for (TActorIterator<ASimWorldManager> It(GetWorld()); It; ++It)
		{
			return *It;
		}
	}

	return nullptr;
}

void UDroneSensorComponent::PerformDetection()
{
	return;
}

void UDroneSensorComponent::SetDetectionRate(float Rate)
{
	(void)Rate;
	DetectionHz = 0.0f;
	SamplingTimer = 0.0f;
	SetComponentTickEnabled(false);
	Deactivate();
}
