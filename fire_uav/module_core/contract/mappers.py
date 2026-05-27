"""Mapping helpers between Skysight contract v1 and internal models."""

from __future__ import annotations

from typing import TypedDict
from uuid import uuid4

from fire_uav.core.telemetry import normalize_battery_value
from fire_uav.module_core.contract.v1 import CapabilitiesV1, RouteV1, TelemetryV1, WaypointV1
from fire_uav.module_core.schema import Route, TelemetrySample, Waypoint
from fire_uav.utils.time import utc_now


class CapabilitiesDict(TypedDict, total=False):
    supports_waypoints: bool
    supports_rtl: bool
    supports_orbit: bool
    supports_set_speed: bool
    supports_camera: bool
    max_waypoints: int | None
    notes: str | None


def telemetry_v1_to_sample(msg: TelemetryV1) -> TelemetrySample:
    battery_fraction, battery_percent = normalize_battery_value(msg.battery_percent)
    return TelemetrySample(
        lat=msg.lat,
        lon=msg.lon,
        alt=msg.alt,
        yaw=msg.yaw or 0.0,
        pitch=msg.pitch or 0.0,
        roll=msg.roll or 0.0,
        vx=None,
        vy=None,
        vz=None,
        battery=battery_fraction,
        battery_percent=battery_percent,
        timestamp=msg.timestamp,
        source="contract_v1",
    )


def route_internal_to_v1(route: Route, *, uav_id: str, mode: str = "MISSION") -> RouteV1:
    waypoints = [WaypointV1(lat=wp.lat, lon=wp.lon, alt=wp.alt) for wp in route.waypoints]
    return RouteV1(
        uav_id=uav_id,
        route_id=f"route-{uuid4().hex}",
        waypoints=waypoints,
        mode=mode,
        created_at=utc_now(),
    )


def route_v1_to_internal(route: RouteV1) -> Route:
    waypoints = [Waypoint(lat=wp.lat, lon=wp.lon, alt=wp.alt) for wp in route.waypoints]
    active_index = 0 if waypoints else None
    return Route(version=1, waypoints=waypoints, active_index=active_index)


def capabilities_v1_to_internal(capabilities: CapabilitiesV1 | None) -> CapabilitiesDict | None:
    if capabilities is None:
        return None
    return CapabilitiesDict(
        supports_waypoints=capabilities.supports_waypoints,
        supports_rtl=capabilities.supports_rtl,
        supports_orbit=capabilities.supports_orbit,
        supports_set_speed=capabilities.supports_set_speed,
        supports_camera=capabilities.supports_camera,
        max_waypoints=capabilities.max_waypoints,
        notes=capabilities.notes,
    )


def capabilities_internal_to_v1(capabilities: CapabilitiesDict | None) -> CapabilitiesV1 | None:
    if capabilities is None:
        return None
    return CapabilitiesV1(
        supports_waypoints=bool(capabilities.get("supports_waypoints", True)),
        supports_rtl=bool(capabilities.get("supports_rtl", True)),
        supports_orbit=bool(capabilities.get("supports_orbit", True)),
        supports_set_speed=bool(capabilities.get("supports_set_speed", True)),
        supports_camera=bool(capabilities.get("supports_camera", True)),
        max_waypoints=capabilities.get("max_waypoints"),
        notes=capabilities.get("notes"),
    )


__all__ = [
    "CapabilitiesDict",
    "telemetry_v1_to_sample",
    "route_internal_to_v1",
    "route_v1_to_internal",
    "capabilities_v1_to_internal",
    "capabilities_internal_to_v1",
]
