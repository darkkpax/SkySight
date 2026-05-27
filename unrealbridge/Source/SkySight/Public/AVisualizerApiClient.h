// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AVisualizerApiClient.generated.h"

UCLASS()
class SKYSIGHT_API AAVisualizerApiClient : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAVisualizerApiClient();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

};
