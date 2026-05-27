#include "UDroneTelemetryComponent.h"
#include "ADronePawn.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"

UDroneTelemetryComponent::UDroneTelemetryComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UDroneTelemetryComponent::BeginPlay()
{
	Super::BeginPlay();
	SampleInterval = TelemetryHz > 0.0f ? 1.0f / TelemetryHz : 0.1f;
}

void UDroneTelemetryComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (SampleInterval <= 0.0f)
	{
		return;
	}

	SamplingTimer += DeltaTime;
	if (SamplingTimer >= SampleInterval)
	{
		SampleTelemetry();
		SamplingTimer = 0.0f;
	}
}

void UDroneTelemetryComponent::SetUavId(const FString& InId)
{
	UavId = InId;
	LatestTelemetry.UavId = UavId;
}

void UDroneTelemetryComponent::SampleTelemetry()
{
	if (ADronePawn* Drone = GetDroneOwner())
	{
		const FVector LocationCm = Drone->GetActorLocation();
		double Latitude = 0.0;
		double Longitude = 0.0;
		double Altitude = 0.0;
		UUnrealBridgeProtocol::UnrealCmToGeo(Drone->GetGeoReference(), LocationCm, Latitude, Longitude, Altitude);

		SampleInterval = TelemetryHz > 0.0f ? 1.0f / TelemetryHz : SampleInterval;

		BatteryPercent = FMath::Max(0.0f, BatteryPercent - (BatteryDrainPerSecond * SampleInterval));

		LatestTelemetry.UavId = UavId.IsEmpty() ? TEXT("sim") : UavId;
		LatestTelemetry.TimestampSecondsEpoch = UUnrealBridgeProtocol::GetUnixEpochSeconds();
		LatestTelemetry.LatitudeDeg = Latitude;
		LatestTelemetry.LongitudeDeg = Longitude;
		LatestTelemetry.AltitudeMeters = Altitude;
		LatestTelemetry.AltitudeAglMeters = Drone->GetAltitudeAglMeters();
		const FRotator Rotation = Drone->GetActorRotation();
		LatestTelemetry.YawDeg = Rotation.Yaw;
		LatestTelemetry.PitchDeg = Rotation.Pitch;
		LatestTelemetry.RollDeg = Rotation.Roll;
		LatestTelemetry.GroundSpeedMps = Drone->GetVelocity().Size() / 100.0f;
		LatestTelemetry.BatteryPercent = static_cast<float>(BatteryPercent);
		LatestTelemetry.FlightMode = Drone->GetFlightMode();
		LatestTelemetry.bArmed = true;
	}
}

void UDroneTelemetryComponent::SetTelemetryRate(float Rate)
{
	if (Rate > 0.0f)
	{
		TelemetryHz = Rate;
		SampleInterval = 1.0f / TelemetryHz;
	}
}

ADronePawn* UDroneTelemetryComponent::GetDroneOwner() const
{
	if (AActor* Owner = GetOwner())
	{
		return Cast<ADronePawn>(Owner);
	}

	return nullptr;
}
