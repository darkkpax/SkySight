#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UUnrealBridgeProtocol.h"
#include "AOrthoMapSnapshotter.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

UCLASS()
class SKYSIGHT_API AOrthoMapSnapshotter : public AActor
{
	GENERATED_BODY()

public:
	AOrthoMapSnapshotter();

	virtual void BeginPlay() override;

	// ручной fallback (если auto bounds не сработали)
	UPROPERTY(EditAnywhere, Category="Capture")
	FVector2D LatLonMin = FVector2D(47.6050, -122.3340);

	UPROPERTY(EditAnywhere, Category="Capture")
	FVector2D LatLonMax = FVector2D(47.6080, -122.3300);

	UPROPERTY(EditAnywhere, Category="Capture")
	float OrthoWidthMeters = 1000.0f;

	UPROPERTY(EditAnywhere, Category="Capture")
	float OrthoHeightMeters = 1000.0f;

	// новый режим: bounds вычисляются от origin из ASimWorldManager
	UPROPERTY(EditAnywhere, Category="Geo")
	bool bAutoBoundsFromSimWorldManager = true;

	// если менеджера нет — можно задать origin вручную
	UPROPERTY(EditAnywhere, Category="Geo")
	double OverrideOriginLatDeg = 47.6062;

	UPROPERTY(EditAnywhere, Category="Geo")
	double OverrideOriginLonDeg = -122.3321;

	const FMapSnapshotInfo& GetSnapshotInfo() const { return SnapshotInfo; }

protected:
	UPROPERTY(VisibleAnywhere)
	USceneCaptureComponent2D* SceneCapture;

	UPROPERTY()
	UTextureRenderTarget2D* RenderTarget;

	FMapSnapshotInfo SnapshotInfo;

	void CaptureMap();

private:
	bool ComputeBounds(double& OutLatMin, double& OutLonMin, double& OutLatMax, double& OutLonMax) const;
	static bool IsValidGeoBounds(double LatMin, double LonMin, double LatMax, double LonMax);
};
