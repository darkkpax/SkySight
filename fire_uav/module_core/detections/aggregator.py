from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from typing import Deque, List, Sequence

from fire_uav.module_core.geometry import haversine_m
from fire_uav.module_core.schema import GeoDetection, WorldCoord


@dataclass
class DetectionEvent:
    class_id: int
    confidence: float
    location: WorldCoord
    frame_id: str
    timestamp: datetime
    track_id: int | None = None


@dataclass
class _Cluster:
    class_id: int
    events: Deque[DetectionEvent] = field(default_factory=deque)
    last_reported: datetime | None = None
    last_centroid: WorldCoord | None = None

    def add(self, event: DetectionEvent, maxlen: int) -> None:
        self.events.append(event)
        while len(self.events) > maxlen:
            self.events.popleft()

    def centroid(self) -> WorldCoord:
        lat = sum(ev.location.lat for ev in self.events) / len(self.events)
        lon = sum(ev.location.lon for ev in self.events) / len(self.events)
        c = WorldCoord(lat=lat, lon=lon)
        self.last_centroid = c
        return c

    def votes(self) -> int:
        return len({ev.frame_id for ev in self.events})

    def avg_conf(self) -> float:
        return sum(ev.confidence for ev in self.events) / len(self.events)

    def ref_location(self) -> WorldCoord | None:
        """Most recent known position: last event or last confirmed centroid."""
        if self.events:
            return self.events[-1].location
        return self.last_centroid


class DetectionAggregator:
    """Хранит sliding-window детекций и выполняет голосование K из N кадров."""

    def __init__(
        self,
        *,
        window: int,
        votes_required: int,
        min_confidence: float,
        max_distance_m: float,
        ttl_seconds: float,
    ) -> None:
        self.window = max(1, window)
        self.votes_required = max(1, votes_required)
        self.min_confidence = min_confidence
        self.max_distance_m = max(1.0, max_distance_m)
        self.ttl = timedelta(seconds=max(1.0, ttl_seconds))
        self._clusters: List[_Cluster] = []

    def add_event(self, event: DetectionEvent) -> GeoDetection | None:
        cluster = self._find_cluster(event)
        cluster.add(event, self.window)
        detection: GeoDetection | None = None
        if cluster.votes() >= self.votes_required and cluster.avg_conf() >= self.min_confidence:
            centroid = cluster.centroid()
            detection = GeoDetection(
                track_id=event.track_id,
                lat=centroid.lat,
                lon=centroid.lon,
                class_id=event.class_id,
                confidence=cluster.avg_conf(),
                timestamp=event.timestamp,
                frame_id=event.frame_id,
            )
            cluster.last_reported = event.timestamp
            cluster.events.clear()

        self._cleanup(event.timestamp)
        return detection

    def add_many(self, events: Sequence[DetectionEvent]) -> List[GeoDetection]:
        aggregated: List[GeoDetection] = []
        for event in events:
            det = self.add_event(event)
            if det:
                aggregated.append(det)
        return aggregated

    # ------------------------------------------------------------------ #
    def _find_cluster(self, event: DetectionEvent) -> _Cluster:
        closest: _Cluster | None = None
        closest_dist = float("inf")
        for cluster in self._clusters:
            if cluster.class_id != event.class_id:
                continue
            ref = cluster.ref_location()
            if ref is None:
                continue
            dist = haversine_m(
                (ref.lat, ref.lon),
                (event.location.lat, event.location.lon),
            )
            if dist <= self.max_distance_m and dist < closest_dist:
                closest = cluster
                closest_dist = dist

        if closest is None:
            closest = _Cluster(class_id=event.class_id)
            self._clusters.append(closest)
        return closest

    def _cleanup(self, now: datetime) -> None:
        for cluster in list(self._clusters):
            if not cluster.events:
                if cluster.last_reported is None or now - cluster.last_reported > self.ttl:
                    self._clusters.remove(cluster)
                continue
            if now - cluster.events[-1].timestamp > self.ttl:
                self._clusters.remove(cluster)
