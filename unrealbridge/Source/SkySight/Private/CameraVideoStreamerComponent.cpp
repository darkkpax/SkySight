#include "CameraVideoStreamerComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

// AVEncoder (deprecated in 5.5, но пока компилится)
#include "VideoEncoderFactory.h"
#include "VideoEncoderInput.h"
#include "VideoEncoder.h"

namespace
{
	bool IsSolidColorFrame(const TArray<FColor>& Bitmap)
	{
		if (Bitmap.Num() <= 1)
		{
			return Bitmap.Num() == 1;
		}

		const FColor First = Bitmap[0];
		for (int32 Index = 1; Index < Bitmap.Num(); ++Index)
		{
			if (Bitmap[Index] != First)
			{
				return false;
			}
		}

		return true;
	}

	uint32 MpegTsCrc32(const uint8* Data, int32 Length)
	{
		static uint32 Table[256];
		static bool bInitialized = false;
		if (!bInitialized)
		{
			for (uint32 i = 0; i < 256; ++i)
			{
				uint32 Crc = i << 24;
				for (int32 Bit = 0; Bit < 8; ++Bit)
				{
					Crc = (Crc & 0x80000000) ? (Crc << 1) ^ 0x04C11DB7 : (Crc << 1);
				}
				Table[i] = Crc;
			}
			bInitialized = true;
		}

		uint32 Crc = 0xFFFFFFFF;
		for (int32 i = 0; i < Length; ++i)
		{
			const uint8 Index = static_cast<uint8>((Crc >> 24) ^ Data[i]);
			Crc = (Crc << 8) ^ Table[Index];
		}
		return Crc;
	}

	void WritePts(TArray<uint8>& Out, uint64 Pts)
	{
		const uint64 Masked = Pts & 0x1FFFFFFFF;
		const uint8 Pts32_30 = static_cast<uint8>((Masked >> 30) & 0x07);
		const uint16 Pts29_15 = static_cast<uint16>((Masked >> 15) & 0x7FFF);
		const uint16 Pts14_0 = static_cast<uint16>(Masked & 0x7FFF);

		Out.Add(static_cast<uint8>(0x20 | (Pts32_30 << 1) | 1));
		Out.Add(static_cast<uint8>(Pts29_15 >> 7));
		Out.Add(static_cast<uint8>(((Pts29_15 & 0x7F) << 1) | 1));
		Out.Add(static_cast<uint8>(Pts14_0 >> 7));
		Out.Add(static_cast<uint8>(((Pts14_0 & 0x7F) << 1) | 1));
	}

	void WriteTsPacket(
		TArray<uint8>& Out,
		uint16 Pid,
		bool bPayloadStart,
		uint8 ContinuityCounter,
		const uint8* Payload,
		int32 PayloadLen,
		bool bUseAdaptation,
		bool bIncludePcr,
		uint64 PcrBase)
	{
		uint8 Packet[188];
		FMemory::Memset(Packet, 0xFF, sizeof(Packet));

		Packet[0] = 0x47;
		Packet[1] = static_cast<uint8>((bPayloadStart ? 0x40 : 0x00) | ((Pid >> 8) & 0x1F));
		Packet[2] = static_cast<uint8>(Pid & 0xFF);

		uint8 AdaptationControl = bUseAdaptation ? 0x30 : 0x10;
		Packet[3] = static_cast<uint8>(AdaptationControl | (ContinuityCounter & 0x0F));

		int32 HeaderSize = 4;
		if (bUseAdaptation)
		{
			int32 AdaptationFieldLen = 0;
			if (bIncludePcr)
			{
				AdaptationFieldLen = 1 + 6;
			}

			int32 AvailablePayload = 184 - (1 + AdaptationFieldLen);
			if (PayloadLen < AvailablePayload)
			{
				if (AdaptationFieldLen == 0)
				{
					AdaptationFieldLen = 1;
				}
				AdaptationFieldLen += (AvailablePayload - PayloadLen);
			}

			Packet[HeaderSize] = static_cast<uint8>(AdaptationFieldLen);
			if (AdaptationFieldLen > 0)
			{
				uint8 Flags = bIncludePcr ? 0x10 : 0x00;
				Packet[HeaderSize + 1] = Flags;
				int32 AdaptationWriteIndex = HeaderSize + 2;

				if (bIncludePcr)
				{
					const uint64 PcrMasked = PcrBase & 0x1FFFFFFFF;
					Packet[AdaptationWriteIndex + 0] = static_cast<uint8>(PcrMasked >> 25);
					Packet[AdaptationWriteIndex + 1] = static_cast<uint8>(PcrMasked >> 17);
					Packet[AdaptationWriteIndex + 2] = static_cast<uint8>(PcrMasked >> 9);
					Packet[AdaptationWriteIndex + 3] = static_cast<uint8>(PcrMasked >> 1);
					Packet[AdaptationWriteIndex + 4] = static_cast<uint8>(((PcrMasked & 0x1) << 7) | 0x7E);
					Packet[AdaptationWriteIndex + 5] = 0x00;
					AdaptationWriteIndex += 6;
				}

				for (int32 Index = AdaptationWriteIndex; Index < HeaderSize + 1 + AdaptationFieldLen; ++Index)
				{
					Packet[Index] = 0xFF;
				}
			}

			HeaderSize += 1 + AdaptationFieldLen;
		}

		if (PayloadLen > 0)
		{
			FMemory::Memcpy(Packet + HeaderSize, Payload, PayloadLen);
		}

		Out.Append(Packet, sizeof(Packet));
	}
}

struct FMpegTsMuxer
{
	uint16 PmtPid = 0x0100;
	uint16 VideoPid = 0x0101;
	uint8 PatContinuity = 0;
	uint8 PmtContinuity = 0;
	uint8 VideoContinuity = 0;
	double Pts90k = 0.0;
	double PtsStep90k = 0.0;

	void Reset(double InPtsStep)
	{
		PatContinuity = 0;
		PmtContinuity = 0;
		VideoContinuity = 0;
		Pts90k = 0.0;
		PtsStep90k = InPtsStep;
	}

	void BuildPatPmt(TArray<uint8>& Out)
	{
		TArray<uint8> PatSection;
		PatSection.Reserve(16);
		PatSection.Add(0x00);
		PatSection.Add(0xB0);
		PatSection.Add(0x0D);
		PatSection.Add(0x00);
		PatSection.Add(0x01);
		PatSection.Add(0xC1);
		PatSection.Add(0x00);
		PatSection.Add(0x00);
		PatSection.Add(0x00);
		PatSection.Add(0x01);
		PatSection.Add(static_cast<uint8>(0xE0 | ((PmtPid >> 8) & 0x1F)));
		PatSection.Add(static_cast<uint8>(PmtPid & 0xFF));

		uint32 PatCrc = MpegTsCrc32(PatSection.GetData(), PatSection.Num());
		PatSection.Add(static_cast<uint8>(PatCrc >> 24));
		PatSection.Add(static_cast<uint8>(PatCrc >> 16));
		PatSection.Add(static_cast<uint8>(PatCrc >> 8));
		PatSection.Add(static_cast<uint8>(PatCrc));

		TArray<uint8> PatPayload;
		PatPayload.Add(0x00);
		PatPayload.Append(PatSection);
		const int32 PatFillStart = PatPayload.Num();
		PatPayload.SetNum(184);
		for (int32 Index = PatFillStart; Index < PatPayload.Num(); ++Index)
		{
			PatPayload[Index] = 0xFF;
		}

		WriteTsPacket(Out, 0x0000, true, PatContinuity++, PatPayload.GetData(), PatPayload.Num(), false, false, 0);

		TArray<uint8> PmtSection;
		PmtSection.Reserve(21);
		PmtSection.Add(0x02);
		PmtSection.Add(0xB0);
		PmtSection.Add(0x12);
		PmtSection.Add(0x00);
		PmtSection.Add(0x01);
		PmtSection.Add(0xC1);
		PmtSection.Add(0x00);
		PmtSection.Add(0x00);
		PmtSection.Add(static_cast<uint8>(0xE0 | ((VideoPid >> 8) & 0x1F)));
		PmtSection.Add(static_cast<uint8>(VideoPid & 0xFF));
		PmtSection.Add(0xF0);
		PmtSection.Add(0x00);
		PmtSection.Add(0x1B);
		PmtSection.Add(static_cast<uint8>(0xE0 | ((VideoPid >> 8) & 0x1F)));
		PmtSection.Add(static_cast<uint8>(VideoPid & 0xFF));
		PmtSection.Add(0xF0);
		PmtSection.Add(0x00);

		uint32 PmtCrc = MpegTsCrc32(PmtSection.GetData(), PmtSection.Num());
		PmtSection.Add(static_cast<uint8>(PmtCrc >> 24));
		PmtSection.Add(static_cast<uint8>(PmtCrc >> 16));
		PmtSection.Add(static_cast<uint8>(PmtCrc >> 8));
		PmtSection.Add(static_cast<uint8>(PmtCrc));

		TArray<uint8> PmtPayload;
		PmtPayload.Add(0x00);
		PmtPayload.Append(PmtSection);
		const int32 PmtFillStart = PmtPayload.Num();
		PmtPayload.SetNum(184);
		for (int32 Index = PmtFillStart; Index < PmtPayload.Num(); ++Index)
		{
			PmtPayload[Index] = 0xFF;
		}

		WriteTsPacket(Out, PmtPid, true, PmtContinuity++, PmtPayload.GetData(), PmtPayload.Num(), false, false, 0);
	}

	void BuildPes(const TArray<uint8>& H264Data, TArray<uint8>& Out)
	{
		TArray<uint8> PesPayload;
		PesPayload.Reserve(9 + 5 + H264Data.Num());
		PesPayload.Add(0x00);
		PesPayload.Add(0x00);
		PesPayload.Add(0x01);
		PesPayload.Add(0xE0);
		PesPayload.Add(0x00);
		PesPayload.Add(0x00);
		PesPayload.Add(0x80);
		PesPayload.Add(0x80);
		PesPayload.Add(0x05);

		uint64 Pts = static_cast<uint64>(Pts90k);
		WritePts(PesPayload, Pts);
		PesPayload.Append(H264Data);

		int32 Offset = 0;
		bool bFirstPacket = true;
		while (Offset < PesPayload.Num())
		{
			const int32 Remaining = PesPayload.Num() - Offset;
			const int32 BasePayload = 184;
			bool bUseAdaptation = bFirstPacket;
			bool bIncludePcr = bFirstPacket;
			int32 AdaptationLen = bFirstPacket ? (1 + 6) : 0;
			int32 AvailablePayload = BasePayload - (bUseAdaptation ? (1 + AdaptationLen) : 0);

			if (Remaining < AvailablePayload)
			{
				if (!bUseAdaptation)
				{
					bUseAdaptation = true;
					AdaptationLen = 1;
				}
				AdaptationLen += (AvailablePayload - Remaining);
				AvailablePayload = Remaining;
			}

			const int32 PayloadSize = FMath::Min(Remaining, AvailablePayload);
			WriteTsPacket(Out, VideoPid, bFirstPacket, VideoContinuity++, PesPayload.GetData() + Offset, PayloadSize, bUseAdaptation, bIncludePcr, Pts);
			Offset += PayloadSize;
			bFirstPacket = false;
		}

		Pts90k += PtsStep90k;
	}
};

struct FCameraVideoStreamerImpl
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	TSharedPtr<AVEncoder::FVideoEncoderInput> VideoEncoderInput;
	TUniquePtr<AVEncoder::FVideoEncoder> VideoEncoder;
	TMap<TSharedPtr<AVEncoder::FVideoEncoderInputFrame>, FTextureRHIRef> BackBuffers;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	TUniquePtr<FMpegTsMuxer> TsMuxer;
};

UCameraVideoStreamerComponent::UCameraVideoStreamerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	LoadConfig();
}

UCameraVideoStreamerComponent::~UCameraVideoStreamerComponent()
{
	ShutdownStreaming();

	if (Impl)
	{
		delete Impl;
		Impl = nullptr;
	}
}

void UCameraVideoStreamerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!Impl)
	{
		Impl = new FCameraVideoStreamerImpl();
	}
}

void UCameraVideoStreamerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownStreaming();

	if (Impl)
	{
		delete Impl;
		Impl = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void UCameraVideoStreamerComponent::InitializeStreaming(USceneCaptureComponent2D* InSceneCapture, UTextureRenderTarget2D* InRenderTarget)
{
	SceneCapture = InSceneCapture;
	CaptureTarget = InRenderTarget;

	if (!SceneCapture)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: SceneCapture is missing"));
		return;
	}

	ApplyCaptureSettings(TargetWidth, TargetHeight);

	if (!InitializeEncoder(TargetWidth, TargetHeight) && (FallbackWidth > 0 && FallbackHeight > 0))
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: fallback to %dx%d"), FallbackWidth, FallbackHeight);
		ApplyCaptureSettings(FallbackWidth, FallbackHeight);
		InitializeEncoder(FallbackWidth, FallbackHeight);
	}

	{
		const int32 ActiveWidth = CachedWidth.Load();
		const int32 ActiveHeight = CachedHeight.Load();
		const int32 EffectiveJpegFps = GetEffectiveJpegFps();
		RuntimeJpegQuality = FMath::Clamp(JpegQuality, 1, 100);
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer active capture settings: %dx%d targetFps=%d bitrate=%d queue=%d jpegFps=%d (requested=%d) jpegQuality=%d ldr=%d demand(video=%d jpeg=%d)"),
			ActiveWidth > 0 ? ActiveWidth : TargetWidth,
			ActiveHeight > 0 ? ActiveHeight : TargetHeight,
			TargetFps,
			TargetBitrate,
			QueueSize,
			EffectiveJpegFps,
			JpegFps,
			RuntimeJpegQuality,
			bUseLdrFinalColorForJpegAndVideo ? 1 : 0,
			bVideoStreamingEnabled ? 1 : 0,
			bJpegStreamingEnabled ? 1 : 0);
	}

	if (GetEffectiveJpegFps() > 0 && bJpegStreamingEnabled && IsJpegDemandActiveNow())
	{
		UpdateCachedJpeg();
	}
	StartCaptureTimers();
}

void UCameraVideoStreamerComponent::ShutdownStreaming()
{
	StopCaptureTimers();
	ShutdownEncoder();
}

bool UCameraVideoStreamerComponent::DequeueVideoPacket(TArray<uint8>& OutPacket)
{
	FScopeLock Lock(&QueueMutex);
	if (RingCount == 0 || PacketRing.Num() == 0)
	{
		return false;
	}

	OutPacket = MoveTemp(PacketRing[RingStart]);
	RingStart = (RingStart + 1) % PacketRing.Num();
	RingCount--;
	return true;
}

bool UCameraVideoStreamerComponent::GetLatestJpeg(TArray<uint8>& OutJpeg) const
{
	FScopeLock Lock(&JpegMutex);
	if (LatestJpeg.Num() == 0)
	{
		return false;
	}

	OutJpeg = LatestJpeg;
	return true;
}

TArray<uint8> UCameraVideoStreamerComponent::GetStreamHeader() const
{
	TArray<uint8> Header;
	{
		FScopeLock Lock(&MuxerMutex);
		if (Impl && Impl->TsMuxer.IsValid())
		{
			FMpegTsMuxer Local = *Impl->TsMuxer;
			Local.BuildPatPmt(Header);
		}
	}
	return Header;
}

void UCameraVideoStreamerComponent::SetVideoStreamingEnabled(bool bEnabled)
{
	UpdateStreamingState(bEnabled, bJpegStreamingEnabled);
}

void UCameraVideoStreamerComponent::SetJpegActive(bool bEnabled)
{
	UpdateStreamingState(bVideoStreamingEnabled, bEnabled);
}

void UCameraVideoStreamerComponent::SetLastJpegRequestTime(double RequestTimeSec)
{
	LastJpegRequestTimeSec = RequestTimeSec;
	RefreshCaptureTimers();
}

bool UCameraVideoStreamerComponent::EnsureJpegFresh(double NowSec, double MinIntervalSec)
{
	const double SafeMinIntervalSec = FMath::Max(0.0, MinIntervalSec);

	{
		FScopeLock Lock(&JpegMutex);
		if (LatestJpeg.Num() > 0 && LastJpegProducedSec > 0.0 && (NowSec - LastJpegProducedSec) < SafeMinIntervalSec)
		{
			return false;
		}
	}

	const double PreviousProducedSec = LastJpegProducedSec;
	UpdateCachedJpeg();
	return LastJpegProducedSec > PreviousProducedSec;
}

void UCameraVideoStreamerComponent::UpdateStreamingState(bool bWantVideo, bool bWantJpeg)
{
	const bool bNewVideo = bWantVideo;
	const bool bNewJpeg = bWantJpeg;
	const bool bChanged = (bVideoStreamingEnabled != bNewVideo) || (bJpegStreamingEnabled != bNewJpeg);

	bVideoStreamingEnabled = bNewVideo;
	bJpegStreamingEnabled = bNewJpeg;

	if (bChanged)
	{
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer demand state: video=%s jpeg=%s"),
			bVideoStreamingEnabled ? TEXT("on") : TEXT("off"),
			bJpegStreamingEnabled ? TEXT("on") : TEXT("off"));
	}

	RefreshCaptureTimers();
}

int32 UCameraVideoStreamerComponent::GetEffectiveJpegFps() const
{
	return FMath::Clamp(JpegFps, 0, 120);
}

bool UCameraVideoStreamerComponent::IsJpegDemandActiveNow() const
{
	if (LastJpegRequestTimeSec <= 0.0)
	{
		return false;
	}

	const double NowSec = FPlatformTime::Seconds();
	return (NowSec - LastJpegRequestTimeSec) < JpegDemandTimeoutSec;
}

void UCameraVideoStreamerComponent::StartCaptureTimers()
{
	RefreshCaptureTimers();
}

void UCameraVideoStreamerComponent::StopCaptureTimers()
{
	if (!GetWorld())
	{
		return;
	}

	GetWorld()->GetTimerManager().ClearTimer(CaptureTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(JpegTimerHandle);
}

void UCameraVideoStreamerComponent::RefreshCaptureTimers()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FTimerManager& TimerManager = World->GetTimerManager();

	const bool bWantCaptureTimer = bVideoStreamingEnabled && TargetFps > 0 && bEncoderReady;
	const bool bHasCaptureTimer = TimerManager.IsTimerActive(CaptureTimerHandle);
	if (bWantCaptureTimer && !bHasCaptureTimer)
	{
		const float Interval = 1.0f / static_cast<float>(TargetFps);
		TimerManager.SetTimer(CaptureTimerHandle, this, &UCameraVideoStreamerComponent::CaptureAndEncodeFrame, Interval, true);
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer timer enabled: video %dx%d targetFps=%d bitrate=%d interval=%.4fs"),
			CachedWidth.Load() > 0 ? CachedWidth.Load() : TargetWidth,
			CachedHeight.Load() > 0 ? CachedHeight.Load() : TargetHeight,
			TargetFps,
			TargetBitrate,
			Interval);
	}
	else if (!bWantCaptureTimer && bHasCaptureTimer)
	{
		TimerManager.ClearTimer(CaptureTimerHandle);
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer timer disabled: video"));
	}

	const bool bWantJpegTimer = false; // JPEG is on-demand from /camera.jpg
	const bool bHasJpegTimer = TimerManager.IsTimerActive(JpegTimerHandle);
	if (bWantJpegTimer && !bHasJpegTimer)
	{
		const int32 EffectiveJpegFps = GetEffectiveJpegFps();
		const float JpegInterval = 1.0f / static_cast<float>(FMath::Max(1, EffectiveJpegFps));
		TimerManager.SetTimer(JpegTimerHandle, this, &UCameraVideoStreamerComponent::UpdateCachedJpeg, JpegInterval, true);
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer timer enabled: jpeg fps=%d (requested=%d) interval=%.4fs"), EffectiveJpegFps, JpegFps, JpegInterval);
	}
	else if (!bWantJpegTimer && bHasJpegTimer)
	{
		TimerManager.ClearTimer(JpegTimerHandle);
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer timer disabled: jpeg"));
	}
}

void UCameraVideoStreamerComponent::CaptureAndEncodeFrame()
{
	if (!SceneCapture || !CaptureTarget || !bEncoderReady)
	{
		return;
	}

	{
		FScopeLock Lock(&QueueMutex);
		const int32 NearFullThreshold = (QueueSize > 0) ? FMath::Max(1, FMath::CeilToInt(static_cast<float>(QueueSize) * 0.7f)) : 0;
		if (NearFullThreshold > 0 && RingCount >= NearFullThreshold)
		{
			const double NowSec = FPlatformTime::Seconds();
			if ((NowSec - LastBackpressureLogSec) >= 1.0)
			{
				LastBackpressureLogSec = NowSec;
				UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: backpressure drop before capture (queue=%d/%d, threshold=%d)"),
					RingCount, QueueSize, NearFullThreshold);
			}
			return;
		}
	}

	SceneCapture->CaptureScene();
	LastSceneCaptureSeconds = FPlatformTime::Seconds();
	FTextureRenderTargetResource* Resource = CaptureTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}

	FTextureRHIRef SourceTexture = Resource->GetRenderTargetTexture();
	if (!SourceTexture.IsValid())
	{
		return;
	}

	TWeakObjectPtr<UCameraVideoStreamerComponent> WeakThis(this);
	ENQUEUE_RENDER_COMMAND(SkySightCaptureEncode)(
		[WeakThis, SourceTexture](FRHICommandListImmediate& RHICmdList)
		{
			if (!WeakThis.IsValid())
			{
				return;
			}
			WeakThis->EncodeOnRenderThread(SourceTexture);
		});
}

void UCameraVideoStreamerComponent::UpdateCachedJpeg()
{
	if (!SceneCapture || !CaptureTarget)
	{
		return;
	}

	const int32 EffectiveJpegFps = GetEffectiveJpegFps();

	if (RuntimeJpegQuality <= 0)
	{
		RuntimeJpegQuality = FMath::Clamp(JpegQuality, 1, 100);
	}

	const double FrameStartSec = FPlatformTime::Seconds();
	bool bDidCaptureThisJpegTick = false;
	if (!bReuseLatestCaptureForJpeg)
	{
		SceneCapture->CaptureScene();
		LastSceneCaptureSeconds = FPlatformTime::Seconds();
		bDidCaptureThisJpegTick = true;
	}
	else
	{
		const bool bCanReuseEncoderCapture = (TargetFps > 0 && bEncoderReady);
		const double NowSec = FPlatformTime::Seconds();
		const double EncoderFrameIntervalSec = bCanReuseEncoderCapture ? (1.0 / static_cast<double>(FMath::Max(1, TargetFps))) : 0.0;
		const double ReuseWindowSec = EncoderFrameIntervalSec > 0.0 ? (EncoderFrameIntervalSec * 0.9) : 0.0;
		const bool bHasRecentCapture = bCanReuseEncoderCapture && LastSceneCaptureSeconds > 0.0 && ((NowSec - LastSceneCaptureSeconds) <= ReuseWindowSec);

		if (!bHasRecentCapture)
		{
			SceneCapture->CaptureScene();
			LastSceneCaptureSeconds = FPlatformTime::Seconds();
			bDidCaptureThisJpegTick = true;
		}
	}
	const double AfterCaptureSec = FPlatformTime::Seconds();
	if (!bDisableJpegFlushRenderingCommands)
	{
		FlushRenderingCommands();
	}

	FTextureRenderTargetResource* RenderTargetResource = CaptureTarget->GameThread_GetRenderTargetResource();
	if (!RenderTargetResource)
	{
		return;
	}

	JpegReadbackScratch.Reset();
	if (!RenderTargetResource->ReadPixels(JpegReadbackScratch) || JpegReadbackScratch.Num() == 0)
	{
		return;
	}
	const double AfterReadbackSec = FPlatformTime::Seconds();

	if (IsSolidColorFrame(JpegReadbackScratch))
	{
		static bool bLoggedSolidFrameDiscard = false;
		if (!bLoggedSolidFrameDiscard)
		{
			const FColor Color = JpegReadbackScratch[0];
			UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: discarded solid-color JPEG source frame (%d,%d,%d,%d); keeping previous JPEG"),
				Color.R, Color.G, Color.B, Color.A);
			bLoggedSolidFrameDiscard = true;
		}
		return;
	}

	IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = WrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!ImageWrapper.IsValid())
	{
		return;
	}

	const int32 Width = CaptureTarget->SizeX;
	const int32 Height = CaptureTarget->SizeY;
	const int32 RawBytes = JpegReadbackScratch.Num() * sizeof(FColor);
	if (!ImageWrapper->SetRaw(JpegReadbackScratch.GetData(), RawBytes, Width, Height, ERGBFormat::BGRA, 8))
	{
		return;
	}

	const int32 EffectiveJpegQuality = FMath::Clamp(RuntimeJpegQuality, 1, 100);
	const TArray64<uint8> Compressed64 = ImageWrapper->GetCompressed(EffectiveJpegQuality);
	TArray<uint8> Compressed;
	if (Compressed64.Num() > 0)
	{
		Compressed.SetNumUninitialized(static_cast<int32>(Compressed64.Num()));
		FMemory::Memcpy(Compressed.GetData(), Compressed64.GetData(), Compressed64.Num());
	}

	if (Compressed.Num() == 0)
	{
		return;
	}
	const double AfterJpegSec = FPlatformTime::Seconds();

	{
		FScopeLock Lock(&JpegMutex);
		LatestJpeg = MoveTemp(Compressed);
	}
	LastJpegProducedSec = AfterJpegSec;

	static bool bLoggedFirstJpeg = false;
	if (!bLoggedFirstJpeg)
	{
		bLoggedFirstJpeg = true;
		UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer: JPEG cache primed"));
	}

	const double CaptureMs = bDidCaptureThisJpegTick ? ((AfterCaptureSec - FrameStartSec) * 1000.0) : 0.0;
	const double ReadbackMs = (AfterReadbackSec - AfterCaptureSec) * 1000.0;
	const double JpegMs = (AfterJpegSec - AfterReadbackSec) * 1000.0;
	const double TotalMs = (AfterJpegSec - FrameStartSec) * 1000.0;
	const bool bHasJpegTargetBudget = EffectiveJpegFps > 0;
	const double TargetFrameMs = bHasJpegTargetBudget ? (1000.0 / static_cast<double>(EffectiveJpegFps)) : 0.0;

	JpegPerfAccumCaptureMs += CaptureMs;
	JpegPerfAccumReadbackMs += ReadbackMs;
	JpegPerfAccumJpegMs += JpegMs;
	JpegPerfAccumTotalMs += TotalMs;
	++JpegPerfAccumFrames;

	if (bAutoAdjustJpegQuality && bHasJpegTargetBudget)
	{
		const int32 BaseQuality = FMath::Clamp(JpegQuality, 1, 100);
		const int32 MinQualityClamped = FMath::Min(BaseQuality, FMath::Clamp(MinJpegQuality, 1, 100));
		const bool bOverBudget = TotalMs > (TargetFrameMs * 1.10);
		const bool bUnderBudget = TotalMs < (TargetFrameMs * 0.75);

		if (bOverBudget)
		{
			++JpegOverBudgetFrameCount;
			JpegUnderBudgetFrameCount = 0;
			if (JpegOverBudgetFrameCount >= 10 && RuntimeJpegQuality > MinQualityClamped)
			{
				const int32 Prev = RuntimeJpegQuality;
				RuntimeJpegQuality = FMath::Max(MinQualityClamped, RuntimeJpegQuality - 5);
				JpegOverBudgetFrameCount = 0;
				UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: JPEG quality reduced %d -> %d (%.2fms > %.2fms budget @ %d fps, %dx%d)"),
					Prev, RuntimeJpegQuality, TotalMs, TargetFrameMs, EffectiveJpegFps, Width, Height);
			}
		}
		else if (bUnderBudget)
		{
			++JpegUnderBudgetFrameCount;
			JpegOverBudgetFrameCount = 0;
			if (JpegUnderBudgetFrameCount >= 60 && RuntimeJpegQuality < BaseQuality)
			{
				const int32 Prev = RuntimeJpegQuality;
				RuntimeJpegQuality = FMath::Min(BaseQuality, RuntimeJpegQuality + 5);
				JpegUnderBudgetFrameCount = 0;
				UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer: JPEG quality restored %d -> %d"), Prev, RuntimeJpegQuality);
			}
		}
		else
		{
			JpegOverBudgetFrameCount = 0;
			JpegUnderBudgetFrameCount = 0;
		}
	}

	const double NowSec = AfterJpegSec;
	if (JpegPerfLogIntervalSec > 0.0 && (NowSec - JpegPerfLastLogSeconds) >= JpegPerfLogIntervalSec && JpegPerfAccumFrames > 0)
	{
		const double InvN = 1.0 / static_cast<double>(JpegPerfAccumFrames);
		if (bHasJpegTargetBudget)
		{
			UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer JPEG perf avg %d frames: capture=%.2fms readback=%.2fms jpeg=%.2fms total=%.2fms (target %.2fms @ %d fps, q=%d, %dx%d, reuse=%d)"),
				JpegPerfAccumFrames,
				JpegPerfAccumCaptureMs * InvN,
				JpegPerfAccumReadbackMs * InvN,
				JpegPerfAccumJpegMs * InvN,
				JpegPerfAccumTotalMs * InvN,
				TargetFrameMs,
				EffectiveJpegFps,
				EffectiveJpegQuality,
				Width,
				Height,
				bReuseLatestCaptureForJpeg ? 1 : 0);
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("CameraVideoStreamer JPEG perf avg %d frames: capture=%.2fms readback=%.2fms jpeg=%.2fms total=%.2fms (on-demand, q=%d, %dx%d, reuse=%d)"),
				JpegPerfAccumFrames,
				JpegPerfAccumCaptureMs * InvN,
				JpegPerfAccumReadbackMs * InvN,
				JpegPerfAccumJpegMs * InvN,
				JpegPerfAccumTotalMs * InvN,
				EffectiveJpegQuality,
				Width,
				Height,
				bReuseLatestCaptureForJpeg ? 1 : 0);
		}
		JpegPerfLastLogSeconds = NowSec;
		JpegPerfAccumCaptureMs = 0.0;
		JpegPerfAccumReadbackMs = 0.0;
		JpegPerfAccumJpegMs = 0.0;
		JpegPerfAccumTotalMs = 0.0;
		JpegPerfAccumFrames = 0;
	}
}

void UCameraVideoStreamerComponent::ApplyCaptureSettings(int32 Width, int32 Height)
{
	if (!SceneCapture)
	{
		return;
	}

	if (!CaptureTarget)
	{
		CaptureTarget = NewObject<UTextureRenderTarget2D>(GetOwner(), TEXT("DroneCapture_RT"));
	}

	if (CaptureTarget)
	{
		if (bUseLdrFinalColorForJpegAndVideo)
		{
			CaptureTarget->RenderTargetFormat = RTF_RGBA8;
			CaptureTarget->TargetGamma = CaptureTargetGamma;
			CaptureTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, false);
		}
		else
		{
			CaptureTarget->RenderTargetFormat = RTF_RGBA16f;
			CaptureTarget->TargetGamma = CaptureTargetGamma;
			CaptureTarget->InitAutoFormat(Width, Height);
		}
		CaptureTarget->ClearColor = FLinearColor::Black;
		CaptureTarget->UpdateResourceImmediate(true);
		JpegReadbackScratch.Reserve(FMath::Max(1, Width * Height));
	}

	CachedWidth.Store(Width);
	CachedHeight.Store(Height);

	SceneCapture->TextureTarget = CaptureTarget;
	SceneCapture->CaptureSource = bUseLdrFinalColorForJpegAndVideo
		? ESceneCaptureSource::SCS_FinalColorLDR
		: ESceneCaptureSource::SCS_FinalColorHDR;
	SceneCapture->bCaptureEveryFrame = false;
	SceneCapture->bCaptureOnMovement = false;
	SceneCapture->bAlwaysPersistRenderingState = true;
}

bool UCameraVideoStreamerComponent::InitializeEncoder(int32 Width, int32 Height)
{
	if (!GDynamicRHI)
	{
		return false;
	}

	FScopeLock Lock(&EncoderMutex);
	if (!Impl)
	{
		return false;
	}

	if (Impl->VideoEncoder.IsValid())
	{
		return true;
	}

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const ERHIInterfaceType RHIType = RHIGetInterfaceType();
	if (RHIType == ERHIInterfaceType::D3D11)
	{
		Impl->VideoEncoderInput = AVEncoder::FVideoEncoderInput::CreateForD3D11(GDynamicRHI->RHIGetNativeDevice(), true, IsRHIDeviceAMD());
	}
	else if (RHIType == ERHIInterfaceType::D3D12)
	{
		Impl->VideoEncoderInput = AVEncoder::FVideoEncoderInput::CreateForD3D12(GDynamicRHI->RHIGetNativeDevice(), true, IsRHIDeviceNVIDIA());
	}
	else if (RHIType == ERHIInterfaceType::Vulkan)
	{
		Impl->VideoEncoderInput = AVEncoder::FVideoEncoderInput::CreateForVulkan(GDynamicRHI->RHIGetNativeDevice(), true);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: unsupported RHI for video encoding"));
		return false;
	}

	if (!Impl->VideoEncoderInput.IsValid())
	{
		return false;
	}

	Impl->VideoEncoderInput->SetMaxNumBuffers(3);

	AVEncoder::FVideoEncoder::FLayerConfig Config;
	Config.Width = Width;
	Config.Height = Height;
	Config.MaxFramerate = TargetFps;
	Config.MaxBitrate = TargetBitrate;
	Config.TargetBitrate = TargetBitrate;
	Config.RateControlMode = AVEncoder::FVideoEncoder::RateControlMode::CBR;
	Config.H264Profile = AVEncoder::FVideoEncoder::H264Profile::BASELINE;

	const TArray<AVEncoder::FVideoEncoderInfo>& AvailableEncoders = AVEncoder::FVideoEncoderFactory::Get().GetAvailable();
	for (const AVEncoder::FVideoEncoderInfo& EncoderInfo : AvailableEncoders)
	{
		if (EncoderInfo.CodecType == AVEncoder::ECodecType::H264)
		{
			Impl->VideoEncoder = AVEncoder::FVideoEncoderFactory::Get().Create(EncoderInfo.ID, Impl->VideoEncoderInput, Config);
			if (Impl->VideoEncoder)
			{
				break;
			}
		}
	}

	if (!Impl->VideoEncoder)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraVideoStreamer: no H264 encoder available"));
		return false;
	}

	Impl->VideoEncoder->SetOnEncodedPacket(
		[this](uint32 LayerIndex, const TSharedPtr<AVEncoder::FVideoEncoderInputFrame> Frame, const AVEncoder::FCodecPacket& Packet)
		{
			HandleEncodedPacket(LayerIndex, Frame, Packet);
		});
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	const double PtsStep = TargetFps > 0 ? (90000.0 / static_cast<double>(TargetFps)) : 6000.0;
	{
		FScopeLock MuxerLock(&MuxerMutex);
		if (!Impl->TsMuxer.IsValid())
		{
			Impl->TsMuxer = MakeUnique<FMpegTsMuxer>();
		}
		Impl->TsMuxer->Reset(PtsStep);
	}

	{
		FScopeLock QueueLock(&QueueMutex);
		PacketRing.Reset();
		RingStart = 0;
		RingCount = 0;
	}

	bEncoderReady = true;
	return true;
}

void UCameraVideoStreamerComponent::ShutdownEncoder()
{
	FScopeLock Lock(&EncoderMutex);

	bEncoderReady = false;
	if (!Impl)
	{
		return;
	}

	if (Impl->VideoEncoder.IsValid())
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Impl->VideoEncoder->Shutdown();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		Impl->VideoEncoder.Reset();
	}

	Impl->VideoEncoderInput.Reset();
	Impl->BackBuffers.Empty();
}

void UCameraVideoStreamerComponent::EncodeOnRenderThread(const FTextureRHIRef& SourceTexture)
{
	FScopeLock Lock(&EncoderMutex);
	if (!Impl || !Impl->VideoEncoder.IsValid() || !Impl->VideoEncoderInput.IsValid())
	{
		return;
	}

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	TSharedPtr<AVEncoder::FVideoEncoderInputFrame> InputFrame = ObtainInputFrame();
	InputFrame->SetTimestampUs(static_cast<int64>(FPlatformTime::Seconds() * 1000000.0));
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	if (!Impl->BackBuffers.Contains(InputFrame))
	{
		return;
	}

	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
	TransitionAndCopyTexture(RHICmdList, SourceTexture, Impl->BackBuffers[InputFrame], {});

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	AVEncoder::FVideoEncoder::FEncodeOptions Options;
	Impl->VideoEncoder->Encode(InputFrame, Options);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void UCameraVideoStreamerComponent::HandleEncodedPacket(uint32 LayerIndex, const TSharedPtr<AVEncoder::FVideoEncoderInputFrame> Frame, const AVEncoder::FCodecPacket& Packet)
{
	(void)LayerIndex;

	TArray<uint8> H264Data;
	if (Packet.DataSize > 0)
	{
		H264Data.Append(Packet.Data.Get(), Packet.DataSize);
	}

	if (H264Data.Num() == 0)
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		if (Frame.IsValid())
		{
			Frame->Release();
		}
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		return;
	}

	TArray<uint8> TsData;
	{
		FScopeLock MuxerLock(&MuxerMutex);
		if (!Impl || !Impl->TsMuxer.IsValid())
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			if (Frame.IsValid())
			{
				Frame->Release();
			}
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			return;
		}

		if (Packet.IsKeyFrame)
		{
			Impl->TsMuxer->BuildPatPmt(TsData);
		}
		Impl->TsMuxer->BuildPes(H264Data, TsData);
	}

	PushPacket(MoveTemp(TsData));

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (Frame.IsValid())
	{
		Frame->Release();
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
TSharedPtr<AVEncoder::FVideoEncoderInputFrame> UCameraVideoStreamerComponent::ObtainInputFrame()
PRAGMA_ENABLE_DEPRECATION_WARNINGS
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	TSharedPtr<AVEncoder::FVideoEncoderInputFrame> InputFrame = Impl->VideoEncoderInput->ObtainInputFrame();
	const int32 CachedWidthValue = CachedWidth.Load();
	const int32 CachedHeightValue = CachedHeight.Load();
	const int32 Width = CachedWidthValue > 0 ? CachedWidthValue : TargetWidth;
	const int32 Height = CachedHeightValue > 0 ? CachedHeightValue : TargetHeight;
	InputFrame->SetWidth(Width);
	InputFrame->SetHeight(Height);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	if (!Impl->BackBuffers.Contains(InputFrame))
	{
#if PLATFORM_WINDOWS && PLATFORM_DESKTOP
		const ERHIInterfaceType RHIType = RHIGetInterfaceType();

		const FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(TEXT("CameraVideoStreamerBackBuffer"), Width, Height, PF_B8G8R8A8)
			.SetFlags(ETextureCreateFlags::Shared | ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::UAV)
			.SetInitialState(ERHIAccess::CopyDest);

		FTextureRHIRef Texture = RHICreateTexture(Desc);

		TWeakObjectPtr<UCameraVideoStreamerComponent> WeakThis(this);

		if (RHIType == ERHIInterfaceType::D3D11)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			InputFrame->SetTexture(static_cast<ID3D11Texture2D*>(Texture->GetNativeResource()), [WeakThis, InputFrame](ID3D11Texture2D*)
			{
				if (!WeakThis.IsValid()) return;
				if (!WeakThis->Impl) return;
				WeakThis->Impl->BackBuffers.Remove(InputFrame);
			});
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			Impl->BackBuffers.Add(InputFrame, Texture);
		}
		else if (RHIType == ERHIInterfaceType::D3D12)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			InputFrame->SetTexture(static_cast<ID3D12Resource*>(Texture->GetNativeResource()), [WeakThis, InputFrame](ID3D12Resource*)
			{
				if (!WeakThis.IsValid()) return;
				if (!WeakThis->Impl) return;
				WeakThis->Impl->BackBuffers.Remove(InputFrame);
			});
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			Impl->BackBuffers.Add(InputFrame, Texture);
		}
#else
		unimplemented();
#endif
	}

	return InputFrame;
}

void UCameraVideoStreamerComponent::PushPacket(TArray<uint8>&& Packet)
{
	FScopeLock Lock(&QueueMutex);

	if (QueueSize <= 0)
	{
		return;
	}

	if (PacketRing.Num() != QueueSize)
	{
		PacketRing.SetNum(QueueSize);
		RingStart = 0;
		RingCount = 0;
	}

	int32 Index = (RingStart + RingCount) % QueueSize;
	if (RingCount == QueueSize)
	{
		Index = RingStart;
		RingStart = (RingStart + 1) % QueueSize;
	}
	else
	{
		RingCount++;
	}

	PacketRing[Index] = MoveTemp(Packet);
}

// если у тебя реально есть LoadConfig() где-то в другом месте — удали это.
// если нет — оставь так, чтобы компилилось.
void UCameraVideoStreamerComponent::LoadConfig()
{
	// noop: config properties are already UPROPERTY(Config)
}
