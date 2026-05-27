#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AFireSourceActor.generated.h"

UCLASS()
class SKYSIGHT_API AFireSourceActor : public AActor
{
	GENERATED_BODY()

public:
	AFireSourceActor();

	virtual void BeginPlay() override;
	void ForceVisibleForCapture();

	const FString& GetFireId() const { return FireId; }
	void SetFireId(const FString& InId) { FireId = InId; }

protected:
	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* VisualMesh;

	UPROPERTY(EditAnywhere, Category="Fire")
	float RadiusCm = 150.0f;

	UPROPERTY(BlueprintReadOnly, Category="Fire")
	FString FireId;
};
