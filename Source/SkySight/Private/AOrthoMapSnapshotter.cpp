#include "AOrthoMapSnapshotter.h"

#include "ASimWorldManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Math/UnrealMathUtility.h"

#include "Modules/ModuleManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

AOrthoMapSnapshotter::AOrthoMapSnapshotter()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneCapture = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("SceneCapture"));
	SetRootComponent(SceneCapture);

	SceneCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	SceneCapture->ProjectionType = ECameraProjectionMode::Orthographic;
	SceneCapture->bCaptureEveryFrame = false;
	SceneCapture->bCaptureOnMovement = false;

	// ВАЖНО: RenderTarget НЕ создаём через NewObject в конструкторе.
	// Создадим его в BeginPlay, чтобы UE не падал.
	RenderTarget = nullptr;
}

void AOrthoMapSnapshotter::BeginPlay()
{
	Super::BeginPlay();

	// Инициализируем ortho ширину уже когда свойства гарантированно установлены
	if (SceneCapture)
	{
		SceneCapture->OrthoWidth = OrthoWidthMeters * 100.0f;
	}

	// Создаём RenderTarget безопасно после конструктора
	if (!RenderTarget)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("MapSnapshot_RT"));
		if (RenderTarget)
		{
			RenderTarget->InitAutoFormat(2048, 2048);
			RenderTarget->ClearColor = FLinearColor::Black;
			RenderTarget->UpdateResourceImmediate(true);

			if (SceneCapture)
			{
				SceneCapture->TextureTarget = RenderTarget;
			}
		}
	}

	CaptureMap();
}

static bool IsValidGeoBounds(double LatMin, double LonMin, double LatMax, double LonMax)
{
	if (!(LatMin < LatMax && LonMin < LonMax))
	{
		return false;
	}
	if (LatMin < -90.0 || LatMax > 90.0)
	{
		return false;
	}
	if (LonMin < -180.0 || LonMax > 180.0)
	{
		return false;
	}
	if ((LatMax - LatMin) < 1e-9 || (LonMax - LonMin) < 1e-9)
	{
		return false;
	}
	return true;
}

static bool ComputeBoundsFromOrigin(UWorld* World, double& OutLatMin, double& OutLonMin, double& OutLatMax, double& OutLonMax, float OrthoWidthMeters)
{
	if (!World)
	{
		return false;
	}

	const ASimWorldManager* Manager = nullptr;
	for (TActorIterator<ASimWorldManager> It(World); It; ++It)
	{
		Manager = *It;
		break;
	}
	if (!Manager)
	{
		return false;
	}

	const double OriginLat = Manager->OriginLatDeg;
	const double OriginLon = Manager->OriginLonDeg;

	const bool bOriginOk =
		(OriginLat >= -90.0 && OriginLat <= 90.0) &&
		(OriginLon >= -180.0 && OriginLon <= 180.0);

	if (!bOriginOk)
	{
		return false;
	}

	const double HalfMeters = FMath::Max(1.0, static_cast<double>(OrthoWidthMeters) * 0.5);
	const double MetersPerDegLat = 111320.0;
	const double LatRad = FMath::DegreesToRadians(OriginLat);
	const double MetersPerDegLon = FMath::Max(1.0, FMath::Cos(LatRad) * 111320.0);

	const double dLat = HalfMeters / MetersPerDegLat;
	const double dLon = HalfMeters / MetersPerDegLon;

	OutLatMin = OriginLat - dLat;
	OutLatMax = OriginLat + dLat;
	OutLonMin = OriginLon - dLon;
	OutLonMax = OriginLon + dLon;

	return IsValidGeoBounds(OutLatMin, OutLonMin, OutLatMax, OutLonMax);
}

bool AOrthoMapSnapshotter::ComputeBounds(double& OutLatMin, double& OutLonMin, double& OutLatMax, double& OutLonMax) const
{
	return ComputeBoundsFromOrigin(GetWorld(), OutLatMin, OutLonMin, OutLatMax, OutLonMax, OrthoWidthMeters);
}

bool AOrthoMapSnapshotter::IsValidGeoBounds(double LatMin, double LonMin, double LatMax, double LonMax)
{
	return ::IsValidGeoBounds(LatMin, LonMin, LatMax, LonMax);
}

void AOrthoMapSnapshotter::CaptureMap()
{
	if (!RenderTarget || !SceneCapture)
	{
		return;
	}

	double LatMin = LatLonMin.X;
	double LonMin = LatLonMin.Y;
	double LatMax = LatLonMax.X;
	double LonMax = LatLonMax.Y;

	// если можно — берём bounds от ASimWorldManager origin (чтобы GUI и сим совпадали)
	{
		double BLatMin = 0.0, BLonMin = 0.0, BLatMax = 0.0, BLonMax = 0.0;
		if (ComputeBoundsFromOrigin(GetWorld(), BLatMin, BLonMin, BLatMax, BLonMax, OrthoWidthMeters))
		{
			LatMin = BLatMin;
			LonMin = BLonMin;
			LatMax = BLatMax;
			LonMax = BLonMax;
		}
	}

	// жёсткая страховка: если bounds мусор — ставим дефолт Seattle
	if (!IsValidGeoBounds(LatMin, LonMin, LatMax, LonMax))
	{
		LatMin = 47.6050;
		LonMin = -122.3340;
		LatMax = 47.6080;
		LonMax = -122.3300;
	}

	SceneCapture->CaptureScene();

	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return;
	}

	TArray<FColor> Bitmap;
	if (!Resource->ReadPixels(Bitmap) || Bitmap.Num() == 0)
	{
		return;
	}

	IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	TSharedPtr<IImageWrapper> Wrapper = WrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid())
	{
		return;
	}

	const int32 Width = RenderTarget->SizeX;
	const int32 Height = RenderTarget->SizeY;
	const int32 RawBytes = Bitmap.Num() * sizeof(FColor);

	if (!Wrapper->SetRaw(Bitmap.GetData(), RawBytes, Width, Height, ERGBFormat::BGRA, 8))
	{
		return;
	}

	// UE 5.5: GetCompressed возвращает TArray64<uint8>
	const TArray64<uint8> Compressed64 = Wrapper->GetCompressed(100);

	// SaveArrayToFile ждёт TArray<uint8>
	TArray<uint8> Compressed;
	if (Compressed64.Num() > 0)
	{
		Compressed.Append(Compressed64.GetData(), static_cast<int32>(Compressed64.Num()));
	}

	const FString OutputPath = FPaths::ProjectSavedDir() / TEXT("skysight_map.png");
	if (!FFileHelper::SaveArrayToFile(Compressed, *OutputPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to save map snapshot to %s"), *OutputPath);
		return;
	}

	SnapshotInfo.ImagePath = OutputPath;
	SnapshotInfo.WidthPx = Width;
	SnapshotInfo.HeightPx = Height;
	SnapshotInfo.LatMin = LatMin;
	SnapshotInfo.LonMin = LonMin;
	SnapshotInfo.LatMax = LatMax;
	SnapshotInfo.LonMax = LonMax;

	UE_LOG(LogTemp, Log, TEXT("Map snapshot saved to %s (lat[%f..%f], lon[%f..%f])"),
		*OutputPath, LatMin, LatMax, LonMin, LonMax);
}
