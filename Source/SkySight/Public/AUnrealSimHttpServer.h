#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AUnrealSimHttpServer.generated.h"

class FSocket;
class ADronePawn;
class ASimWorldManager;
class AOrthoMapSnapshotter;
class UCameraVideoStreamerComponent;
struct FSkysightRoute;

UCLASS()
class SKYSIGHT_API AUnrealSimHttpServer : public AActor
{
	GENERATED_BODY()

public:
	AUnrealSimHttpServer();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(EditAnywhere, Category="Server")
	int32 Port = 9000;

protected:
	FSocket* ListenerSocket = nullptr;
	ADronePawn* DronePawn = nullptr;
	TAtomic<int32> ActiveVideoClients{0};
	double LastJpegRequestSec = 0.0;
	UPROPERTY(EditAnywhere, Category="Server|Camera")
	float JpegActiveRecentWindowSec = 1.5f;

	void StartServer();
	void StopServer();
	bool HandleClient(FSocket* ClientSocket);
	bool ReadFullHttpRequest(FSocket* Socket, FString& OutHeaders, TArray<uint8>& OutBodyBytes);
	static void SendString(FSocket* Socket, const FString& Data);
	void SendJsonResponse(FSocket* Socket, int32 StatusCode, const FString& Body);
	void SendJsonResponseUtf8(FSocket* Socket, int32 StatusCode, const FString& JsonString);
	static void SendBinaryResponse(FSocket* Socket, const TArray<uint8>& Data, const FString& ContentType);
	void ProcessRequest(const FString& Method, const FString& Path, const FString& Query, const FString& Body, FSocket* Socket, bool& bCloseSocket);
	ADronePawn* FindDronePawn();
	ASimWorldManager* FindWorldManager();
	AOrthoMapSnapshotter* FindSnapshotter();
	void EnsureDroneSpawnedAndPossessed();
	void EnsureDroneSpawnedAndPossessedAtRouteStart(const FSkysightRoute& Route);
	void RefreshCameraStreamingState();
	void SetPlanningCameraView();
	void StreamVideoTs(FSocket* Socket, TWeakObjectPtr<UCameraVideoStreamerComponent> Streamer);
	bool SendChunk(FSocket* Socket, const TArray<uint8>& Data);
};
