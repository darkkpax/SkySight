#include "AFireSourceActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"

AFireSourceActor::AFireSourceActor()
{
	PrimaryActorTick.bCanEverTick = false;

	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visual"));
	RootComponent = VisualMesh;
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	VisualMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	VisualMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	VisualMesh->SetGenerateOverlapEvents(false);
}

void AFireSourceActor::BeginPlay()
{
	Super::BeginPlay();
	ForceVisibleForCapture();

	if (VisualMesh)
	{
		if (UMaterialInterface* Material = VisualMesh->GetMaterial(0))
		{
			UMaterialInstanceDynamic* Dyn = VisualMesh->CreateDynamicMaterialInstance(0, Material);
			if (Dyn)
			{
				Dyn->SetVectorParameterValue(TEXT("EmissiveColor"), FLinearColor::Red);
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("Fire %s active at %s"), *FireId, *GetActorLocation().ToString());
}

void AFireSourceActor::ForceVisibleForCapture()
{
	SetActorHiddenInGame(false);

	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(this);
	for (UPrimitiveComponent* Primitive : PrimitiveComponents)
	{
		if (!Primitive)
		{
			continue;
		}

		Primitive->SetHiddenInGame(false, true);
		Primitive->SetVisibility(true, true);
		Primitive->bOwnerNoSee = false;
		Primitive->bOnlyOwnerSee = false;

	}
}
