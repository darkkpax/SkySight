from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Callable

from fire_uav.module_core.geometry import haversine_m
from fire_uav.services.bus import Event, bus

def _get_dedup_radius() -> float:
    from fire_uav.config.settings import settings  # late import avoids circular dependency
    return float(getattr(settings, "ui_spatial_dedup_radius_m", 80.0) or 80.0)


def _get_smooth_alpha() -> float:
    from fire_uav.config.settings import settings
    return float(getattr(settings, "bbox_smooth_alpha", 0.25) or 0.25)

@dataclass(slots=True)
class ConfirmedObject:
    object_id: str
    class_id: int
    confidence: float
    lat: float
    lon: float
    track_id: int | None
    timestamp: datetime | None


log = logging.getLogger(__name__)

class ConfirmedObjectsStore:
    def __init__(self, on_change: Callable[[], None] | None = None) -> None:
        self._objects: dict[str, ConfirmedObject] = {}
        self._order: list[str] = []
        self._selected_id: str | None = None
        self._on_change = on_change
        bus.subscribe(Event.OBJECT_CONFIRMED_UI, self._on_confirmed)

    def all(self) -> list[ConfirmedObject]:
        return [self._objects[obj_id] for obj_id in self._order if obj_id in self._objects]

    def count(self) -> int:
        return len(self._objects)

    def get(self, object_id: str) -> ConfirmedObject | None:
        return self._objects.get(object_id)

    def selected(self) -> ConfirmedObject | None:
        if self._selected_id is None:
            return None
        return self._objects.get(self._selected_id)

    def set_selected(self, object_id: str | None) -> None:
        self._selected_id = object_id if object_id in self._objects else None

    def latest(self) -> ConfirmedObject | None:
        if not self._order:
            return None
        return self._objects.get(self._order[-1])

    def clear(self) -> None:
        self._objects.clear()
        self._order.clear()
        self._selected_id = None
        if self._on_change:
            self._on_change()

    # ------------------------------------------------------------------ #
    def _on_confirmed(self, payload: object) -> None:
        if not isinstance(payload, dict):
            return
        object_id = str(payload.get("object_id", ""))
        if not object_id:
            return
        timestamp = self._parse_timestamp(payload.get("timestamp"))
        try:
            lat = float(payload.get("lat", 0.0))
            lon = float(payload.get("lon", 0.0))
            class_id = int(payload.get("class_id", -1))
        except (TypeError, ValueError):
            return
        obj = ConfirmedObject(
            object_id=object_id,
            class_id=class_id,
            confidence=float(payload.get("confidence", 0.0)),
            lat=lat,
            lon=lon,
            track_id=payload.get("track_id"),
            timestamp=timestamp,
        )

        is_new = object_id not in self._objects

        if is_new and not object_id.startswith("manual-"):
            # Last-resort spatial dedup: if a same-class object already exists
            # within _SPATIAL_DEDUP_RADIUS_M, this is a duplicate — update the
            # existing marker instead of creating a second one.
            duplicate_id = self._find_spatial_duplicate(obj)
            if duplicate_id is not None:
                existing = self._objects[duplicate_id]
                alpha = _get_smooth_alpha()
                existing.lat = (1.0 - alpha) * existing.lat + alpha * obj.lat
                existing.lon = (1.0 - alpha) * existing.lon + alpha * obj.lon
                if obj.confidence > existing.confidence:
                    existing.confidence = obj.confidence
                log.debug(
                    "Spatial dedup: merged %s into %s (dist=%.1fm)",
                    object_id,
                    duplicate_id,
                    haversine_m((existing.lat, existing.lon), (obj.lat, obj.lon)),
                )
                if self._on_change:
                    self._on_change()
                return

        self._objects[object_id] = obj
        if is_new:
            self._order.append(object_id)
            if object_id.startswith("manual-"):
                log.info(
                    "MANUAL_SPAWN stored id=%s total=%s",
                    object_id,
                    self.count(),
                )
        if self._on_change:
            self._on_change()

    def _find_spatial_duplicate(self, obj: ConfirmedObject) -> str | None:
        """Return the object_id of the closest existing same-class object within radius, or None."""
        radius = _get_dedup_radius()
        best_id: str | None = None
        best_dist = float("inf")
        for existing_id, existing in self._objects.items():
            if existing.class_id != obj.class_id:
                continue
            dist = haversine_m((existing.lat, existing.lon), (obj.lat, obj.lon))
            if dist < radius and dist < best_dist:
                best_id = existing_id
                best_dist = dist
        return best_id

    @staticmethod
    def _parse_timestamp(value: object) -> datetime | None:
        if isinstance(value, datetime):
            if value.tzinfo is None:
                return value
            return value.astimezone(timezone.utc)
        if isinstance(value, str):
            text = value.strip()
            if not text:
                return None
            try:
                if text.endswith("Z"):
                    text = f"{text[:-1]}+00:00"
                parsed = datetime.fromisoformat(text)
                if parsed.tzinfo is None:
                    return parsed
                return parsed.astimezone(timezone.utc)
            except ValueError:
                return None
        return None


__all__ = ["ConfirmedObjectsStore", "ConfirmedObject"]
