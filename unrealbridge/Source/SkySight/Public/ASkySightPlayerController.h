#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ASkySightPlayerController.generated.h"

UCLASS()
class SKYSIGHT_API ASkySightPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void SetupInputComponent() override;

private:
	UFUNCTION()
	void OnSpawnFirePressed();
};
