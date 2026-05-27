#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RHIResources.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "CameraVideoStreamerComponent.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

namespace AVEncoder
{
	class FVideoEncoderInput;
	class FVideoEncoderInputFrame;
	class FVideoEncoder;
	class FCodecPacket;
}

struct FCameraVideoStreamerImpl;

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent), Config=Game)
class SKYSIGHT_API UCameraVideoStreamerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCameraVideoStreamerComponent();
	virtual ~UCameraVideoStreamerComponent() override;

	void InitializeStreaming(USceneCaptureComponent2D* InSceneCapture, UTextureRenderTarget2D* InRenderTarget);
	void ShutdownStreaming();

	bool IsReady() const { return bEncoderReady; }
	bool DequeueVideoPacket(TArray<uint8>& OutPacket);
	bool GetLatestJpeg(TArray<uint8>& OutJpeg) const;
	TArray<uint8> GetStreamHeader() const;
	void SetVideoStreamingEnabled(bool bEnabled);
	void SetJpegActive(bool bEnabled);
	void SetLastJpegRequestTime(double RequestTimeSec);
	bool EnsureJpegFresh(double NowSec, double MinIntervalSec);
	void UpdateStreamingState(bool bWantVideo, bool bWantJpeg);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 TargetWidth = 640;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 TargetHeight = 360;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 FallbackWidth = 960;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 FallbackHeight = 540;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 TargetFps = 15;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 TargetBitrate = 1200000;

	UPROPERTY(EditAnywhere, Config, Category="Video", meta=(ClampMin="0"))
	int32 JpegFps = 0;

	UPROPERTY(EditAnywhere, Config, Category="Video", meta=(ClampMin="1", ClampMax="100"))
	int32 JpegQuality = 75;

	UPROPERTY(EditAnywhere, Config, Category="Video", meta=(ClampMin="1", ClampMax="100"))
	int32 MinJpegQuality = 60;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	bool bUseLdrFinalColorForJpegAndVideo = true;

	UPROPERTY(EditAnywhere, Config, Category="Video", meta=(ClampMin="1.0", ClampMax="4.0"))
	float CaptureTargetGamma = 1.8f;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	bool bDisableJpegFlushRenderingCommands = true;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	bool bReuseLatestCaptureForJpeg = true;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	bool bAutoAdjustJpegQuality = true;

	UPROPERTY(EditAnywhere, Config, Category="Video", meta=(ClampMin="0.0"))
	float JpegPerfLogIntervalSec = 3.0f;

	UPROPERTY(EditAnywhere, Config, Category="Video")
	int32 QueueSize = 32;

private:
	void LoadConfig(); // если у тебя уже было — ок. если нет — просто добавь пустую реализацию в cpp или убери вызов.

	void StartCaptureTimers();
	void StopCaptureTimers();
	void RefreshCaptureTimers();
	int32 GetEffectiveJpegFps() const;
	bool IsJpegDemandActiveNow() const;
	void CaptureAndEncodeFrame();
	void UpdateCachedJpeg();
	void ApplyCaptureSettings(int32 Width, int32 Height);
	bool InitializeEncoder(int32 Width, int32 Height);
	void ShutdownEncoder();

	void EncodeOnRenderThread(const FTextureRHIRef& SourceTexture);
	void HandleEncodedPacket(uint32 LayerIndex, const TSharedPtr<AVEncoder::FVideoEncoderInputFrame> Frame, const AVEncoder::FCodecPacket& Packet);
	TSharedPtr<AVEncoder::FVideoEncoderInputFrame> ObtainInputFrame();

	void PushPacket(TArray<uint8>&& Packet);

private:
	mutable FCriticalSection QueueMutex;
	TArray<TArray<uint8>> PacketRing;
	int32 RingStart = 0;
	int32 RingCount = 0;

	mutable FCriticalSection JpegMutex;
	TArray<uint8> LatestJpeg;
	TArray<FColor> JpegReadbackScratch;
	int32 RuntimeJpegQuality = -1;
	int32 JpegOverBudgetFrameCount = 0;
	int32 JpegUnderBudgetFrameCount = 0;
	double JpegPerfLastLogSeconds = 0.0;
	double JpegPerfAccumCaptureMs = 0.0;
	double JpegPerfAccumReadbackMs = 0.0;
	double JpegPerfAccumJpegMs = 0.0;
	double JpegPerfAccumTotalMs = 0.0;
	int32 JpegPerfAccumFrames = 0;

	FCriticalSection EncoderMutex;
	mutable FCriticalSection MuxerMutex;
	bool bEncoderReady = false;

	USceneCaptureComponent2D* SceneCapture = nullptr;
	UTextureRenderTarget2D* CaptureTarget = nullptr;

	TAtomic<int32> CachedWidth{0};
	TAtomic<int32> CachedHeight{0};
	double LastSceneCaptureSeconds = 0.0;
	double LastJpegProducedSec = 0.0;
	double LastBackpressureLogSec = 0.0;

	FTimerHandle CaptureTimerHandle;
	FTimerHandle JpegTimerHandle;
	bool bVideoStreamingEnabled = false;
	bool bJpegStreamingEnabled = false;
	double LastJpegRequestTimeSec = 0.0;
	double JpegDemandTimeoutSec = 1.5;

	// важно: НЕ smart pointer в header (иначе снова incomplete type delete в UHT)
	FCameraVideoStreamerImpl* Impl = nullptr;
};
