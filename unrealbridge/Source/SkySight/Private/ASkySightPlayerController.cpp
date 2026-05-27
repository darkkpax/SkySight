#include "ASkySightPlayerController.h"

#include "ASimWorldManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"

void ASkySightPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (InputComponent)
	{
		InputComponent->BindAction(TEXT("ForceSpawnFire"), IE_Pressed, this, &ASkySightPlayerController::OnSpawnFirePressed);
		InputComponent->BindKey(EKeys::F, IE_Pressed, this, &ASkySightPlayerController::OnSpawnFirePressed);
	}
}

void ASkySightPlayerController::OnSpawnFirePressed()
{
	UE_LOG(LogTemp, Log, TEXT("[FireSpawnDebug] F pressed (PlayerController)."));

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: world is null in player controller."));
		return;
	}

	ASimWorldManager* SimWorldManager = Cast<ASimWorldManager>(
		UGameplayStatics::GetActorOfClass(World, ASimWorldManager::StaticClass())
	);
	if (!SimWorldManager)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireSpawnDebug] Failed: ASimWorldManager not found."));
		return;
	}

	SimWorldManager->SpawnFireDebugInFrontOfPlayerOrDrone();
}
