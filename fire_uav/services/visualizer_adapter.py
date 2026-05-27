from __future__ import annotations

import logging

from fire_uav.module_core.schema import GeoDetection, Route, TelemetrySample, Waypoint
from fire_uav.services.visualizer_client import VisualizerClient


class VisualizerAdapter:
    """
    Bridges module_core state to the ground-side visualizer API.

    settings should expose:
      - visualizer_enabled: bool
      - visualizer_url: str
      - uav_id: str
    """

    def __init__(self, settings) -> None:  # noqa: ANN001
        self._log = logging.getLogger(__name__)
        self._enabled = getattr(settings, "visualizer_enabled", False)
        self._client: VisualizerClient | None = None
        if self._enabled:
            self._client = VisualizerClient(
                base_url=getattr(settings, "visualizer_url", "http://127.0.0.1:8000"),
                uav_id=getattr(settings, "uav_id", "uav"),
            )
            self._log.info("VisualizerAdapter: enabled for UAV %s", getattr(settings, "uav_id", "uav"))
        else:
            self._log.info("VisualizerAdapter: disabled in settings")

    async def publish_telemetry(self, telemetry: TelemetrySample) -> None:
        if not self._client:
            return
        await self._client.send_telemetry(
            timestamp=telemetry.timestamp,
            lat=telemetry.lat,
            lon=telemetry.lon,
            alt=telemetry.alt,
            yaw=telemetry.yaw,
            battery=telemetry.battery,
        )

    async def publish_route(self, route: Route) -> None:
        if not self._client:
            return
        wps = [Waypoint(lat=wp.lat, lon=wp.lon, alt=wp.alt) for wp in route.waypoints]
        await self._client.send_route(
            version=route.version,
            waypoints=wps,
            active_index=route.active_index,
        )

    async def publish_object(self, det: GeoDetection) -> None:
        if not self._client:
            return
        object_id = det.object_id or (det.frame_id or "unknown")
        await self._client.send_object(
            object_id=object_id,
            class_id=det.class_id,
            confidence=det.confidence,
            lat=det.lat,
            lon=det.lon,
            alt=det.alt,
            status="confirmed",
        )

    async def aclose(self) -> None:
        if self._client:
            await self._client.aclose()

