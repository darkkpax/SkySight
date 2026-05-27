#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UUnrealBridgeProtocol.generated.h"

UENUM(BlueprintType)
enum class ESkysightCommandType : uint8
{
	RESET,
	DESPAWN,
	HOLD,
	RESUME,
	RTL,
	SET_SPEED,
	SET_VELOCITY,
	CLEAR_VELOCITY_OVERRIDE,
	SET_MODE,
	ORBIT_START,
	ORBIT_STOP
};

UENUM(BlueprintType)
enum class ESkysightObjectClass : uint8
{
	Unknown,
	Fire,
	Human
};

USTRUCT(BlueprintType)
struct FSkysightGeoReference
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	double OriginLatDeg = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	double OriginLonDeg = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	double OriginAltMeters = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float MetersToUnrealCm = 100.0f;

	UPROPERTY(BlueprintReadWrite)
	FVector OriginWorldCm = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FSkysightTelemetry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString UavId;

	UPROPERTY(BlueprintReadWrite)
	double TimestampSecondsEpoch = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LatitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LongitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double AltitudeMeters = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double AltitudeAglMeters = 0.0;

	UPROPERTY(BlueprintReadWrite)
	float YawDeg = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float PitchDeg = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float RollDeg = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float GroundSpeedMps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float BatteryPercent = 100.0f;

	UPROPERTY(BlueprintReadWrite)
	bool bArmed = true;

	UPROPERTY(BlueprintReadWrite)
	FString FlightMode;
};

USTRUCT(BlueprintType)
struct FSkysightWaypoint
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	double LatitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LongitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double AltitudeMeters = 0.0;
};

USTRUCT(BlueprintType)
struct FSkysightRoute
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString UavId = TEXT("sim");

	UPROPERTY(BlueprintReadWrite)
	int32 Version = 1;

	UPROPERTY(BlueprintReadWrite)
	TArray<FSkysightWaypoint> Waypoints;

	UPROPERTY(BlueprintReadWrite)
	int32 ActiveIndex = 0;
};

USTRUCT(BlueprintType)
struct FSkysightCommand
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString UavId = TEXT("sim");

	UPROPERTY(BlueprintReadWrite)
	ESkysightCommandType Type = ESkysightCommandType::RESET;

	UPROPERTY(BlueprintReadWrite)
	FString ModeName;

	UPROPERTY(BlueprintReadWrite)
	float SpeedMps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float VxMps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float VyMps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float VzMps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	float YawRateDps = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	int32 Sequence = 0;

	UPROPERTY(BlueprintReadWrite)
	bool bHasTargetWorldPoint = false;

	UPROPERTY(BlueprintReadWrite)
	FVector TargetWorldPointCm = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite)
	bool bClearCameraTrack = false;
};

USTRUCT(BlueprintType)
struct FDetectionMessage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString SourceId;

	UPROPERTY(BlueprintReadWrite)
	ESkysightObjectClass ClassId = ESkysightObjectClass::Unknown;

	UPROPERTY(BlueprintReadWrite)
	float Confidence = 0.0f;

	UPROPERTY(BlueprintReadWrite)
	double LatitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LongitudeDeg = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double AltitudeMeters = 0.0;
};

USTRUCT(BlueprintType)
struct FDetectionsBatch
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString UavId = TEXT("sim");

	UPROPERTY(BlueprintReadWrite)
	double TimestampSecondsEpoch = 0.0;

	UPROPERTY(BlueprintReadWrite)
	TArray<FDetectionMessage> Detections;
};

USTRUCT(BlueprintType)
struct FMapSnapshotInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	FString ImagePath;

	UPROPERTY(BlueprintReadWrite)
	double LatMin = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LonMin = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LatMax = 0.0;

	UPROPERTY(BlueprintReadWrite)
	double LonMax = 0.0;

	UPROPERTY(BlueprintReadWrite)
	int32 WidthPx = 0;

	UPROPERTY(BlueprintReadWrite)
	int32 HeightPx = 0;
};

UCLASS()
class SKYSIGHT_API UUnrealBridgeProtocol : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="Skysight|Geo")
	static FVector GeoToUnrealCm(const FSkysightGeoReference& Reference, double LatitudeDeg, double LongitudeDeg, double AltitudeMeters);

	UFUNCTION(BlueprintCallable, Category="Skysight|Geo")
	static void UnrealCmToGeo(const FSkysightGeoReference& Reference, const FVector& PositionCm, double& OutLatitudeDeg, double& OutLongitudeDeg, double& OutAltitudeMeters);

	UFUNCTION(BlueprintCallable, Category="Skysight|Time")
	static double GetUnixEpochSeconds();
};
