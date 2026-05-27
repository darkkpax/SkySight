from __future__ import annotations

import logging
import math
import time
from pathlib import Path
from typing import Callable

import numpy as np
from PySide6.QtCore import QTimer

from fire_uav.config import settings
import fire_uav.infrastructure.providers as deps
from fire_uav.module_core.geometry import EARTH_RADIUS_M, haversine_m, interpolate_path_point, offset_latlon
from fire_uav.module_core.schema import GeoDetection, TelemetrySample
from fire_uav.module_core.detections.notifications import JsonNotificationWriter
from fire_uav.module_core.detections.registry import ObjectRegistry
from fire_uav.module_core.detections.manager import ObjectNotificationManager
from fire_uav.utils.time import utc_now

_log = logging.getLogger(__name__)


class DebugSimulationService:
    def __init__(
        self,
        *,
        route_provider: Callable[[], list[tuple[float, float]]],
        telemetry_callback: Callable[[TelemetrySample], None],
        frame_callback: Callable[[np.ndarray], None] | None = None,
    ) -> None:
        self._route_provider = route_provider
        self._telemetry_callback = telemetry_callback
        self._frame_callback = frame_callback
        self._telemetry_timer = QTimer()
        self._telemetry_timer.setInterval(100)
        self._telemetry_timer.timeout.connect(self._on_telemetry_tick)
        self._camera_timer = QTimer()
        self._camera_timer.setInterval(200)
        self._camera_timer.timeout.connect(self._on_camera_tick)
        self._last_tick: float | None = None
        self._progress = 0.0
        self._distance_m = 0.0
        self._last_pos: tuple[float, float] | None = None
        self._last_alt: float | None = None
        self._speed_mps = 8.0
        self._max_distance_m = float(getattr(settings, "max_flight_distance_m", 15000.0) or 15000.0)
        self._completed = False
        self._frame_size = (360, 640)
        self._bridge_mode: bool = False
        self._bridge_source_log_emitted = False
        self._object_registry = ObjectRegistry()
        notifications_dir = Path(getattr(settings, "notifications_dir", "data/notifications"))
        self._object_manager = ObjectNotificationManager(
            registry=self._object_registry,
            writer=JsonNotificationWriter(notifications_dir),
            logger=logging.getLogger(__name__),
            uav_id=getattr(settings, "uav_id", None),
        )

    def set_bridge_mode(self, enabled: bool) -> None:
        self._bridge_mode = bool(enabled)
        self._bridge_source_log_emitted = False

    def telemetry_enabled(self) -> bool:
        return self._telemetry_timer.isActive()

    def camera_enabled(self) -> bool:
        return self._camera_timer.isActive()

    def set_telemetry_enabled(self, enabled: bool) -> None:
        if enabled and not self._telemetry_timer.isActive():
            self._last_tick = time.monotonic()
            self._telemetry_timer.start()
        elif not enabled and self._telemetry_timer.isActive():
            self._telemetry_timer.stop()
            self._last_tick = None

    def set_camera_enabled(self, enabled: bool) -> None:
        if enabled and not self._camera_timer.isActive():
            self._camera_timer.start()
        elif not enabled and self._camera_timer.isActive():
            self._camera_timer.stop()

    def reset_progress(self) -> None:
        self._progress = 0.0
        self._distance_m = 0.0
        self._last_pos = None
        self._last_alt = None
        self._last_tick = time.monotonic()
        self._completed = False

    def set_speed_mps(self, speed_mps: float) -> None:
        try:
            value = float(speed_mps)
        except (TypeError, ValueError):
            return
        self._speed_mps = max(1.0, min(40.0, value))

    def spawn_confirmed_object(self) -> GeoDetection:
        lat, lon = self._last_pos or self._fallback_center()
        det = GeoDetection(
            class_id=0,
            confidence=0.92,
            lat=float(lat),
            lon=float(lon),
            alt=120.0,
            frame_id="debug",
            timestamp=utc_now(),
            track_id=None,
        )
        self._object_manager.handle_confirmed_detection(det)
        return det

    # ------------------------------------------------------------------ #
    def _on_telemetry_tick(self) -> None:
        now = time.monotonic()
        dt = 0.0 if self._last_tick is None else max(0.0, now - self._last_tick)
        self._last_tick = now
        route = self._route_provider() or []
        route_len = self._route_length(route)
        if route_len > 0:
            if not self._completed:
                self._progress = min(1.0, self._progress + (self._speed_mps * dt / route_len))
                if self._progress >= 1.0:
                    self._completed = True
            pos = interpolate_path_point(route, self._progress)
        else:
            self._progress = (self._progress + 0.01) % 1.0
            pos = self._circle_position(self._progress)
        if pos is None:
            return
        deps.debug_flight_progress = self._progress
        deps.debug_flight_completed = bool(self._completed)
        prev_pos = self._last_pos
        prev_alt = self._last_alt
        current_alt = 120.0
        if prev_pos is not None and not self._completed:
            self._distance_m += haversine_m(prev_pos, pos)
        vx = vy = vz = 0.0
        if prev_pos is not None and dt > 0:
            lat1, lon1 = prev_pos
            lat2, lon2 = pos
            avg_lat_rad = math.radians((lat1 + lat2) / 2.0)
            dx = math.radians(lon2 - lon1) * EARTH_RADIUS_M * math.cos(avg_lat_rad)
            dy = math.radians(lat2 - lat1) * EARTH_RADIUS_M
            vx = dx / dt
            vy = dy / dt
            if prev_alt is not None:
                vz = (current_alt - prev_alt) / dt
        source_id = str(getattr(settings, "uav_id", None) or "uav")
        if self._bridge_mode:
            source = source_id
            if not self._bridge_source_log_emitted:
                _log.info("Bridge telemetry source %s", source)
                self._bridge_source_log_emitted = True
        else:
            source = "debug"
        battery_percent = 100.0
        if self._max_distance_m > 0:
            battery_percent = max(0.0, 100.0 - (self._distance_m / self._max_distance_m * 100.0))
        sample = TelemetrySample(
            lat=pos[0],
            lon=pos[1],
            alt=current_alt,
            yaw=(self._progress * 360.0) % 360.0,
            pitch=0.0,
            roll=0.0,
            vx=vx,
            vy=vy,
            vz=vz,
            battery=battery_percent / 100.0,
            battery_percent=battery_percent,
            timestamp=utc_now(),
            source=source,
        )
        self._last_pos = pos
        self._last_alt = current_alt
        self._telemetry_callback(sample)

    def _on_camera_tick(self) -> None:
        if self._frame_callback is None:
            return
        h, w = self._frame_size
        frame = np.zeros((h, w, 3), dtype=np.uint8)
        t = time.monotonic()
        cx = int((t * 40) % w)
        cy = int((t * 30) % h)
        frame[max(0, cy - 3) : cy + 3, max(0, cx - 3) : cx + 3] = (90, 210, 255)
        self._frame_callback(frame)

    def _route_length(self, pts: list[tuple[float, float]]) -> float:
        distance = 0.0
        for a, b in zip(pts[:-1], pts[1:]):
            distance += haversine_m(a, b)
        return distance

    def _circle_position(self, progress: float) -> tuple[float, float]:
        center = self._fallback_center()
        angle = progress * 2.0 * math.pi
        dx = math.cos(angle) * 40.0
        dy = math.sin(angle) * 40.0
        return offset_latlon(center[0], center[1], dx, dy)

    def _fallback_center(self) -> tuple[float, float]:
        center = getattr(settings, "map_center", None)
        if isinstance(center, (list, tuple)) and len(center) >= 2:
            return float(center[0]), float(center[1])
        return 56.02, 92.90


__all__ = ["DebugSimulationService"]
