from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from typing import Dict, List, Tuple

from fire_uav.module_core.geometry import haversine_m
from fire_uav.module_core.schema import GeoDetection


@dataclass
class TrackedObjectState:
    object_id: str
    track_id: int | None
    class_id: int
    confidence: float
    lat: float
    lon: float
    alt: float | None
    first_seen: datetime
    last_seen: datetime
    frames: List[str] = field(default_factory=list)
    uav_id: str | None = None
    notified: bool = False


_POSITION_SMOOTH_ALPHA: float = 0.25  # weight given to newest detection vs stored position


class ObjectRegistry:
    def __init__(self, *, spatial_match_radius_m: float = 80.0) -> None:
        self._objects: Dict[str, TrackedObjectState] = {}
        self._by_track: Dict[Tuple[int, int], str] = {}
        self._counter: int = 0
        self._spatial_match_radius_m: float = max(1.0, float(spatial_match_radius_m))

    def _new_object_id(self) -> str:
        oid = f"obj_{self._counter:06d}"
        self._counter += 1
        return oid

    def find_by_track(self, track_id: int, class_id: int) -> TrackedObjectState | None:
        key = (track_id, class_id)
        obj_id = self._by_track.get(key)
        if obj_id is None:
            return None
        return self._objects.get(obj_id)

    def _find_spatial(self, detection: GeoDetection) -> TrackedObjectState | None:
        """Fallback matching when track_id is unavailable: find closest object of same class."""
        closest: TrackedObjectState | None = None
        closest_dist = float("inf")
        for state in self._objects.values():
            if state.class_id != detection.class_id:
                continue
            dist = haversine_m((state.lat, state.lon), (detection.lat, detection.lon))
            if dist < self._spatial_match_radius_m and dist < closest_dist:
                closest = state
                closest_dist = dist
        return closest

    def create_or_update(
        self,
        detection: GeoDetection,
        uav_id: str | None,
        track_id: int | None,
    ) -> TrackedObjectState:
        state: TrackedObjectState | None = None
        if track_id is not None:
            state = self.find_by_track(track_id, detection.class_id)

        if state is None:
            state = self._find_spatial(detection)

        if state is None:
            state = TrackedObjectState(
                object_id=self._new_object_id(),
                track_id=track_id,
                class_id=detection.class_id,
                confidence=detection.confidence,
                lat=detection.lat,
                lon=detection.lon,
                alt=detection.alt,
                first_seen=detection.timestamp,
                last_seen=detection.timestamp,
                frames=[detection.frame_id] if detection.frame_id else [],
                uav_id=uav_id,
            )
            self._objects[state.object_id] = state
            if track_id is not None:
                self._by_track[(track_id, detection.class_id)] = state.object_id
            return state

        # Update existing — smooth position to reduce projection noise.
        state.last_seen = detection.timestamp
        state.confidence = max(state.confidence, detection.confidence)
        state.lat = (1.0 - _POSITION_SMOOTH_ALPHA) * state.lat + _POSITION_SMOOTH_ALPHA * detection.lat
        state.lon = (1.0 - _POSITION_SMOOTH_ALPHA) * state.lon + _POSITION_SMOOTH_ALPHA * detection.lon
        state.alt = detection.alt
        if detection.frame_id:
            state.frames.append(detection.frame_id)
        if track_id is not None:
            state.track_id = track_id
            self._by_track[(track_id, detection.class_id)] = state.object_id
        if uav_id is not None:
            state.uav_id = uav_id
        return state

