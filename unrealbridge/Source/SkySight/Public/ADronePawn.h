#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Pawn.h"
#include "HAL/CriticalSection.h"
#include "TimerManager.h"
#include "UUnrealBridgeProtocol.h"
#include "ADronePawn.generated.h"

class UStaticMeshComponent;
class UFloatingPawnMovement;
class UDroneRouteFollowerComponent;
class UDroneTelemetryComponent;
class UDroneSensorComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UCameraVideoStreamerComponent;

UCLASS()
class SKYSIGHT_API ADronePawn : public APawn
{
	GENERATED_BODY()

public:
	ADronePawn();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void InitializeForSimulation(const FSkysightGeoReference& Reference, float TelemetryRate, float DetectionRate);
	void ApplyRoute(const FSkysightRoute& Route);
	void ApplyCommand(const FSkysightCommand& Command);
	void ConfigureAltitudeHold(float InFixedWorldAltitudeCm, bool bDisableTerrainFollow = true);
	void SetTerrainFollowEnabled(bool bEnabled);
	void SetDesiredAGLMeters(float InDesiredAGLMeters);

	const FSkysightTelemetry& GetLatestTelemetry() const;
	const FDetectionsBatch& GetLatestDetections() const;
	bool GetCameraFrameBytes(TArray<uint8>& OutBytes) const;
	UCameraVideoStreamerComponent* GetVideoStreamer() const { return VideoStreamer; }
	const FSkysightGeoReference& GetGeoReference() const { return GeoReference; }
	FString GetFlightMode() const { return FlightMode; }
	bool IsRouteComplete() const;
	float GetCameraFovDeg() const;
	float GetCameraMountPitchDeg() const;
	float GetCameraMountYawDeg() const;
	float GetCameraMountRollDeg() const;
	FRotator GetCameraRelativeRotation() const;
	FRotator GetCameraBaseRelativeRotation() const;
	FVector GetCameraRelativeLocation() const;
	FIntPoint GetCameraResolution() const;
	float GetCameraAspectRatio() const;
	float GetAltitudeAglMeters() const;
	bool IsCameraTrackingTarget() const { return bCameraTrackTarget; }
	void SetCameraTrackPoint(const FVector& WorldPoint);
	void ClearCameraTrack();

protected:
	// Components
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	USceneComponent* Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	USceneComponent* MotionRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	USceneComponent* VisualRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	USceneComponent* ThirdPersonMount;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	UStaticMeshComponent* BodyMesh;

	UPROPERTY(VisibleAnywhere)
	UFloatingPawnMovement* MovementComponent;

	UPROPERTY(VisibleAnywhere)
	UDroneRouteFollowerComponent* RouteFollower;

	UPROPERTY(VisibleAnywhere)
	UDroneTelemetryComponent* TelemetryComponent;

	UPROPERTY(VisibleAnywhere)
	UDroneSensorComponent* SensorComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Drone")
	USceneCaptureComponent2D* SceneCapture;

	UPROPERTY(VisibleAnywhere)
	UCameraVideoStreamerComponent* VideoStreamer;

	UPROPERTY()
	UTextureRenderTarget2D* CaptureTarget;

	UPROPERTY(EditAnywhere, Category = "Camera")
	int32 CameraWidth = 1920;

	UPROPERTY(EditAnywhere, Category = "Camera")
	int32 CameraHeight = 1080;

	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = "1", ClampMax = "100"))
	int32 CameraJpegQuality = 80;

	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = "1", ClampMax = "100"))
	int32 CameraMinJpegQuality = 60;

	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = "1.0"))
	float CameraFps = 30.0f;

	UPROPERTY(EditAnywhere, Category = "Camera")
	bool bCameraUseFinalColorLdr = true;

	UPROPERTY(EditAnywhere, Category = "Camera|Mount")
	float CameraMountPitchDeg = -90.0f;

	UPROPERTY(EditAnywhere, Category = "Camera|Mount")
	float CameraMountYawDeg = 0.0f;

	UPROPERTY(EditAnywhere, Category = "Camera|Mount")
	float CameraMountRollDeg = 0.0f;

	UPROPERTY(EditAnywhere, Category = "Camera|Track", meta = (ClampMin = "0.1"))
	float CameraTrackInterpSpeed = 5.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Camera|Mount")
	FVector CameraRelLocation = FVector(220.0f, 0.0f, 90.0f);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Camera|Mount")
	FRotator CameraRelRotation = FRotator(-90.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, Category = "Camera", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float CameraTargetGamma = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Camera|PostProcess")
	bool bCameraDisableMotionBlur = true;

	UPROPERTY(EditAnywhere, Category = "Camera|PostProcess")
	bool bCameraDisableBloom = false;

	UPROPERTY(EditAnywhere, Category = "Camera|PostProcess")
	bool bCameraDisableAmbientOcclusion = false;

	UPROPERTY(EditAnywhere, Category = "Camera|Perf")
	bool bCameraAutoAdjustJpegQuality = true;

	UPROPERTY(EditAnywhere, Category = "Camera|Perf", meta = (ClampMin = "0.0"))
	float CameraPerfLogIntervalSec = 3.0f;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float MaxSpeedCmPerSec = 1500.0f;

	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "0.01"))
	float SpeedScale = 0.6666667f;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float AccelerationCmPerSec2 = 4000.0f;

	UPROPERTY(EditAnywhere, Category = "Movement")
	float DecelerationCmPerSec2 = 5000.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	bool bFollowTerrain = true;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	bool bUseFixedWorldAltitude = false;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude", meta = (EditCondition = "bUseFixedWorldAltitude"))
	float FixedWorldAltitudeCm = -4000.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	float DesiredAGLMeters = 30.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	float TerrainFollowInterpSpeed = 4.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	float TerrainTraceUpCm = 50000.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Altitude")
	float TerrainTraceDownCm = 250000.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Rotation")
	float TurnInterpSpeed = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Rotation")
	float MinTurnSpeedCmPerSec = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	float MeshYawOffsetDeg = 180.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Visual")
	bool bBankMeshOnly = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	bool bDisableCollisionForSim = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drone|Flight")
	bool bSweepMoves = false;

	UPROPERTY(EditAnywhere, Category = "Movement|Bank")
	bool bEnableBanking = false;

	UPROPERTY(EditAnywhere, Category = "Movement|Bank", meta = (EditCondition = "bEnableBanking"))
	float MaxBankAngleDeg = 12.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Bank", meta = (EditCondition = "bEnableBanking"))
	float BankInterpSpeed = 6.0f;

	UPROPERTY(EditAnywhere, Category = "Movement|Bank", meta = (EditCondition = "bEnableBanking"))
	float BankYawRateScale = 0.1f;

	mutable FCriticalSection CameraMutex;
	TArray<uint8> CachedJpeg;
	TArray<FColor> CameraReadbackScratch;
	FTimerHandle CameraTimerHandle;
	int32 RuntimeCameraJpegQuality = -1;
	int32 CameraOverBudgetFrameCount = 0;
	int32 CameraUnderBudgetFrameCount = 0;
	double CameraPerfLastLogSeconds = 0.0;
	double CameraPerfAccumCaptureMs = 0.0;
	double CameraPerfAccumReadbackMs = 0.0;
	double CameraPerfAccumJpegMs = 0.0;
	double CameraPerfAccumTotalMs = 0.0;
	int32 CameraPerfAccumFrames = 0;

	FSkysightGeoReference GeoReference;
	FSkysightRoute CurrentRoute;
	FString FlightMode = TEXT("IDLE");
	FString UavId = TEXT("sim");
	FVector HomeLocationCm = FVector::ZeroVector;
	FVector PrevLocation = FVector::ZeroVector;
	FRotator LastFacingRotation = FRotator::ZeroRotator;
	FRotator BodyMeshBaseRotation = FRotator::ZeroRotator;
	float CurrentBankRoll = 0.0f;
	bool bHasPrevLocation = false;
	bool bHasFacingRotation = false;
	bool bUseRouteWaypointAltitude = false;
	bool bCameraTrackTarget = false;
	FVector CameraTrackWorldPoint = FVector::ZeroVector;
	FRotator DefaultCameraRelRotation = FRotator::ZeroRotator;

	void UpdateRouteFromGeo();
	void ApplyMovementSettings();
	void UpdateFacingRotation(float DeltaTime);
	void UpdateBanking(float DeltaTime, float YawRateDegPerSec);
	void UpdateCameraCache();
	void SyncAuthoredCameraMountFromSceneCapture();

private:
	bool TraceGroundZ(float X, float Y, float& OutGroundZ) const;
	void MaintainAltitude(float DeltaTime);
};
