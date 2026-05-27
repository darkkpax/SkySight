from __future__ import annotations

import logging

from fire_uav.module_core.detections.notifications import JsonNotificationWriter
from fire_uav.module_core.detections.registry import ObjectRegistry
from fire_uav.module_core.geometry import haversine_m
from fire_uav.module_core.schema import GeoDetection
from fire_uav.services.bus import Event, bus

_POSITION_UPDATE_THRESHOLD_M: float = 10.0  # re-emit UI event when position shifts this much


class ObjectNotificationManager:
    def __init__(
        self,
        registry: ObjectRegistry,
        writer: JsonNotificationWriter,
        logger: logging.Logger,
        uav_id: str | None,
    ) -> None:
        self.registry = registry
        self.writer = writer
        self.log = logger
        self.uav_id = uav_id

    def handle_confirmed_detection(self, detection: GeoDetection) -> None:
        prev_lat: float | None = None
        prev_lon: float | None = None

        # Snapshot position before update to detect significant drift.
        existing = (
            self.registry.find_by_track(detection.track_id, detection.class_id)
            if detection.track_id is not None else None
        )
        if existing is not None and existing.notified:
            prev_lat, prev_lon = existing.lat, existing.lon

        state = self.registry.create_or_update(
            detection,
            uav_id=self.uav_id,
            track_id=detection.track_id,
        )

        def _emit_ui() -> None:
            bus.emit(
                Event.OBJECT_CONFIRMED_UI,
                {
                    "object_id": state.object_id,
                    "class_id": state.class_id,
                    "confidence": state.confidence,
                    "lat": state.lat,
                    "lon": state.lon,
                    "track_id": state.track_id,
                    "timestamp": state.last_seen,
                },
            )

        if not state.notified:
            state.notified = True
            self.writer.write_notification(state)
            self.log.info(
                "Confirmed object %s (class %d, track %s, conf %.2f) at lat=%.6f, lon=%.6f",
                state.object_id,
                state.class_id,
                state.track_id,
                state.confidence,
                state.lat,
                state.lon,
            )
            _emit_ui()
        else:
            # Re-emit when the raw detection has shifted enough from the last
            # emitted position to matter on the map.  Compare against the raw
            # detection coordinates (before EMA), which reflects true movement;
            # comparing against the EMA-smoothed state.lat would understate a
            # real 35 m shift as ~8.75 m and never cross the threshold.
            if prev_lat is not None and prev_lon is not None:
                drift_m = haversine_m((prev_lat, prev_lon), (detection.lat, detection.lon))
                if drift_m >= _POSITION_UPDATE_THRESHOLD_M:
                    _emit_ui()
                    self.log.debug(
                        "Position update for object %s: drift=%.1fm → lat=%.6f lon=%.6f",
                        state.object_id,
                        drift_m,
                        state.lat,
                        state.lon,
                    )
                    return
            self.log.debug(
                "Updated object %s (track=%s) last_seen=%s",
                state.object_id,
                state.track_id,
                state.last_seen.isoformat(),
            )


__all__ = ["ObjectNotificationManager"]
