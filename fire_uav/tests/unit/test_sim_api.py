# mypy: ignore-errors
from __future__ import annotations

from fastapi.testclient import TestClient

import fire_uav.infrastructure.providers as deps
from fire_uav.api.sim_api import app, capabilities
from fire_uav.services.bus import Event, bus
from fire_uav.utils.time import utc_iso_z

client = TestClient(app)


def test_handshake_emits_capabilities() -> None:
    received: list[dict] = []

    def _capture(payload: object) -> None:
        if isinstance(payload, dict):
            received.append(payload)

    bus.subscribe(Event.CAPABILITIES_UPDATED, _capture)

    payload = {
        "uav_id": "uav-handshake",
        "client_type": "drone_bridge",
        "client_version": "1.0.0",
        "capabilities": {
            "supports_waypoints": True,
            "supports_rtl": True,
            "supports_orbit": False,
            "supports_set_speed": False,
            "supports_camera": True,
            "max_waypoints": 40,
            "notes": "test",
        },
    }
    response = client.post("/sim/v1/handshake", json=payload)
    assert response.status_code == 200
    assert "uav-handshake" in capabilities
    assert received
    assert received[-1]["uav_id"] == "uav-handshake"


def test_telemetry_ingest_and_health() -> None:
    msg = {
        "protocol_version": 1,
        "uav_id": "uav-telemetry",
        "timestamp": utc_iso_z(),
        "lat": 56.0,
        "lon": 92.9,
        "alt": 120.0,
    }
    response = client.post("/sim/v1/telemetry", json=msg)
    assert response.status_code == 200

    health = client.get("/sim/v1/health/uav-telemetry").json()
    assert health["link_status"] == "CONNECTED"
    assert health["camera_status"] == "NOT_READY"


def test_health_when_missing_telemetry() -> None:
    health = client.get("/sim/v1/health/uav-missing").json()
    assert health["link_status"] == "DISCONNECTED"
    assert health["message"] == "telemetry_missing"


def test_route_poll_uses_plan_data() -> None:
    previous = deps.plan_data
    deps.plan_data = {"path": [(1.0, 2.0), (3.0, 4.0)]}
    try:
        response = client.get("/sim/v1/route/uav-route")
        assert response.status_code == 200
        body = response.json()
        assert body["mode"] == "MISSION"
        assert len(body["waypoints"]) == 2
    finally:
        deps.plan_data = previous
