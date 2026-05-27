# mypy: ignore-errors
from __future__ import annotations

import pytest
from pydantic import ValidationError

from fire_uav.module_core.contract.mappers import (
    capabilities_internal_to_v1,
    capabilities_v1_to_internal,
    route_internal_to_v1,
    route_v1_to_internal,
    telemetry_v1_to_sample,
)
from fire_uav.module_core.contract.v1 import CapabilitiesV1, TelemetryV1
from fire_uav.module_core.schema import Route, Waypoint
from fire_uav.utils.time import utc_now


def test_telemetry_v1_requires_min_fields() -> None:
    payload = {
        "uav_id": "uav-1",
        "timestamp": utc_now(),
        "lat": 56.0,
        "lon": 92.9,
        "alt": 120.0,
    }
    msg = TelemetryV1(**payload)
    assert msg.protocol_version == 1

    with pytest.raises(ValidationError):
        TelemetryV1(lat=56.0, lon=92.9, alt=120.0, timestamp=utc_now())


def test_telemetry_mapping_roundtrip() -> None:
    msg = TelemetryV1(
        uav_id="uav-2",
        timestamp=utc_now(),
        lat=55.0,
        lon=90.0,
        alt=100.0,
        yaw=45.0,
        battery_percent=80.0,
    )
    sample = telemetry_v1_to_sample(msg)
    assert sample.lat == 55.0
    assert sample.lon == 90.0
    assert sample.alt == 100.0
    assert sample.battery_percent == 80.0


def test_route_mapping_roundtrip() -> None:
    route = Route(
        version=1,
        waypoints=[Waypoint(lat=1.0, lon=2.0, alt=3.0), Waypoint(lat=4.0, lon=5.0, alt=6.0)],
        active_index=0,
    )
    route_v1 = route_internal_to_v1(route, uav_id="uav-3")
    assert route_v1.uav_id == "uav-3"
    assert len(route_v1.waypoints) == 2

    internal = route_v1_to_internal(route_v1)
    assert len(internal.waypoints) == 2
    assert internal.waypoints[0].lat == 1.0


def test_capabilities_mapping() -> None:
    caps = CapabilitiesV1(
        supports_waypoints=True,
        supports_rtl=False,
        supports_orbit=False,
        supports_set_speed=False,
        supports_camera=True,
        max_waypoints=10,
        notes=None,
    )
    internal = capabilities_v1_to_internal(caps)
    roundtrip = capabilities_internal_to_v1(internal)
    assert roundtrip is not None
    assert roundtrip.supports_waypoints is True
    assert roundtrip.supports_rtl is False
