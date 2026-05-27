#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UUnrealBridgeProtocol.h"
#include "UDroneSensorComponent.generated.h"

class ASimWorldManager;
class ADronePawn;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SKYSIGHT_API UDroneSensorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDroneSensorComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	const FDetectionsBatch& GetLatestBatch() const { return LatestBatch; }

	void SetUavId(const FString& InId);
	void SetDetectionRate(float Rate);

	UPROPERTY(EditAnywhere, Category="Sensor")
	float DetectionHz = 2.0f;

	UPROPERTY(EditAnywhere, Category="Sensor")
	float DetectionRangeMeters = 500.0f;

	UPROPERTY(EditAnywhere, Category="Sensor")
	float DetectionFovDegrees = 120.0f;

	UPROPERTY(EditAnywhere, Category="Sensor|Debug")
	bool bDebugFullCircleScan = false;

protected:
	float SamplingTimer = 0.0f;
	FDetectionsBatch LatestBatch;
	FString UavId = TEXT("sim");
	ASimWorldManager* WorldManager = nullptr;

	void PerformDetection();
	ADronePawn* GetDroneOwner() const;
	ASimWorldManager* FindSimWorldManager() const;
};
