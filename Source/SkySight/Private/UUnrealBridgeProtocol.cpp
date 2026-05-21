#include "UUnrealBridgeProtocol.h"
#include "Misc/DateTime.h"
#include "Math/UnrealMathUtility.h"

namespace
{
	constexpr double MetersPerDegLatitude = 111320.0;

	double GetMetersPerDegLongitude(double OriginLatDeg)
	{
		return 111320.0 * FMath::Cos(FMath::DegreesToRadians(OriginLatDeg));
	}

	double SafeScale(float Scale)
	{
		return Scale > 0.0f ? Scale : 100.0;
	}
}

FVector UUnrealBridgeProtocol::GeoToUnrealCm(const FSkysightGeoReference& Reference, double LatitudeDeg, double LongitudeDeg, double AltitudeMeters)
{
	const double DeltaLat = LatitudeDeg - Reference.OriginLatDeg;
	const double DeltaLon = LongitudeDeg - Reference.OriginLonDeg;
	const double LatMeters = DeltaLat * MetersPerDegLatitude;
	const double LonMeters = DeltaLon * GetMetersPerDegLongitude(Reference.OriginLatDeg);
	const double AltMeters = AltitudeMeters - Reference.OriginAltMeters;
	const double Scale = SafeScale(Reference.MetersToUnrealCm);
	return Reference.OriginWorldCm + FVector(LonMeters * Scale, LatMeters * Scale, AltMeters * Scale);
}

void UUnrealBridgeProtocol::UnrealCmToGeo(const FSkysightGeoReference& Reference, const FVector& PositionCm, double& OutLatitudeDeg, double& OutLongitudeDeg, double& OutAltitudeMeters)
{
	const double Scale = SafeScale(Reference.MetersToUnrealCm);
	const FVector LocalPositionCm = PositionCm - Reference.OriginWorldCm;
	const double LonMeters = LocalPositionCm.X / Scale;
	const double LatMeters = LocalPositionCm.Y / Scale;
	const double AltMeters = LocalPositionCm.Z / Scale;
	OutLatitudeDeg = Reference.OriginLatDeg + LatMeters / MetersPerDegLatitude;
	const double LonScale = GetMetersPerDegLongitude(Reference.OriginLatDeg);
	OutLongitudeDeg = Reference.OriginLonDeg + LonMeters / LonScale;
	OutAltitudeMeters = Reference.OriginAltMeters + AltMeters;
}

double UUnrealBridgeProtocol::GetUnixEpochSeconds()
{
	const FDateTime Now = FDateTime::UtcNow();
	return static_cast<double>(Now.ToUnixTimestamp());
}
