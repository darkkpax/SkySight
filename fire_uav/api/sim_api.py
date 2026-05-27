from __future__ import annotations

from collections import defaultdict, deque
from typing import Deque, Dict, List

from fastapi import Depends, FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import ORJSONResponse
from pydantic import BaseModel
from prometheus_fastapi_instrumentator import Instrumentator

import fire_uav.infrastructure.providers as deps
from fire_uav.api.security import require_api_key
from fire_uav.module_core.contract.v1 import (
    CameraStatusV1,
    CapabilitiesV1,
    HandshakeRequestV1,
    HandshakeResponseV1,
    HealthV1,
    RouteV1,
    TelemetryV1,
    WaypointV1,
)
from fire_uav.services.bus import Event, bus
from fire_uav.services.mission.camera_monitor import CameraMonitor
from fire_uav.services.mission.link_monitor import LinkMonitor
from fire_uav.services.mission.state import MissionState
from fire_uav.services.telemetry_ingest import TelemetryIngestContext, ingest_telemetry
from fire_uav.utils.time import utc_now

app = FastAPI(
    title="Skysight sim API",
    version="1.0.0",
    default_response_class=ORJSONResponse,
    dependencies=[Depends(require_api_key)],
)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)
app.add_middleware(GZipMiddleware, minimum_size=512)
Instrumentator().instrument(app).expose(app)

last_telemetry: Dict[str, TelemetryV1] = {}
telemetry_history: Dict[str, Deque[TelemetryV1]] = defaultdict(lambda: deque(maxlen=50))
last_camera_status: Dict[str, CameraStatusV1] = {}
capabilities: Dict[str, CapabilitiesV1] = {}
active_route: Dict[str, RouteV1] = {}
last_command_ack: Dict[str, dict] = {}

_link_monitors: Dict[str, LinkMonitor] = {}
_camera_monitors: Dict[str, CameraMonitor] = {}
_current_mission_state: MissionState = MissionState.PREFLIGHT


class CommandAck(BaseModel):
    uav_id: str
    command_id: str
    ok: bool
    message: str | None = None


def _get_link_monitor(uav_id: str) -> LinkMonitor:
    monitor = _link_monitors.get(uav_id)
    if monitor is None:
        monitor = LinkMonitor()
        _link_monitors[uav_id] = monitor
    return monitor


def _get_camera_monitor(uav_id: str) -> CameraMonitor:
    monitor = _camera_monitors.get(uav_id)
    if monitor is None:
        monitor = CameraMonitor()
        _camera_monitors[uav_id] = monitor
    return monitor


def _get_ingest_context(uav_id: str) -> TelemetryIngestContext:
    return TelemetryIngestContext(link_monitor=_get_link_monitor(uav_id))


def _resolve_route_path() -> tuple[List[tuple[float, float]], str]:
    if _current_mission_state == MissionState.RTL and deps.rtl_path:
        return list(deps.rtl_path), "RTL"
    if deps.debug_orbit_path:
        return list(deps.debug_orbit_path), "ORBIT"
    plan = deps.plan_data or {}
    path = plan.get("path") or []
    return list(path), "MISSION"


def _build_route(uav_id: str, path: List[tuple[float, float]], mode: str) -> RouteV1:
    last = last_telemetry.get(uav_id)
    alt = last.alt if last else 120.0
    waypoints = [WaypointV1(lat=lat, lon=lon, alt=alt) for lat, lon in path]
    route = RouteV1(
        uav_id=uav_id,
        route_id=f"{uav_id}:{mode.lower()}:{int(utc_now().timestamp())}",
        waypoints=waypoints,
        mode=mode,
        created_at=utc_now(),
    )
    return route


def _on_mission_state(payload: object) -> None:
    global _current_mission_state
    if not isinstance(payload, dict):
        return
    raw = payload.get("state")
    if raw is None:
        return
    try:
        _current_mission_state = MissionState(str(raw))
    except Exception:
        return


bus.subscribe(Event.MISSION_STATE_CHANGED, _on_mission_state)


@app.get("/")
def root() -> dict[str, str]:
    return {"service": "skysight_sim_api"}


@app.post("/sim/v1/handshake", response_model=HandshakeResponseV1)
async def handshake(req: HandshakeRequestV1) -> HandshakeResponseV1:
    if req.capabilities is not None:
        capabilities[req.uav_id] = req.capabilities
        bus.emit(
            Event.CAPABILITIES_UPDATED,
            {"uav_id": req.uav_id, "capabilities": req.capabilities.model_dump()},
        )
    required = {
        "telemetry": ["uav_id", "timestamp", "lat", "lon", "alt"],
        "camera_status": ["uav_id", "timestamp", "available", "streaming"],
    }
    return HandshakeResponseV1(
        ok=True,
        server_time=utc_now(),
        required_fields=required,
        assigned_uav_id=req.uav_id,
        notes="contract_v1",
    )


@app.post("/sim/v1/telemetry")
async def post_telemetry(msg: TelemetryV1) -> dict[str, str]:
    last_telemetry[msg.uav_id] = msg
    telemetry_history[msg.uav_id].append(msg)
    await ingest_telemetry(msg, context=_get_ingest_context(msg.uav_id))
    return {"status": "ok"}


@app.post("/sim/v1/camera_status")
async def post_camera_status(msg: CameraStatusV1) -> dict[str, str]:
    last_camera_status[msg.uav_id] = msg
    monitor = _get_camera_monitor(msg.uav_id)
    monitor.on_status(
        available=msg.available,
        streaming=msg.streaming,
        fps=msg.fps,
        last_frame_age_s=msg.last_frame_age_s,
    )
    return {"status": "ok"}


@app.get("/sim/v1/route/{uav_id}", response_model=RouteV1)
def get_route(uav_id: str) -> RouteV1:
    path, mode = _resolve_route_path()
    if path:
        route = _build_route(uav_id, path, mode)
        active_route[uav_id] = route
        return route
    route = active_route.get(uav_id)
    if route is None:
        raise HTTPException(status_code=404, detail="route not found")
    return route


@app.post("/sim/v1/command_ack")
async def command_ack(payload: CommandAck) -> dict[str, str]:
    last_command_ack[payload.uav_id] = payload.model_dump()
    bus.emit(
        Event.COMMAND_ACK,
        {
            "uav_id": payload.uav_id,
            "command_id": payload.command_id,
            "ok": payload.ok,
            "message": payload.message,
        },
    )
    return {"status": "ok"}


@app.get("/sim/v1/health/{uav_id}", response_model=HealthV1)
def get_health(uav_id: str) -> HealthV1:
    link_status = _get_link_monitor(uav_id).status.value
    camera_status = _get_camera_monitor(uav_id).status.value
    telemetry = last_telemetry.get(uav_id)
    battery = telemetry.battery_percent if telemetry else None
    message = None
    if telemetry is None:
        message = "telemetry_missing"
    return HealthV1(
        uav_id=uav_id,
        timestamp=utc_now(),
        link_status=link_status,
        camera_status=camera_status,
        battery_percent=battery,
        message=message,
    )


__all__ = [
    "app",
    "last_telemetry",
    "last_camera_status",
    "capabilities",
    "active_route",
    "last_command_ack",
]
