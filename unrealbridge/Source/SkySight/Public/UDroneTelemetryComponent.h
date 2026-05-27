#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UUnrealBridgeProtocol.h"
#include "UDroneTelemetryComponent.generated.h"

class ADronePawn;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class SKYSIGHT_API UDroneTelemetryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDroneTelemetryComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	const FSkysightTelemetry& GetLatestTelemetry() const { return LatestTelemetry; }

	void SetUavId(const FString& InId);
	void SetTelemetryRate(float Rate);

	UPROPERTY(EditAnywhere, Category="Telemetry")
	float TelemetryHz = 10.0f;

	UPROPERTY(EditAnywhere, Category="Telemetry")
	float BatteryDrainPerSecond = 0.5f;

protected:
	float SampleInterval = 0.1f;
	float SamplingTimer = 0.0f;
	FSkysightTelemetry LatestTelemetry;
	FString UavId = TEXT("sim");
	float BatteryPercent = 100.0f;

	void SampleTelemetry();
	ADronePawn* GetDroneOwner() const;
};
