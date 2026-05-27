"""Engine-agnostic protocol models for UAV telemetry, routes, and objects."""

from __future__ import annotations

from datetime import datetime
from typing import List, Literal, Union

from pydantic import BaseModel

from fire_uav.module_core.schema import GeoDetection, Route, TelemetrySample
from fire_uav.utils.time import utc_now


class TelemetryMessage(BaseModel):
    """Normalized telemetry update for external visualizers."""

    type: Literal["telemetry"] = "telemetry"
    uav_id: str
    timestamp: datetime
    lat: float
    lon: float
    alt: float
    alt_agl: float | None = None
    yaw: float
    battery: float
    pitch: float | None = None
    roll: float | None = None
    status: str | None = None
    flight_mode: str | None = None
    camera_mount_pitch_deg: float | None = None
    camera_mount_yaw_deg: float | None = None
    camera_mount_roll_deg: float | None = None


class Waypoint(BaseModel):
    lat: float
    lon: float
    alt: float


class RouteMessage(BaseModel):
    """Route upload/visualization message."""

    type: Literal["route"] = "route"
    uav_id: str
    version: int
    waypoints: List[Waypoint]
    active_index: int | None = None


class ObjectMessage(BaseModel):
    """Confirmed/known object report."""

    type: Literal["object"] = "object"
    uav_id: str
    object_id: str
    class_id: int
    confidence: float
    lat: float
    lon: float
    alt: float | None = None
    status: str


AnyMessage = Union[TelemetryMessage, RouteMessage, ObjectMessage]


def make_telemetry(uav_id: str, sample: TelemetrySample) -> dict:
    """Build a telemetry message dict from TelemetrySample."""
    msg = TelemetryMessage(
        uav_id=uav_id,
        timestamp=sample.timestamp,
        lat=sample.lat,
        lon=sample.lon,
        alt=sample.alt,
        yaw=sample.yaw,
        battery=sample.battery,
    )
    return msg.model_dump()


def make_route(uav_id: str, route: Route) -> dict:
    """Build a route message dict from Route."""
    wps = [Waypoint(lat=wp.lat, lon=wp.lon, alt=wp.alt) for wp in route.waypoints]
    msg = RouteMessage(
        uav_id=uav_id,
        version=route.version,
        waypoints=wps,
        active_index=route.active_index,
    )
    return msg.model_dump()


def make_object(uav_id: str, obj: GeoDetection, status: str = "confirmed") -> dict:
    """Build an object message dict from GeoDetection."""
    msg = ObjectMessage(
        uav_id=uav_id,
        object_id=obj.object_id or (obj.frame_id or "unknown"),
        class_id=obj.class_id,
        confidence=obj.confidence,
        lat=obj.lat,
        lon=obj.lon,
        alt=obj.alt,
        status=status,
    )
    return msg.model_dump()


class MapBounds(BaseModel):
    lat_min: float
    lon_min: float
    lat_max: float
    lon_max: float


class MapSnapshotInfo(BaseModel):
    uav_id: str
    image_path: str
    bounds: MapBounds
    timestamp: datetime


def make_map_snapshot_info(uav_id: str, image_path: str, bounds: MapBounds) -> MapSnapshotInfo:
    """Build a MapSnapshotInfo with the current UTC timestamp."""
    return MapSnapshotInfo(
        uav_id=uav_id,
        image_path=image_path,
        bounds=bounds,
        timestamp=utc_now(),
    )


__all__ = [
    "TelemetryMessage",
    "RouteMessage",
    "ObjectMessage",
    "AnyMessage",
    "Waypoint",
    "MapBounds",
    "MapSnapshotInfo",
    "make_telemetry",
    "make_route",
    "make_object",
    "make_map_snapshot_info",
]
