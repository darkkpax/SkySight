#include "AUnrealSimHttpServer.h"
#include "ADronePawn.h"
#include "ASimWorldManager.h"
#include "AOrthoMapSnapshotter.h"
#include "CameraVideoStreamerComponent.h"
#include "UUnrealBridgeProtocol.h"
#include "Async/Async.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "EngineUtils.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Containers/StringConv.h"
#include "Engine/World.h"
#include "Camera/CameraActor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	int32 FindHeaderEnd(const TArray<uint8>& Data)
	{
		if (Data.Num() < 4)
		{
			return INDEX_NONE;
		}

		for (int32 Index = 0; Index <= Data.Num() - 4; ++Index)
		{
			if (Data[Index] == '\r' && Data[Index + 1] == '\n' && Data[Index + 2] == '\r' && Data[Index + 3] == '\n')
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	int32 ParseContentLength(const FString& Headers)
	{
		TArray<FString> Lines;
		Headers.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("Content-Length"), ESearchCase::IgnoreCase))
			{
				FString Key;
				FString Value;
				if (Line.Split(TEXT(":"), &Key, &Value))
				{
					return FMath::Max(0, FCString::Atoi(*Value.TrimStartAndEnd()));
				}
			}
		}

		return 0;
	}

	FString FormatIso8601FromEpochSeconds(double EpochSeconds)
	{
		const int64 WholeSeconds = static_cast<int64>(EpochSeconds);
		const double Fractional = EpochSeconds - static_cast<double>(WholeSeconds);
		const int32 Millis = FMath::Clamp(FMath::RoundToInt(Fractional * 1000.0), 0, 999);

		FDateTime DateTime = FDateTime::FromUnixTimestamp(WholeSeconds);
		if (Millis > 0)
		{
			DateTime += FTimespan::FromMilliseconds(Millis);
		}

		return FString::Printf(TEXT("%s.%03dZ"), *DateTime.ToString(TEXT("%Y-%m-%dT%H:%M:%S")), DateTime.GetMillisecond());
	}

	int64 GetUnixTimestampMs()
	{
		const FDateTime Now = FDateTime::UtcNow();
		return (Now.ToUnixTimestamp() * 1000LL) + static_cast<int64>(Now.GetMillisecond());
	}

	FString BuildMapInfoJson(const FMapSnapshotInfo& Info)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

		// preferred snake_case fields
		Root->SetNumberField(TEXT("lat_min"), Info.LatMin);
		Root->SetNumberField(TEXT("lon_min"), Info.LonMin);
		Root->SetNumberField(TEXT("lat_max"), Info.LatMax);
		Root->SetNumberField(TEXT("lon_max"), Info.LonMax);
		Root->SetNumberField(TEXT("width_px"), Info.WidthPx);
		Root->SetNumberField(TEXT("height_px"), Info.HeightPx);
		Root->SetStringField(TEXT("image_path"), Info.ImagePath);

		// legacy PascalCase fields (kept for compatibility)
		Root->SetNumberField(TEXT("LatMin"), Info.LatMin);
		Root->SetNumberField(TEXT("LonMin"), Info.LonMin);
		Root->SetNumberField(TEXT("LatMax"), Info.LatMax);
		Root->SetNumberField(TEXT("LonMax"), Info.LonMax);
		Root->SetNumberField(TEXT("WidthPx"), Info.WidthPx);
		Root->SetNumberField(TEXT("HeightPx"), Info.HeightPx);
		Root->SetStringField(TEXT("ImagePath"), Info.ImagePath);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	bool TryParseCommandType(const FString& CommandString, ESkysightCommandType& OutType)
	{
		const FString Upper = CommandString.ToUpper();
		if (Upper == TEXT("DESPAWN") || Upper == TEXT("RESET_SIM") || Upper == TEXT("END_MISSION"))
		{
			OutType = ESkysightCommandType::DESPAWN;
			return true;
		}
		if (Upper == TEXT("RESET"))
		{
			OutType = ESkysightCommandType::RESET;
			return true;
		}
		if (Upper == TEXT("HOLD"))
		{
			OutType = ESkysightCommandType::HOLD;
			return true;
		}
		if (Upper == TEXT("PAUSE") || Upper == TEXT("STOP"))
		{
			OutType = ESkysightCommandType::HOLD;
			return true;
		}
		if (Upper == TEXT("RESUME"))
		{
			OutType = ESkysightCommandType::RESUME;
			return true;
		}
		if (Upper == TEXT("CONTINUE") || Upper == TEXT("UNPAUSE"))
		{
			OutType = ESkysightCommandType::RESUME;
			return true;
		}
		if (Upper == TEXT("RTL"))
		{
			OutType = ESkysightCommandType::RTL;
			return true;
		}
		if (Upper == TEXT("SET_SPEED"))
		{
			OutType = ESkysightCommandType::SET_SPEED;
			return true;
		}
		if (Upper == TEXT("SET_VELOCITY"))
		{
			OutType = ESkysightCommandType::SET_VELOCITY;
			return true;
		}
		if (Upper == TEXT("CLEAR_VELOCITY_OVERRIDE") || Upper == TEXT("CLEAR_SPEED_OVERRIDE"))
		{
			OutType = ESkysightCommandType::CLEAR_VELOCITY_OVERRIDE;
			return true;
		}
		if (Upper == TEXT("SET_MODE"))
		{
			OutType = ESkysightCommandType::SET_MODE;
			return true;
		}
		if (Upper == TEXT("ORBIT") || Upper == TEXT("ORBIT_START") || Upper == TEXT("START_ORBIT"))
		{
			OutType = ESkysightCommandType::ORBIT_START;
			return true;
		}
		if (Upper == TEXT("ORBIT_STOP") || Upper == TEXT("STOP_ORBIT") || Upper == TEXT("END_ORBIT")
			|| Upper == TEXT("RESTORE_ROUTE") || Upper == TEXT("RETURN_TO_ROUTE"))
		{
			OutType = ESkysightCommandType::ORBIT_STOP;
			return true;
		}

		return false;
	}

	bool IsFinite(double Value)
	{
		return FMath::IsFinite(Value);
	}

	double SanitizeFinite(double Value, double DefaultValue)
	{
		return IsFinite(Value) ? Value : DefaultValue;
	}

	double ClampLatitude(double LatitudeDeg, double DefaultValue)
	{
		const double Value = SanitizeFinite(LatitudeDeg, DefaultValue);
		return FMath::Clamp(Value, -90.0, 90.0);
	}

	double ClampLongitude(double LongitudeDeg, double DefaultValue)
	{
		const double Value = SanitizeFinite(LongitudeDeg, DefaultValue);
		return FMath::Clamp(Value, -180.0, 180.0);
	}

	bool TryGetQueryParamValue(const FString& Query, const FString& Key, FString& OutValue)
	{
		if (Query.IsEmpty())
		{
			return false;
		}

		TArray<FString> Pairs;
		Query.ParseIntoArray(Pairs, TEXT("&"), true);
		for (const FString& Pair : Pairs)
		{
			FString ParamKey;
			FString ParamValue;
			if (Pair.Split(TEXT("="), &ParamKey, &ParamValue))
			{
				if (ParamKey.Equals(Key, ESearchCase::IgnoreCase))
				{
					OutValue = ParamValue;
					return true;
				}
			}
			else if (Pair.Equals(Key, ESearchCase::IgnoreCase))
			{
				OutValue.Empty();
				return true;
			}
		}

		return false;
	}

	double ParseRequestedHz(const FString& Query, double DefaultHz)
	{
		FString HzValue;
		if (!TryGetQueryParamValue(Query, TEXT("hz"), HzValue))
		{
			return DefaultHz;
		}

		const double Parsed = FCString::Atod(*HzValue);
		if (!FMath::IsFinite(Parsed) || Parsed <= 0.0)
		{
			return DefaultHz;
		}

		return FMath::Clamp(Parsed, 0.5, 60.0);
	}
}

AUnrealSimHttpServer::AUnrealSimHttpServer()
{
	PrimaryActorTick.bCanEverTick = true;
}

void AUnrealSimHttpServer::BeginPlay()
{
	Super::BeginPlay();
	StartServer();
	SetPlanningCameraView();
	DronePawn = FindDronePawn();
}

void AUnrealSimHttpServer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!DronePawn)
	{
		DronePawn = FindDronePawn();
	}

	RefreshCameraStreamingState();

	if (!ListenerSocket)
	{
		return;
	}

	FSocket* ClientSocket = ListenerSocket->Accept(TEXT("SkySightClient"));
	if (!ClientSocket)
	{
		return;
	}

	ClientSocket->SetNonBlocking(false);
	const bool bCloseSocket = HandleClient(ClientSocket);
	if (bCloseSocket)
	{
		ClientSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
	}
}

void AUnrealSimHttpServer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopServer();
	Super::EndPlay(EndPlayReason);
}

void AUnrealSimHttpServer::StartServer()
{
	StopServer();

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		return;
	}

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	Addr->SetAnyAddress();
	Addr->SetPort(Port);

	ListenerSocket = FTcpSocketBuilder(TEXT("SkySightHttp"))
		.AsReusable()
		.BoundToAddress(FIPv4Address(127,0,0,1))
		.BoundToPort(Port)
		.Listening(8)
		.WithReceiveBufferSize(2 * 1024 * 1024);

	if (ListenerSocket)
	{
		ListenerSocket->SetNonBlocking(true);
		UE_LOG(LogTemp, Log, TEXT("HTTP server listening on port %d"), Port);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to start HTTP server on port %d"), Port);
	}
}

void AUnrealSimHttpServer::StopServer()
{
	if (ListenerSocket)
	{
		ListenerSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
		ListenerSocket = nullptr;
	}
}

bool AUnrealSimHttpServer::HandleClient(FSocket* ClientSocket)
{
	if (!ClientSocket)
	{
		return true;
	}

	FString Headers;
	TArray<uint8> BodyBytes;
	if (!ReadFullHttpRequest(ClientSocket, Headers, BodyBytes))
	{
		return true;
	}

	TArray<FString> Lines;
	Headers.ParseIntoArrayLines(Lines, false);
	if (Lines.Num() == 0)
	{
		SendJsonResponseUtf8(ClientSocket, 400, TEXT("{\"error\":\"empty request\"}"));
		return true;
	}

	FString FirstLine = Lines[0];
	TArray<FString> Tokens;
	FirstLine.ParseIntoArrayWS(Tokens);
	if (Tokens.Num() < 2)
	{
		SendJsonResponseUtf8(ClientSocket, 400, TEXT("{\"error\":\"malformed request line\"}"));
		return true;
	}

	FString Method = Tokens[0];
	const FString RequestTarget = Tokens[1];
	FString Path = RequestTarget;
	FString Query;
	if (Path.Split(TEXT("?"), &Path, &Query))
	{
		// Path and query normalized for routing and endpoint-specific params.
	}

	FString Body;
	if (BodyBytes.Num() > 0)
	{
		FUTF8ToTCHAR BodyConverter(reinterpret_cast<const ANSICHAR*>(BodyBytes.GetData()), BodyBytes.Num());
		Body = FString(BodyConverter.Length(), BodyConverter.Get());
	}

	if (!Path.Equals(TEXT("/sim/v1/camera.jpg"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Log, TEXT("HTTP request %s %s (body %d bytes)"), *Method, *Path, BodyBytes.Num());
	}
	bool bCloseSocket = true;
	ProcessRequest(Method, Path, Query, Body, ClientSocket, bCloseSocket);
	return bCloseSocket;
}

bool AUnrealSimHttpServer::ReadFullHttpRequest(FSocket* Socket, FString& OutHeaders, TArray<uint8>& OutBodyBytes)
{
	if (!Socket)
	{
		return false;
	}

	const int32 ChunkSize = 4096;
	TArray<uint8> Temp;
	Temp.SetNumUninitialized(ChunkSize);

	TArray<uint8> Data;
	Data.Reserve(ChunkSize * 2);

	int32 BytesRead = 0;
	int32 HeaderEnd = INDEX_NONE;

	while (HeaderEnd == INDEX_NONE)
	{
		if (!Socket->Recv(Temp.GetData(), Temp.Num(), BytesRead) || BytesRead <= 0)
		{
			return false;
		}

		Data.Append(Temp.GetData(), BytesRead);
		HeaderEnd = FindHeaderEnd(Data);
	}

	const int32 HeaderBytes = HeaderEnd + 4;
	FUTF8ToTCHAR HeaderConverter(reinterpret_cast<const ANSICHAR*>(Data.GetData()), HeaderBytes);
	OutHeaders = FString(HeaderConverter.Length(), HeaderConverter.Get());

	const int32 ContentLength = ParseContentLength(OutHeaders);

	OutBodyBytes.Reset();
	const int32 ExistingBodyBytes = Data.Num() - HeaderBytes;
	if (ExistingBodyBytes > 0)
	{
		OutBodyBytes.Append(Data.GetData() + HeaderBytes, ExistingBodyBytes);
	}

	while (OutBodyBytes.Num() < ContentLength)
	{
		if (!Socket->Recv(Temp.GetData(), Temp.Num(), BytesRead) || BytesRead <= 0)
		{
			return false;
		}

		OutBodyBytes.Append(Temp.GetData(), BytesRead);
	}

	return true;
}

void AUnrealSimHttpServer::SendString(FSocket* Socket, const FString& Data)
{
	if (!Socket)
	{
		return;
	}

	FTCHARToUTF8 Converter(*Data);
	int32 TotalSent = 0;
	while (TotalSent < Converter.Length())
	{
		int32 Sent = 0;
		if (!Socket->Send(reinterpret_cast<const uint8*>(Converter.Get()) + TotalSent, Converter.Length() - TotalSent, Sent) || Sent <= 0)
		{
			return;
		}
		TotalSent += Sent;
	}
}

void AUnrealSimHttpServer::SendJsonResponse(FSocket* Socket, int32 StatusCode, const FString& Body)
{
	SendJsonResponseUtf8(Socket, StatusCode, Body);
}

void AUnrealSimHttpServer::SendJsonResponseUtf8(FSocket* Socket, int32 StatusCode, const FString& JsonString)
{
	if (!Socket)
	{
		return;
	}

	const FString StatusText = StatusCode == 200 ? TEXT("OK") : TEXT("ERROR");
	FTCHARToUTF8 BodyConverter(*JsonString);
	const int32 BodyLength = BodyConverter.Length();
	const FString Header = FString::Printf(
		TEXT("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n"),
		StatusCode,
		*StatusText,
		BodyLength
	);
	SendString(Socket, Header);

	int32 Sent = 0;
	Socket->Send(reinterpret_cast<const uint8*>(BodyConverter.Get()), BodyLength, Sent);
}

void AUnrealSimHttpServer::SendBinaryResponse(FSocket* Socket, const TArray<uint8>& Data, const FString& ContentType)
{
	if (!Socket)
	{
		return;
	}

	const FString Header = FString::Printf(TEXT("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n"), *ContentType, Data.Num());
	SendString(Socket, Header);

	int32 TotalSent = 0;
	while (TotalSent < Data.Num())
	{
		int32 Sent = 0;
		if (!Socket->Send(Data.GetData() + TotalSent, Data.Num() - TotalSent, Sent) || Sent <= 0)
		{
			return;
		}
		TotalSent += Sent;
	}
}

void AUnrealSimHttpServer::ProcessRequest(const FString& Method, const FString& Path, const FString& Query, const FString& Body, FSocket* Socket, bool& bCloseSocket)
{
	if (Path == TEXT("/sim/v1/map_info") && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		AOrthoMapSnapshotter* Snapshotter = FindSnapshotter();
		const FMapSnapshotInfo Info = Snapshotter ? Snapshotter->GetSnapshotInfo() : FMapSnapshotInfo{};
		const FString Payload = BuildMapInfoJson(Info);

		UE_LOG(
			LogTemp,
			Log,
			TEXT("map_info served lat_min=%.6f lon_min=%.6f lat_max=%.6f lon_max=%.6f"),
			Info.LatMin,
			Info.LonMin,
			Info.LatMax,
			Info.LonMax
		);

		SendJsonResponseUtf8(Socket, 200, Payload);
		return;
	}

	if (Path == TEXT("/sim/v1/map.png") && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		const FString OutputPath = FPaths::ProjectSavedDir() / TEXT("skysight_map.png");
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *OutputPath) || Bytes.Num() == 0)
		{
			SendJsonResponseUtf8(Socket, 404, TEXT("{\"error\":\"map image not found\"}"));
			return;
		}

		SendBinaryResponse(Socket, Bytes, TEXT("image/png"));
		return;
	}

	if (Path == TEXT("/sim/v1/telemetry") && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		ASimWorldManager* WorldManager = FindWorldManager();
		const double OriginLat = WorldManager ? WorldManager->OriginLatDeg : 0.0;
		const double OriginLon = WorldManager ? WorldManager->OriginLonDeg : 0.0;
		const double OriginAlt = WorldManager ? WorldManager->OriginAltMeters : 0.0;
		const double SafeOriginLat = ClampLatitude(OriginLat, 0.0);
		const double SafeOriginLon = ClampLongitude(OriginLon, 0.0);
		const double SafeOriginAlt = FMath::Max(0.0, SanitizeFinite(OriginAlt, 0.0));

		if (!DronePawn)
		{
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("telemetry"));
			Root->SetStringField(TEXT("uav_id"), TEXT("sim"));
			Root->SetStringField(TEXT("timestamp"), FormatIso8601FromEpochSeconds(UUnrealBridgeProtocol::GetUnixEpochSeconds()));
			Root->SetNumberField(TEXT("timestamp_ms"), static_cast<double>(GetUnixTimestampMs()));
			Root->SetStringField(TEXT("status"), TEXT("WAITING_FOR_ROUTE"));
			Root->SetStringField(TEXT("flight_mode"), TEXT("WAITING_FOR_ROUTE"));
			Root->SetNumberField(TEXT("lat"), SafeOriginLat);
			Root->SetNumberField(TEXT("lon"), SafeOriginLon);
			Root->SetNumberField(TEXT("alt"), SafeOriginAlt);
			Root->SetNumberField(TEXT("alt_agl"), 0.0);
			Root->SetNumberField(TEXT("heading"), 0.0);
			Root->SetNumberField(TEXT("yaw"), 0.0);
			Root->SetNumberField(TEXT("ground_speed"), 0.0);
			Root->SetNumberField(TEXT("vx"), 0.0);
			Root->SetNumberField(TEXT("vy"), 0.0);
			Root->SetNumberField(TEXT("vz"), 0.0);
			// battery = fraction 0..1 for python
			Root->SetNumberField(TEXT("battery"), 1.0);
			// optional: percent 0..100
			Root->SetNumberField(TEXT("battery_percent"), 100.0);
			// optional debug: volts
			Root->SetNumberField(TEXT("battery_voltage_v"), 12.0);

			FString Payload;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
			FJsonSerializer::Serialize(Root, Writer);

			UE_LOG(LogTemp, Log, TEXT("Telemetry waiting_for_route -> 503"));
			SendJsonResponseUtf8(Socket, 503, Payload);
			return;
		}

		const FSkysightTelemetry& Telemetry = DronePawn->GetLatestTelemetry();
		const double TimestampSeconds = SanitizeFinite(Telemetry.TimestampSecondsEpoch, UUnrealBridgeProtocol::GetUnixEpochSeconds());
		const double SafeLat = ClampLatitude(Telemetry.LatitudeDeg, SafeOriginLat);
		const double SafeLon = ClampLongitude(Telemetry.LongitudeDeg, SafeOriginLon);
		double SafeAlt = SanitizeFinite(Telemetry.AltitudeMeters, 0.0);
		SafeAlt = FMath::Max(0.0, SafeAlt);
		double SafeAltAgl = SanitizeFinite(Telemetry.AltitudeAglMeters, 0.0);
		SafeAltAgl = FMath::Max(0.0, SafeAltAgl);
		double SafeHeading = SanitizeFinite(Telemetry.YawDeg, 0.0);
		double SafeGroundSpeed = SanitizeFinite(Telemetry.GroundSpeedMps, 0.0);
		SafeGroundSpeed = FMath::Max(0.0, SafeGroundSpeed);
		// telemetry.BatteryPercent is 0..100
		double BatteryFraction = SanitizeFinite(Telemetry.BatteryPercent / 100.0, 1.0);
		BatteryFraction = FMath::Clamp(BatteryFraction, 0.0, 1.0);
		const FVector VelocityCm = DronePawn->GetVelocity();
		const double VxMps = SanitizeFinite(VelocityCm.X / 100.0, 0.0);
		const double VyMps = SanitizeFinite(VelocityCm.Y / 100.0, 0.0);
		const double VzMps = SanitizeFinite(VelocityCm.Z / 100.0, 0.0);
		const FString SafeFlightMode = Telemetry.FlightMode.IsEmpty() ? TEXT("IN_FLIGHT") : Telemetry.FlightMode;
		const FRotator CameraMountRotation = DronePawn->GetCameraRelativeRotation();

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("telemetry"));
		Root->SetStringField(TEXT("uav_id"), Telemetry.UavId.IsEmpty() ? TEXT("sim") : Telemetry.UavId);
		Root->SetStringField(TEXT("timestamp"), FormatIso8601FromEpochSeconds(TimestampSeconds));
		Root->SetNumberField(TEXT("timestamp_ms"), TimestampSeconds * 1000.0);
		Root->SetStringField(TEXT("status"), SafeFlightMode);
		Root->SetNumberField(TEXT("lat"), SafeLat);
		Root->SetNumberField(TEXT("lon"), SafeLon);
		Root->SetNumberField(TEXT("alt"), SafeAlt);
		Root->SetNumberField(TEXT("alt_agl"), SafeAltAgl);
		Root->SetNumberField(TEXT("heading"), SafeHeading);
		Root->SetNumberField(TEXT("ground_speed"), SafeGroundSpeed);
		Root->SetNumberField(TEXT("yaw"), SafeHeading);
		Root->SetNumberField(TEXT("pitch"), SanitizeFinite(Telemetry.PitchDeg, 0.0));
		Root->SetNumberField(TEXT("roll"), SanitizeFinite(Telemetry.RollDeg, 0.0));
		Root->SetNumberField(TEXT("vx"), VxMps);
		Root->SetNumberField(TEXT("vy"), VyMps);
		Root->SetNumberField(TEXT("vz"), VzMps);
		Root->SetNumberField(TEXT("camera_mount_pitch_deg"), -SanitizeFinite(CameraMountRotation.Pitch, 0.0));
		Root->SetNumberField(TEXT("camera_mount_yaw_deg"), SanitizeFinite(CameraMountRotation.Yaw, 0.0));
		Root->SetNumberField(TEXT("camera_mount_roll_deg"), SanitizeFinite(CameraMountRotation.Roll, 0.0));
		// battery = fraction 0..1
		Root->SetNumberField(TEXT("battery"), BatteryFraction);
		// battery_percent = percent 0..100
		Root->SetNumberField(TEXT("battery_percent"), SanitizeFinite(Telemetry.BatteryPercent, 100.0));
		// optional debug volts
		Root->SetNumberField(TEXT("battery_voltage_v"), 12.0 * BatteryFraction);
		Root->SetStringField(TEXT("flight_mode"), SafeFlightMode);

		FString Payload;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(Root, Writer);

		UE_LOG(LogTemp, Log, TEXT("Telemetry served"));
		SendJsonResponseUtf8(Socket, 200, Payload);
		return;
	}

	if (Path == TEXT("/sim/v1/camera_info") && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		if (!DronePawn)
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"drone not available\"}"));
			return;
		}

		const double FovDeg = DronePawn->GetCameraFovDeg();
		const FRotator MountRotation = DronePawn->GetCameraRelativeRotation();
		const FRotator BaseMountRotation = DronePawn->GetCameraBaseRelativeRotation();
		const FVector MountLocation = DronePawn->GetCameraRelativeLocation();
		const FIntPoint Resolution = DronePawn->GetCameraResolution();
		const double AspectRatio = DronePawn->GetCameraAspectRatio();

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("fov_deg"), FovDeg);
		Root->SetNumberField(TEXT("mount_pitch_deg"), MountRotation.Pitch);
		Root->SetNumberField(TEXT("mount_yaw_deg"), MountRotation.Yaw);
		Root->SetNumberField(TEXT("mount_roll_deg"), MountRotation.Roll);
		Root->SetNumberField(TEXT("base_mount_pitch_deg"), BaseMountRotation.Pitch);
		Root->SetNumberField(TEXT("base_mount_yaw_deg"), BaseMountRotation.Yaw);
		Root->SetNumberField(TEXT("base_mount_roll_deg"), BaseMountRotation.Roll);
		Root->SetBoolField(TEXT("tracking_active"), DronePawn->IsCameraTrackingTarget());
		Root->SetNumberField(TEXT("mount_x_cm"), MountLocation.X);
		Root->SetNumberField(TEXT("mount_y_cm"), MountLocation.Y);
		Root->SetNumberField(TEXT("mount_z_cm"), MountLocation.Z);
		Root->SetNumberField(TEXT("width_px"), Resolution.X);
		Root->SetNumberField(TEXT("height_px"), Resolution.Y);
		Root->SetNumberField(TEXT("aspect_ratio"), AspectRatio);

		FString Payload;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(Root, Writer);

		UE_LOG(
			LogTemp,
			Log,
			TEXT("camera_info served: fov_deg=%.2f mount_pitch_deg=%.2f mount_yaw_deg=%.2f mount_roll_deg=%.2f tracking=%d res=%dx%d"),
			FovDeg,
			MountRotation.Pitch,
			MountRotation.Yaw,
			MountRotation.Roll,
			DronePawn->IsCameraTrackingTarget() ? 1 : 0,
			Resolution.X,
			Resolution.Y
		);

		SendJsonResponseUtf8(Socket, 200, Payload);
		return;
	}

	if (Path == TEXT("/sim/v1/route") && Method.Equals(TEXT("POST"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Log, TEXT("Route received"));

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"invalid route\"}"));
			return;
		}

		const TSharedPtr<FJsonObject>* NestedRouteObject = nullptr;
		TSharedPtr<FJsonObject> RouteObject = Root;
		if (Root->TryGetObjectField(TEXT("route"), NestedRouteObject) && NestedRouteObject && NestedRouteObject->IsValid())
		{
			RouteObject = *NestedRouteObject;
		}

		FSkysightRoute Route;
		FString UavId;
		if (RouteObject->TryGetStringField(TEXT("uav_id"), UavId))
		{
			Route.UavId = UavId;
		}

		double Version = 1.0;
		if (RouteObject->TryGetNumberField(TEXT("version"), Version))
		{
			Route.Version = static_cast<int32>(Version);
		}

		double ActiveIndex = 0.0;
		if (RouteObject->TryGetNumberField(TEXT("active_index"), ActiveIndex))
		{
			Route.ActiveIndex = static_cast<int32>(ActiveIndex);
		}

		const TArray<TSharedPtr<FJsonValue>>* WaypointsJson = nullptr;
		if (!RouteObject->TryGetArrayField(TEXT("waypoints"), WaypointsJson) || !WaypointsJson)
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"route.waypoints array is required\"}"));
			return;
		}

		for (int32 WaypointIndex = 0; WaypointIndex < WaypointsJson->Num(); ++WaypointIndex)
		{
			const TSharedPtr<FJsonValue>& WaypointValue = (*WaypointsJson)[WaypointIndex];
			const TSharedPtr<FJsonObject> WaypointObject = WaypointValue ? WaypointValue->AsObject() : nullptr;
			if (!WaypointObject.IsValid())
			{
				SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"each waypoint must be an object with lat/lon/alt\"}"));
				return;
			}

			FSkysightWaypoint Waypoint;
			double Lat = 0.0;
			double Lon = 0.0;
			double Alt = 0.0;
			if (!WaypointObject->TryGetNumberField(TEXT("lat"), Lat)
				|| !WaypointObject->TryGetNumberField(TEXT("lon"), Lon)
				|| !WaypointObject->TryGetNumberField(TEXT("alt"), Alt)
				|| !FMath::IsFinite(Lat)
				|| !FMath::IsFinite(Lon)
				|| !FMath::IsFinite(Alt))
			{
				SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"waypoint fields lat/lon/alt must be finite numbers\"}"));
				return;
			}

			Waypoint.LatitudeDeg = Lat;
			Waypoint.LongitudeDeg = Lon;
			Waypoint.AltitudeMeters = Alt;
			Route.Waypoints.Add(Waypoint);
		}

		if (Route.Waypoints.Num() == 0)
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"route must contain at least one waypoint\"}"));
			return;
		}

		ASimWorldManager* WorldManager = FindWorldManager();
		if (!WorldManager)
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"world manager not available\"}"));
			return;
		}

		auto TryParseOrbitTargetObject = [WorldManager](const TSharedPtr<FJsonObject>& OrbitTargetObject, FVector& OutWorldPoint) -> bool
		{
			if (!OrbitTargetObject.IsValid())
			{
				return false;
			}

			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (OrbitTargetObject->TryGetNumberField(TEXT("world_x_cm"), X)
				&& OrbitTargetObject->TryGetNumberField(TEXT("world_y_cm"), Y)
				&& OrbitTargetObject->TryGetNumberField(TEXT("world_z_cm"), Z)
				&& FMath::IsFinite(X)
				&& FMath::IsFinite(Y)
				&& FMath::IsFinite(Z))
			{
				OutWorldPoint = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
				return true;
			}

			double Lat = 0.0;
			double Lon = 0.0;
			double Alt = WorldManager->GetGeoReference().OriginAltMeters;
			if (OrbitTargetObject->TryGetNumberField(TEXT("lat"), Lat)
				&& OrbitTargetObject->TryGetNumberField(TEXT("lon"), Lon)
				&& FMath::IsFinite(Lat)
				&& FMath::IsFinite(Lon))
			{
				double ParsedAlt = Alt;
				if (OrbitTargetObject->TryGetNumberField(TEXT("alt"), ParsedAlt) && FMath::IsFinite(ParsedAlt))
				{
					Alt = ParsedAlt;
				}
				OutWorldPoint = UUnrealBridgeProtocol::GeoToUnrealCm(WorldManager->GetGeoReference(), Lat, Lon, Alt);
				return true;
			}

			return false;
		};

		bool bHasOrbitTarget = false;
		FVector OrbitTargetWorldPoint = FVector::ZeroVector;
		const TSharedPtr<FJsonObject>* OrbitTargetObjectPtr = nullptr;
		if (RouteObject->TryGetObjectField(TEXT("orbit_target"), OrbitTargetObjectPtr)
			&& OrbitTargetObjectPtr
			&& OrbitTargetObjectPtr->IsValid())
		{
			bHasOrbitTarget = TryParseOrbitTargetObject(*OrbitTargetObjectPtr, OrbitTargetWorldPoint);
		}
		if (!bHasOrbitTarget
			&& Root->TryGetObjectField(TEXT("orbit_target"), OrbitTargetObjectPtr)
			&& OrbitTargetObjectPtr
			&& OrbitTargetObjectPtr->IsValid())
		{
			bHasOrbitTarget = TryParseOrbitTargetObject(*OrbitTargetObjectPtr, OrbitTargetWorldPoint);
		}

		if (!WorldManager->ApplyRouteToDrone(Route))
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"drone not available\"}"));
			return;
		}

		DronePawn = WorldManager->GetDronePawn();
		if (!DronePawn)
		{
			DronePawn = FindDronePawn();
		}

		if (DronePawn && GetWorld())
		{
			if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
			{
				if (PC->GetPawn() != DronePawn)
				{
					UE_LOG(LogTemp, Log, TEXT("[RouteApply] Possessing active drone pawn after route apply"));
					PC->Possess(DronePawn);
				}
			}

			if (bHasOrbitTarget)
			{
				DronePawn->SetCameraTrackPoint(OrbitTargetWorldPoint);
				UE_LOG(LogTemp, Log, TEXT("[RouteApply] orbit_target applied: %s"), *OrbitTargetWorldPoint.ToString());
			}
			else
			{
				DronePawn->ClearCameraTrack();
			}
		}

		UE_LOG(LogTemp, Log, TEXT("[RouteApply] Route accepted and started (%d waypoints)"), Route.Waypoints.Num());
		SendJsonResponseUtf8(Socket, 200, TEXT("{\"ok\":true}"));
		return;
	}

	if (Path == TEXT("/sim/v1/command") && Method.Equals(TEXT("POST"), ESearchCase::IgnoreCase))
	{
		// Example payloads/curl:
		// curl -X POST http://127.0.0.1:9000/sim/v1/command -H "Content-Type: application/json" -d "{\"type\":\"SET_SPEED\",\"speed_mps\":1.0}"
		// curl -X POST http://127.0.0.1:9000/sim/v1/command -H "Content-Type: application/json" -d "{\"type\":\"SET_VELOCITY\",\"vx\":0.0,\"vy\":0.0,\"vz\":0.0}"
		// curl -X POST http://127.0.0.1:9000/sim/v1/command -H "Content-Type: application/json" -d "{\"type\":\"CLEAR_VELOCITY_OVERRIDE\"}"
		// { "type": "SET_SPEED", "speed_mps": 1.0 }
		// { "type": "SET_VELOCITY", "vx": 0.0, "vy": 0.0, "vz": 0.0 }
		// { "type": "CLEAR_VELOCITY_OVERRIDE" }
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"invalid command\"}"));
			return;
		}

		FString CommandString;
		if (!Root->TryGetStringField(TEXT("type"), CommandString)
			&& !Root->TryGetStringField(TEXT("command"), CommandString))
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"missing type\"}"));
			return;
		}

		FSkysightCommand Command;
		FString UavId;
		if (Root->TryGetStringField(TEXT("uav_id"), UavId))
		{
			Command.UavId = UavId;
		}

		if (!TryParseCommandType(CommandString, Command.Type))
		{
			SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"unsupported command\"}"));
			return;
		}

		if (!DronePawn && Command.Type != ESkysightCommandType::DESPAWN)
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"drone not available\"}"));
			return;
		}

		const TSharedPtr<FJsonObject>* PayloadObjectPtr = nullptr;
		const bool bHasPayloadObject = Root->TryGetObjectField(TEXT("payload"), PayloadObjectPtr)
			&& PayloadObjectPtr
			&& PayloadObjectPtr->IsValid();
		const TSharedPtr<FJsonObject> PayloadObject = bHasPayloadObject ? *PayloadObjectPtr : nullptr;

		auto TryGetNumberFromRootOrPayload = [&Root, &PayloadObject](const TCHAR* FieldName, double& OutValue, bool& bOutFromPayload) -> bool
		{
			bOutFromPayload = false;
			if (Root->TryGetNumberField(FieldName, OutValue))
			{
				return true;
			}
			if (PayloadObject.IsValid() && PayloadObject->TryGetNumberField(FieldName, OutValue))
			{
				bOutFromPayload = true;
				return true;
			}
			return false;
		};

		auto TryGetStringFromRootOrPayload = [&Root, &PayloadObject](const TCHAR* FieldName, FString& OutValue, bool& bOutFromPayload) -> bool
		{
			bOutFromPayload = false;
			if (Root->TryGetStringField(FieldName, OutValue))
			{
				return true;
			}
			if (PayloadObject.IsValid() && PayloadObject->TryGetStringField(FieldName, OutValue))
			{
				bOutFromPayload = true;
				return true;
			}
			return false;
		};

		auto ResolveOrbitTargetWorldPoint = [this, &Root, &PayloadObject, &TryGetNumberFromRootOrPayload](FVector& OutWorldPoint, FString& OutSource, FString& OutError) -> bool
		{
			auto TryReadOrbitTargetObject = [&OutWorldPoint, &OutSource](const TSharedPtr<FJsonObject>& OrbitTargetObject, const TCHAR* SourceName, ASimWorldManager* WorldManager) -> bool
			{
				if (!OrbitTargetObject.IsValid())
				{
					return false;
				}

				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				if (OrbitTargetObject->TryGetNumberField(TEXT("world_x_cm"), X)
					&& OrbitTargetObject->TryGetNumberField(TEXT("world_y_cm"), Y)
					&& OrbitTargetObject->TryGetNumberField(TEXT("world_z_cm"), Z)
					&& FMath::IsFinite(X)
					&& FMath::IsFinite(Y)
					&& FMath::IsFinite(Z))
				{
					OutWorldPoint = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
					OutSource = SourceName;
					return true;
				}

				double Lat = 0.0;
				double Lon = 0.0;
				double Alt = WorldManager ? WorldManager->GetGeoReference().OriginAltMeters : 0.0;
				if (OrbitTargetObject->TryGetNumberField(TEXT("lat"), Lat)
					&& OrbitTargetObject->TryGetNumberField(TEXT("lon"), Lon)
					&& FMath::IsFinite(Lat)
					&& FMath::IsFinite(Lon))
				{
					double ParsedAlt = Alt;
					if (OrbitTargetObject->TryGetNumberField(TEXT("alt"), ParsedAlt) && FMath::IsFinite(ParsedAlt))
					{
						Alt = ParsedAlt;
					}
					if (!WorldManager)
					{
						return false;
					}
					OutWorldPoint = UUnrealBridgeProtocol::GeoToUnrealCm(WorldManager->GetGeoReference(), Lat, Lon, Alt);
					OutSource = SourceName;
					return true;
				}

				return false;
			};

			ASimWorldManager* WorldManager = FindWorldManager();
			const TSharedPtr<FJsonObject>* OrbitTargetObjectPtr = nullptr;
			if (Root->TryGetObjectField(TEXT("orbit_target"), OrbitTargetObjectPtr)
				&& OrbitTargetObjectPtr
				&& TryReadOrbitTargetObject(*OrbitTargetObjectPtr, TEXT("orbit_target.root"), WorldManager))
			{
				return true;
			}
			if (PayloadObject.IsValid()
				&& PayloadObject->TryGetObjectField(TEXT("orbit_target"), OrbitTargetObjectPtr)
				&& OrbitTargetObjectPtr
				&& TryReadOrbitTargetObject(*OrbitTargetObjectPtr, TEXT("orbit_target.payload"), WorldManager))
			{
				return true;
			}

			auto TryReadWorldPoint = [&TryGetNumberFromRootOrPayload, &OutWorldPoint, &OutSource](const TCHAR* XField, const TCHAR* YField, const TCHAR* ZField, const TCHAR* SourceName) -> bool
			{
				double X = 0.0;
				double Y = 0.0;
				double Z = 0.0;
				bool bFromPayloadX = false;
				bool bFromPayloadY = false;
				bool bFromPayloadZ = false;
				const bool bHasX = TryGetNumberFromRootOrPayload(XField, X, bFromPayloadX);
				const bool bHasY = TryGetNumberFromRootOrPayload(YField, Y, bFromPayloadY);
				const bool bHasZ = TryGetNumberFromRootOrPayload(ZField, Z, bFromPayloadZ);
				if (!bHasX || !bHasY || !bHasZ)
				{
					return false;
				}
				if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
				{
					return false;
				}
				OutWorldPoint = FVector(static_cast<float>(X), static_cast<float>(Y), static_cast<float>(Z));
				OutSource = FString::Printf(TEXT("%s.%s"), SourceName, (bFromPayloadX || bFromPayloadY || bFromPayloadZ) ? TEXT("payload") : TEXT("root"));
				return true;
			};

			if (TryReadWorldPoint(TEXT("target_world_x_cm"), TEXT("target_world_y_cm"), TEXT("target_world_z_cm"), TEXT("target_world")))
			{
				return true;
			}
			if (TryReadWorldPoint(TEXT("world_x_cm"), TEXT("world_y_cm"), TEXT("world_z_cm"), TEXT("world")))
			{
				return true;
			}
			if (TryReadWorldPoint(TEXT("target_x"), TEXT("target_y"), TEXT("target_z"), TEXT("target_xyz")))
			{
				return true;
			}

			double Lat = 0.0;
			double Lon = 0.0;
			double Alt = 0.0;
			bool bLatFromPayload = false;
			bool bLonFromPayload = false;
			bool bAltFromPayload = false;

			bool bHasLat = TryGetNumberFromRootOrPayload(TEXT("target_lat"), Lat, bLatFromPayload);
			bool bHasLon = TryGetNumberFromRootOrPayload(TEXT("target_lon"), Lon, bLonFromPayload);
			bool bHasAlt = TryGetNumberFromRootOrPayload(TEXT("target_alt"), Alt, bAltFromPayload);
			if (!bHasLat || !bHasLon)
			{
				bHasLat = TryGetNumberFromRootOrPayload(TEXT("lat"), Lat, bLatFromPayload);
				bHasLon = TryGetNumberFromRootOrPayload(TEXT("lon"), Lon, bLonFromPayload);
				bHasAlt = TryGetNumberFromRootOrPayload(TEXT("alt"), Alt, bAltFromPayload);
			}

			if (!bHasLat || !bHasLon)
			{
				OutError = TEXT("missing orbit target point (expected world_x/y/z or lat/lon fields)");
				return false;
			}

			if (!FMath::IsFinite(Lat) || !FMath::IsFinite(Lon))
			{
				OutError = TEXT("orbit target lat/lon must be finite numbers");
				return false;
			}

			if (!WorldManager)
			{
				OutError = TEXT("world manager not available for orbit target geo conversion");
				return false;
			}

			if (!bHasAlt || !FMath::IsFinite(Alt))
			{
				Alt = WorldManager->GetGeoReference().OriginAltMeters;
			}

			OutWorldPoint = UUnrealBridgeProtocol::GeoToUnrealCm(WorldManager->GetGeoReference(), Lat, Lon, Alt);
			OutSource = FString::Printf(TEXT("geo.%s"), (bLatFromPayload || bLonFromPayload || bAltFromPayload) ? TEXT("payload") : TEXT("root"));
			return true;
		};

		if (Command.Type == ESkysightCommandType::DESPAWN)
		{
			ASimWorldManager* WorldManager = FindWorldManager();
			if (WorldManager)
			{
				WorldManager->DespawnDrone();
			}
			DronePawn = FindDronePawn();
			SetPlanningCameraView();
			UE_LOG(LogTemp, Log, TEXT("Command applied: DESPAWN"));
			SendJsonResponseUtf8(Socket, 200, TEXT("{\"ok\":true}"));
			return;
		}
		else if (Command.Type == ESkysightCommandType::SET_SPEED)
		{
			double SpeedMps = 0.0;
			bool bReadFromPayload = false;
			bool bHasSpeed = Root->TryGetNumberField(TEXT("speed_mps"), SpeedMps);
			if (!bHasSpeed && PayloadObject.IsValid())
			{
				bHasSpeed = PayloadObject->TryGetNumberField(TEXT("speed_mps"), SpeedMps);
				bReadFromPayload = bHasSpeed;
			}

			if (!bHasSpeed || !FMath::IsFinite(SpeedMps))
			{
				SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"SET_SPEED requires numeric speed_mps\"}"));
				return;
			}

			if (SpeedMps < 0.0)
			{
				SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"speed_mps must be >= 0\"}"));
				return;
			}

			Command.SpeedMps = static_cast<float>(SpeedMps);
			UE_LOG(
				LogTemp,
				Log,
				TEXT("Command applied: SET_SPEED speed_mps=%.3f source=%s"),
				Command.SpeedMps,
				bReadFromPayload ? TEXT("payload") : TEXT("root")
			);
		}
		else if (Command.Type == ESkysightCommandType::SET_VELOCITY)
		{
			double Vx = 0.0;
			double Vy = 0.0;
			double Vz = 0.0;
			bool bReadFromPayload = false;
			bool bHasVx = Root->TryGetNumberField(TEXT("vx"), Vx);
			bool bHasVy = Root->TryGetNumberField(TEXT("vy"), Vy);
			bool bHasVz = Root->TryGetNumberField(TEXT("vz"), Vz);
			if ((!bHasVx || !bHasVy || !bHasVz) && PayloadObject.IsValid())
			{
				bHasVx = PayloadObject->TryGetNumberField(TEXT("vx"), Vx);
				bHasVy = PayloadObject->TryGetNumberField(TEXT("vy"), Vy);
				bHasVz = PayloadObject->TryGetNumberField(TEXT("vz"), Vz);
				bReadFromPayload = bHasVx && bHasVy && bHasVz;
			}

			const bool bFinite = FMath::IsFinite(Vx) && FMath::IsFinite(Vy) && FMath::IsFinite(Vz);
			if (!bHasVx || !bHasVy || !bHasVz || !bFinite)
			{
				SendJsonResponseUtf8(Socket, 400, TEXT("{\"error\":\"SET_VELOCITY requires numeric vx, vy, vz\"}"));
				return;
			}

			Command.VxMps = static_cast<float>(Vx);
			Command.VyMps = static_cast<float>(Vy);
			Command.VzMps = static_cast<float>(Vz);
			UE_LOG(
				LogTemp,
				Log,
				TEXT("Command applied: SET_VELOCITY vx=%.3f vy=%.3f vz=%.3f source=%s"),
				Command.VxMps,
				Command.VyMps,
				Command.VzMps,
				bReadFromPayload ? TEXT("payload") : TEXT("root")
			);
		}
		else if (Command.Type == ESkysightCommandType::SET_MODE)
		{
			bool bModeFromPayload = false;
			if (!TryGetStringFromRootOrPayload(TEXT("mode"), Command.ModeName, bModeFromPayload))
			{
				TryGetStringFromRootOrPayload(TEXT("mode_name"), Command.ModeName, bModeFromPayload);
			}
			if (Command.ModeName.IsEmpty())
			{
				Command.ModeName = CommandString;
			}

			if (!Command.ModeName.Contains(TEXT("ORBIT"), ESearchCase::IgnoreCase))
			{
				Command.bClearCameraTrack = true;
			}
			else
			{
				FString TargetSource;
				FString TargetError;
				if (!ResolveOrbitTargetWorldPoint(Command.TargetWorldPointCm, TargetSource, TargetError))
				{
					SendJsonResponseUtf8(Socket, 400, FString::Printf(TEXT("{\"error\":\"%s\"}"), *TargetError));
					return;
				}
				Command.bHasTargetWorldPoint = true;
				UE_LOG(LogTemp, Log, TEXT("Command applied: SET_MODE mode=%s orbit_target=%s source=%s"),
					*Command.ModeName, *Command.TargetWorldPointCm.ToString(), *TargetSource);
			}
		}
		else if (Command.Type == ESkysightCommandType::ORBIT_START)
		{
			FString TargetSource;
			FString TargetError;
			if (!ResolveOrbitTargetWorldPoint(Command.TargetWorldPointCm, TargetSource, TargetError))
			{
				SendJsonResponseUtf8(Socket, 400, FString::Printf(TEXT("{\"error\":\"%s\"}"), *TargetError));
				return;
			}

			Command.bHasTargetWorldPoint = true;
			UE_LOG(LogTemp, Log, TEXT("Command applied: ORBIT_START target=%s source=%s"),
				*Command.TargetWorldPointCm.ToString(), *TargetSource);
		}
		else if (Command.Type == ESkysightCommandType::ORBIT_STOP)
		{
			Command.bClearCameraTrack = true;
			UE_LOG(LogTemp, Log, TEXT("Command applied: ORBIT_STOP"));
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("Command applied: %s"), *CommandString.ToUpper());
		}

		DronePawn->ApplyCommand(Command);
		SendJsonResponseUtf8(Socket, 200, TEXT("{\"ok\":true}"));
		return;
	}

	if (Path == TEXT("/sim/v1/detections") && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("detections"), TArray<TSharedPtr<FJsonValue>>{});

		FString Payload;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
		FJsonSerializer::Serialize(Root, Writer);

		SendJsonResponseUtf8(Socket, 200, Payload);
		return;
	}

	if (Path.Equals(TEXT("/sim/v1/video.ts"), ESearchCase::IgnoreCase) && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		if (!DronePawn)
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"drone not available\"}"));
			return;
		}

		UCameraVideoStreamerComponent* Streamer = DronePawn->GetVideoStreamer();
		if (!Streamer || !Streamer->IsReady())
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"video streamer not ready\"}"));
			return;
		}

		{
			const int32 NewCount = ActiveVideoClients.Load() + 1;
			ActiveVideoClients.Store(NewCount);
			UE_LOG(LogTemp, Log, TEXT("[CameraStream] /video.ts client connected. ActiveVideoClients=%d"), NewCount);
		}
		RefreshCameraStreamingState();

		bCloseSocket = false;
		TWeakObjectPtr<UCameraVideoStreamerComponent> StreamerWeak(Streamer);
		TWeakObjectPtr<AUnrealSimHttpServer> WeakServer(this);
		Async(EAsyncExecution::Thread, [WeakServer, Socket, StreamerWeak]()
		{
			if (!WeakServer.IsValid())
			{
				if (Socket)
				{
					Socket->Close();
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
				}
				return;
			}
			WeakServer->StreamVideoTs(Socket, StreamerWeak);

			AsyncTask(ENamedThreads::GameThread, [WeakServer]()
			{
				if (!WeakServer.IsValid())
				{
					return;
				}

				const int32 Current = WeakServer->ActiveVideoClients.Load();
				const int32 NewCount = FMath::Max(0, Current - 1);
				WeakServer->ActiveVideoClients.Store(NewCount);
				UE_LOG(LogTemp, Log, TEXT("[CameraStream] /video.ts client disconnected. ActiveVideoClients=%d"), NewCount);
				WeakServer->RefreshCameraStreamingState();
			});
		});
		return;
	}

	if (Path.Equals(TEXT("/sim/v1/camera.jpg"), ESearchCase::IgnoreCase) && Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		if (!DronePawn)
		{
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"drone not available\"}"));
			return;
		}

		const double NowSec = FPlatformTime::Seconds();
		LastJpegRequestSec = NowSec;

		TArray<uint8> Frame;
		if (UCameraVideoStreamerComponent* Streamer = DronePawn->GetVideoStreamer())
		{
			const double DesiredClientHz = ParseRequestedHz(Query, 8.0);
			Streamer->SetLastJpegRequestTime(NowSec);
			RefreshCameraStreamingState();
			const bool bCapturedNewJpeg = Streamer->EnsureJpegFresh(NowSec, 1.0 / DesiredClientHz);
			const bool bHasFrame = Streamer->GetLatestJpeg(Frame) && Frame.Num() > 0;
			UE_LOG(LogTemp, Log, TEXT("[CameraStream] /camera.jpg requested_hz=%.2f captured_new=%d bytes=%d"), DesiredClientHz, bCapturedNewJpeg ? 1 : 0, Frame.Num());
			if (!bHasFrame)
			{
				SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"camera frame not ready\"}"));
				return;
			}
		}
		else if (!DronePawn->GetCameraFrameBytes(Frame) || Frame.Num() == 0)
		{
			RefreshCameraStreamingState();
			SendJsonResponseUtf8(Socket, 503, TEXT("{\"error\":\"camera frame not ready\"}"));
			return;
		}
		else
		{
			RefreshCameraStreamingState();
		}

		bCloseSocket = false;
		TArray<uint8> FrameCopy = MoveTemp(Frame);
		Async(EAsyncExecution::ThreadPool, [Socket, Frame = MoveTemp(FrameCopy)]() mutable
		{
			if (Socket)
			{
				AUnrealSimHttpServer::SendBinaryResponse(Socket, Frame, TEXT("image/jpeg"));
				Socket->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			}
		});
		return;
	}

	SendJsonResponseUtf8(Socket, 404, TEXT("{\"error\":\"endpoint not found\"}"));
}

ADronePawn* AUnrealSimHttpServer::FindDronePawn()
{
	if (!GetWorld())
	{
		return nullptr;
	}

	for (TActorIterator<ADronePawn> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

ASimWorldManager* AUnrealSimHttpServer::FindWorldManager()
{
	if (!GetWorld())
	{
		return nullptr;
	}

	for (TActorIterator<ASimWorldManager> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

AOrthoMapSnapshotter* AUnrealSimHttpServer::FindSnapshotter()
{
	if (!GetWorld())
	{
		return nullptr;
	}

	for (TActorIterator<AOrthoMapSnapshotter> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

void AUnrealSimHttpServer::EnsureDroneSpawnedAndPossessed()
{
	if (DronePawn)
	{
		RefreshCameraStreamingState();
		return;
	}

	ASimWorldManager* WorldManager = FindWorldManager();
	if (WorldManager)
	{
		UE_LOG(LogTemp, Log, TEXT("Spawning drone due to route request"));
		WorldManager->SpawnDrone();
	}

	DronePawn = FindDronePawn();
	if (!DronePawn)
	{
		return;
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		UE_LOG(LogTemp, Log, TEXT("Possessing drone pawn"));
		PC->Possess(DronePawn);
	}

	RefreshCameraStreamingState();
}

void AUnrealSimHttpServer::EnsureDroneSpawnedAndPossessedAtRouteStart(const FSkysightRoute& Route)
{
	if (!DronePawn)
	{
		ASimWorldManager* WorldManager = FindWorldManager();
		if (WorldManager)
		{
			bool bSpawnedFromRouteStart = false;
			if (Route.Waypoints.Num() > 0)
			{
				const FSkysightWaypoint& StartWp = Route.Waypoints[0];
				const double SpawnAltMeters = StartWp.AltitudeMeters;
				UE_LOG(LogTemp, Log, TEXT("[RouteSpawn] Spawning drone at waypoint[0]: lat=%.8f lon=%.8f alt=%.2f"),
					StartWp.LatitudeDeg, StartWp.LongitudeDeg, StartWp.AltitudeMeters);
				bSpawnedFromRouteStart = WorldManager->SpawnDroneAtGeo(StartWp.LatitudeDeg, StartWp.LongitudeDeg, SpawnAltMeters);
			}

			if (!bSpawnedFromRouteStart)
			{
				UE_LOG(LogTemp, Log, TEXT("[RouteSpawn] Fallback spawn at world manager location"));
				WorldManager->SpawnDrone();
			}
		}
	}

	DronePawn = FindDronePawn();
	if (!DronePawn || !GetWorld())
	{
		return;
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		if (PC->GetPawn() != DronePawn)
		{
			UE_LOG(LogTemp, Log, TEXT("[RouteSpawn] Possessing drone pawn"));
			PC->Possess(DronePawn);
		}
	}

	RefreshCameraStreamingState();
}

void AUnrealSimHttpServer::RefreshCameraStreamingState()
{
	if (!DronePawn)
	{
		return;
	}

	UCameraVideoStreamerComponent* Streamer = DronePawn->GetVideoStreamer();
	if (!Streamer)
	{
		return;
	}

	const bool bWantVideo = ActiveVideoClients.Load() > 0;
	const double NowSec = FPlatformTime::Seconds();
	const double RecentWindowSec = FMath::Max(0.0, static_cast<double>(JpegActiveRecentWindowSec));
	const bool bWantJpeg = (LastJpegRequestSec > 0.0) && ((NowSec - LastJpegRequestSec) <= RecentWindowSec);

	Streamer->UpdateStreamingState(bWantVideo, bWantJpeg);
}

void AUnrealSimHttpServer::SetPlanningCameraView()
{
	if (!GetWorld())
	{
		return;
	}

	ACameraActor* PlanningCamera = nullptr;
	for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TEXT("PlanningCamera")))
		{
			PlanningCamera = *It;
			break;
		}
	}

	if (!PlanningCamera)
	{
		UE_LOG(LogTemp, Log, TEXT("Planning camera not found; using default view"));
		return;
	}

	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->SetViewTargetWithBlend(PlanningCamera, 0.0f);
		UE_LOG(LogTemp, Log, TEXT("Planning camera set"));
	}
}

void AUnrealSimHttpServer::StreamVideoTs(FSocket* Socket, TWeakObjectPtr<UCameraVideoStreamerComponent> Streamer)
{
	if (!Socket)
	{
		return;
	}

	const FString Header = TEXT("HTTP/1.1 200 OK\r\n")
		TEXT("Content-Type: video/mp2t\r\n")
		TEXT("Transfer-Encoding: chunked\r\n")
		TEXT("Cache-Control: no-cache\r\n")
		TEXT("Connection: close\r\n\r\n");
	SendString(Socket, Header);

	if (Streamer.IsValid())
	{
		const TArray<uint8> HeaderData = Streamer->GetStreamHeader();
		if (HeaderData.Num() > 0 && !SendChunk(Socket, HeaderData))
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			return;
		}
	}

	TArray<uint8> Packet;
	while (Streamer.IsValid())
	{
		if (Streamer->DequeueVideoPacket(Packet))
		{
			if (!SendChunk(Socket, Packet))
			{
				break;
			}
		}
		else
		{
			FPlatformProcess::Sleep(0.005f);
		}
	}

	SendString(Socket, TEXT("0\r\n\r\n"));
	Socket->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
}

bool AUnrealSimHttpServer::SendChunk(FSocket* Socket, const TArray<uint8>& Data)
{
	if (!Socket)
	{
		return false;
	}

	const FString Prefix = FString::Printf(TEXT("%X\r\n"), Data.Num());
	SendString(Socket, Prefix);

	int32 TotalSent = 0;
	while (TotalSent < Data.Num())
	{
		int32 Sent = 0;
		if (!Socket->Send(Data.GetData() + TotalSent, Data.Num() - TotalSent, Sent) || Sent <= 0)
		{
			return false;
		}
		TotalSent += Sent;
	}

	SendString(Socket, TEXT("\r\n"));
	return true;
}
