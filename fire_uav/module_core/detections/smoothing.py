from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Dict, List, TYPE_CHECKING
from fire_uav.utils.time import utc_now

if TYPE_CHECKING:
    from fire_uav.module_core.detections.pipeline import RawDetectionPayload
try:
    import native_core as _native_core

    _NATIVE_TRACKER_AVAILABLE = hasattr(_native_core, "BBoxTracker")
except Exception:  # noqa: BLE001
    _native_core = None
    _NATIVE_TRACKER_AVAILABLE = False


@dataclass
class TrackState:
    bbox: tuple[float, float, float, float]
    class_id: int
    score: float
    hits: int
    missed: int
    last_seen: datetime


class BBoxSmoother:
    """
    Лёгкий IoU-трекер: сопоставление по IoU/центру, сглаживание боксов и время жизни трека.
    """

    def __init__(
        self,
        alpha: float = 0.5,
        max_center_distance_px: float = 80.0,
        iou_threshold: float = 0.25,
        max_age_seconds: float = 2.0,
        min_hits: int = 2,
        max_missed: int = 10,
    ) -> None:
        self.alpha = alpha
        self.max_center_distance_px = max_center_distance_px
        self.iou_threshold = iou_threshold
        self.max_age_seconds = max_age_seconds
        self.min_hits = min_hits
        self.max_missed = max_missed

        self._tracks: Dict[int, TrackState] = {}
        self._next_track_id: int = 0

    # ------------------------------------------------------------------ #
    @staticmethod
    def _center(bbox: tuple[float, float, float, float]) -> tuple[float, float]:
        x_min, y_min, x_max, y_max = bbox
        return (x_min + x_max) / 2.0, (y_min + y_max) / 2.0

    def _center_similarity(self, a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> float:
        if self.max_center_distance_px <= 0:
            return 0.0
        ax, ay = self._center(a)
        bx, by = self._center(b)
        dist = ((ax - bx) ** 2 + (ay - by) ** 2) ** 0.5
        if dist > self.max_center_distance_px:
            return 0.0
        return max(0.0, 1.0 - dist / self.max_center_distance_px)

    @staticmethod
    def _iou(box_a: tuple[float, float, float, float], box_b: tuple[float, float, float, float]) -> float:
        ax1, ay1, ax2, ay2 = box_a
        bx1, by1, bx2, by2 = box_b
        inter_x1 = max(ax1, bx1)
        inter_y1 = max(ay1, by1)
        inter_x2 = min(ax2, bx2)
        inter_y2 = min(ay2, by2)
        inter_w = max(0.0, inter_x2 - inter_x1)
        inter_h = max(0.0, inter_y2 - inter_y1)
        inter_area = inter_w * inter_h
        if inter_area <= 0:
            return 0.0
        area_a = max(0.0, ax2 - ax1) * max(0.0, ay2 - ay1)
        area_b = max(0.0, bx2 - bx1) * max(0.0, by2 - by1)
        union = area_a + area_b - inter_area
        return inter_area / union if union else 0.0

    @staticmethod
    def _ts(det: "RawDetectionPayload") -> datetime:
        return getattr(det, "timestamp", None) or utc_now()

    # ------------------------------------------------------------------ #
    def _smooth_bbox(
        self, prev: tuple[float, float, float, float], bbox: tuple[float, float, float, float]
    ) -> tuple[float, float, float, float]:
        px1, py1, px2, py2 = prev
        x1, y1, x2, y2 = bbox
        return (
            self.alpha * x1 + (1 - self.alpha) * px1,
            self.alpha * y1 + (1 - self.alpha) * py1,
            self.alpha * x2 + (1 - self.alpha) * px2,
            self.alpha * y2 + (1 - self.alpha) * py2,
        )

    def _prune_stale(self, now: datetime) -> None:
        to_drop = []
        for track_id, state in self._tracks.items():
            age = (now - state.last_seen).total_seconds()
            max_missed = self.max_missed if state.hits >= self.min_hits else min(2, self.max_missed)
            if age > self.max_age_seconds or state.missed > max_missed:
                to_drop.append(track_id)
        for track_id in to_drop:
            self._tracks.pop(track_id, None)

    def _match_candidates(
        self, detections: List["RawDetectionPayload"]
    ) -> List[tuple[float, int, int]]:
        """
        Возвращает кандидаты (score, track_id, det_idx), отсортированные по убыванию score.
        score: IoU, либо небольшой вес по центру, если IoU низкий, но объекты рядом.
        """
        candidates: list[tuple[float, int, int]] = []
        for track_id, state in self._tracks.items():
            for det_idx, det in enumerate(detections):
                if det.class_id != state.class_id:
                    continue
                bbox = tuple(float(v) for v in det.bbox)
                iou = self._iou(bbox, state.bbox)
                center_sim = self._center_similarity(bbox, state.bbox)
                if iou < self.iou_threshold and center_sim <= 0.0:
                    continue
                score = iou if iou >= self.iou_threshold else 0.001 + 0.2 * center_sim
                candidates.append((score, track_id, det_idx))
        candidates.sort(key=lambda x: x[0], reverse=True)
        return candidates

    # ------------------------------------------------------------------ #
    def assign_and_smooth(
        self, detections: List["RawDetectionPayload"]
    ) -> List[tuple["RawDetectionPayload", tuple[float, float, float, float], int]]:
        if not detections:
            self._prune_stale(utc_now())
            return []

        now = max(self._ts(det) for det in detections)
        self._prune_stale(now)

        assignments: Dict[int, tuple["RawDetectionPayload", tuple[float, float, float, float], int]] = {}
        used_tracks: set[int] = set()
        used_dets: set[int] = set()

        for score, track_id, det_idx in self._match_candidates(detections):
            if track_id in used_tracks or det_idx in used_dets:
                continue
            det = detections[det_idx]
            bbox = tuple(float(v) for v in det.bbox)
            track = self._tracks[track_id]
            smoothed_bbox = self._smooth_bbox(track.bbox, bbox)
            track.bbox = smoothed_bbox
            track.score = float(det.confidence)
            track.hits += 1
            track.missed = 0
            track.last_seen = self._ts(det)
            det.track_id = track_id
            assignments[det_idx] = (det, smoothed_bbox, track_id)
            used_tracks.add(track_id)
            used_dets.add(det_idx)

        for det_idx, det in enumerate(detections):
            if det_idx in used_dets:
                continue
            bbox = tuple(float(v) for v in det.bbox)
            track_id = self._next_track_id
            self._next_track_id += 1
            ts = self._ts(det)
            self._tracks[track_id] = TrackState(
                bbox=bbox,
                class_id=det.class_id,
                score=float(det.confidence),
                hits=1,
                missed=0,
                last_seen=ts,
            )
            det.track_id = track_id
            assignments[det_idx] = (det, bbox, track_id)
            used_tracks.add(track_id)

        for track_id, state in self._tracks.items():
            if track_id not in used_tracks:
                state.missed += 1
        self._prune_stale(now)

        return [assignments[idx] for idx in sorted(assignments.keys())]


class NativeBBoxSmoother:
    """
    Обёртка над native_core.BBoxTracker с теми же параметрами, что у BBoxSmoother.
    Используется при включённом use_native_core и собранном расширении.
    """

    def __init__(
        self,
        alpha: float = 0.5,
        max_center_distance_px: float = 80.0,
        iou_threshold: float = 0.25,
        max_age_seconds: float = 2.0,
        min_hits: int = 2,
        max_missed: int = 10,
    ) -> None:
        if not _NATIVE_TRACKER_AVAILABLE:
            raise RuntimeError("native_core.BBoxTracker is not available")
        self._tracker = _native_core.BBoxTracker(
            alpha,
            max_center_distance_px,
            iou_threshold,
            max_age_seconds,
            min_hits,
            max_missed,
        )

    @staticmethod
    def _ts(det: "RawDetectionPayload") -> float:
        dt = getattr(det, "timestamp", None)
        if dt:
            return dt.timestamp()
        return utc_now().timestamp()

    def assign_and_smooth(
        self, detections: List["RawDetectionPayload"]
    ) -> List[tuple["RawDetectionPayload", tuple[float, float, float, float], int]]:
        if not detections:
            # Прогоняем пустой список для очистки треков по возрасту
            self._tracker.assign_and_smooth([])
            return []

        det_tuples = []
        for det in detections:
            x1, y1, x2, y2 = det.bbox
            det_tuples.append((int(det.class_id), float(det.confidence), float(x1), float(y1), float(x2), float(y2), self._ts(det)))

        assignments = self._tracker.assign_and_smooth(det_tuples)
        out: List[tuple["RawDetectionPayload", tuple[float, float, float, float], int]] = []
        for det_idx, track_id, bbox in assignments:
            det = detections[det_idx]
            det.track_id = track_id
            out.append((det, tuple(float(v) for v in bbox), track_id))
        return out


def build_smoother(settings) -> BBoxSmoother | NativeBBoxSmoother:  # noqa: ANN001
    params = dict(
        alpha=getattr(settings, "bbox_smooth_alpha", 0.25),
        max_center_distance_px=getattr(
            settings,
            "track_max_center_distance_px",
            getattr(settings, "bbox_smooth_max_dist_px", 80.0),
        ),
        iou_threshold=getattr(settings, "track_iou_threshold", 0.25),
        max_age_seconds=getattr(settings, "track_max_age_seconds", 2.0),
        min_hits=getattr(settings, "track_min_hits", 2),
        max_missed=getattr(settings, "track_max_missed", 10),
    )
    if getattr(settings, "use_native_core", False) and _NATIVE_TRACKER_AVAILABLE:
        return NativeBBoxSmoother(**params)
    return BBoxSmoother(**params)


__all__ = ["BBoxSmoother", "TrackState", "NativeBBoxSmoother", "build_smoother"]
