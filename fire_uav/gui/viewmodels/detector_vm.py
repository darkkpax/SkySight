"""
DetectorVM �?" �?�>�?�� ViewModel �?�>�? �?��'���'�?�?�.

�?� �?�?�+�>���?��' �?�?�+�<�'��? APP_START / APP_STOP / CONF_CHANGE �ؐ�?��� EventBus
�?� �?�?��?��?����' �����ؐ�� �?��'���Ő��, ���?�?�+�?���?�<�?����' ��: �? GUI:
    �"? ���?�>�?�<�� batch   ��' �?��?�?���> detection
    �"? �?����?�?�� bbox'�?�? ��' �?��?�?���> bboxes  (�?�>�? VideoPane)
"""

from __future__ import annotations

import logging
from collections import deque
from typing import Any, Deque, Final, Tuple

from PySide6.QtCore import QObject, Signal

from fire_uav.config import settings
from fire_uav.services.bus import Event, bus

_log: Final = logging.getLogger(__name__)

# x1, y1, x2, y2
BBox = Tuple[int, int, int, int]


class DetectorVM(QObject):
    # -------- ���?�+�>��ؐ?�<�� �?��?�?���>�< �?�>�? GUI -------- #
    detection = Signal(object)  # �?��?�? batch
    bboxes = Signal(list)  # List[BBox]

    def __init__(self) -> None:
        super().__init__()
        self._conf = getattr(settings, "yolo_conf", 0.15)
        self._frame_idx = 0
        self._tracks: list[dict[str, Any]] = []
        self._win = getattr(settings, "agg_window", 3)
        self._votes = getattr(settings, "agg_votes_required", 1)
        self._min_conf = getattr(settings, "agg_min_confidence", 0.4)
        self._smooth_alpha = float(getattr(settings, "bbox_smooth_alpha", 0.25) or 0.25)
        self._iou_threshold = float(getattr(settings, "track_iou_threshold", 0.25))
        self._last_stable_conf: float = 0.0
        self._last_stable_count: int = 0
        # mypy ����>�?��'�?�? �?�� �?��?�?�?�����?��?��� �'����� ��?�>�+�?��� �?" ���?�?���?�>�?��?
        bus.subscribe(Event.DETECTION, self._on_detection)  # type: ignore[arg-type]
        _log.info("DetectorVM subscribed to Event.DETECTION")

    def start(self) -> None:
        """�-�����?�?�'��'�? �?��'���'�?�? (���?�?��>�?�ؐ�'�? EventBus, �� �'.��.)."""
        bus.emit(Event.APP_START)
        _log.debug("APP_START emitted")
        bus.emit(Event.DETECTOR_START)
        _log.debug("DETECTOR_START emitted")

    def stop(self) -> None:
        """�?�?�'���?�?�?��'�? �?��'���'�?�?."""
        bus.emit(Event.DETECTOR_STOP)
        _log.debug("DETECTOR_STOP emitted")

    def reset(self) -> None:
        self._tracks.clear()
        self._last_stable_conf = 0.0
        self._last_stable_count = 0
        self.bboxes.emit([])

    def set_conf(self, value: float) -> None:
        """Update detection confidence threshold (broadcast via EventBus)."""
        self._conf = value
        bus.emit(Event.CONF_CHANGE, value)
        _log.debug("CONF_CHANGE emitted -> %.2f", value)

    def _on_detection(self, batch: Any) -> None:
        """�?�?��?�'�> batch ��' ���?�?�+�?���?�<�?����? �?��?�?���>�< �?���?��?�:."""
        self.detection.emit(batch)

        # ����?�>������? bbox'�< ��� batch.detections
        det_pairs: list[tuple[BBox, float]] = []
        for d in getattr(batch, "detections", []):
            bbox = None
            raw_bbox = getattr(d, "bbox", None)
            if isinstance(raw_bbox, (tuple, list)) and len(raw_bbox) == 4:
                if all(v is not None for v in raw_bbox):
                    try:
                        bbox = (int(raw_bbox[0]), int(raw_bbox[1]), int(raw_bbox[2]), int(raw_bbox[3]))
                    except (TypeError, ValueError):
                        bbox = None
            elif all(hasattr(d, k) for k in ("x1", "y1", "x2", "y2")):
                coords = [getattr(d, k) for k in ("x1", "y1", "x2", "y2")]
                if all(v is not None for v in coords):
                    try:
                        bbox = (int(coords[0]), int(coords[1]), int(coords[2]), int(coords[3]))
                    except (TypeError, ValueError):
                        bbox = None
            if bbox is None:
                continue
            conf = float(getattr(d, "confidence", getattr(d, "score", 0.0)))
            det_pairs.append((bbox, conf))

        stable_boxes, stable_conf = self._stable_tracks(det_pairs)
        self._last_stable_conf = stable_conf
        self._last_stable_count = len(stable_boxes)
        # Show raw boxes immediately for operator feedback; K/N stability is used for confidence/confirmation.
        if stable_boxes:
            self.bboxes.emit(stable_boxes)
        else:
            self.bboxes.emit([bbox for bbox, _ in det_pairs])

    @property
    def last_stable_conf(self) -> float:
        return self._last_stable_conf

    @property
    def last_stable_count(self) -> int:
        return self._last_stable_count

    # ---------- bbox smoothing ---------- #
    def _stable_tracks(self, detections: list[tuple[BBox, float]]) -> tuple[list[BBox], float]:
        """K/N голосование по боксам для устойчивого отображения."""
        self._frame_idx += 1
        # decay: дропаем старые треки
        self._tracks = [
            t for t in self._tracks if (self._frame_idx - t["last_seen"]) < self._win
        ]

        used_tracks: set[int] = set()
        for bbox, conf in detections:
            # guard: skip invalid bboxes (None or wrong shape)
            if bbox is None:
                continue
            if len(bbox) != 4:
                continue
            if any(v is None for v in bbox):
                continue
            match_idx = self._match_track(bbox, used_tracks)
            if match_idx is None:
                self._tracks.append(
                    {
                        "bbox": tuple(map(float, bbox)),
                        "conf": float(conf),
                        "history": deque([float(conf)], maxlen=self._win),
                        "last_seen": self._frame_idx,
                    }
                )
                continue

            track = self._tracks[match_idx]
            alpha = max(0.0, min(0.95, self._smooth_alpha))
            px1, py1, px2, py2 = track["bbox"]
            x1, y1, x2, y2 = bbox
            track["bbox"] = (
                alpha * px1 + (1 - alpha) * x1,
                alpha * py1 + (1 - alpha) * y1,
                alpha * px2 + (1 - alpha) * x2,
                alpha * py2 + (1 - alpha) * y2,
            )
            track["conf"] = 0.6 * track["conf"] + 0.4 * conf
            hist: Deque[float] = track["history"]
            hist.append(float(conf))
            track["last_seen"] = self._frame_idx
            used_tracks.add(match_idx)

        stable: list[BBox] = []
        stable_conf = 0.0
        for t in self._tracks:
            if len(t["history"]) >= self._votes:
                avg_conf = sum(t["history"]) / len(t["history"])
                if avg_conf < self._min_conf:
                    continue
                bx = tuple(int(v) for v in t["bbox"])
                stable.append(bx)  # type: ignore[arg-type]
                stable_conf = max(stable_conf, avg_conf)

        return stable, stable_conf

    def _match_track(self, bbox: BBox, used_tracks: set[int]) -> int | None:
        best_idx: int | None = None
        best_iou = 0.0
        for idx, t in enumerate(self._tracks):
            if idx in used_tracks:
                continue
            iou = self._iou(bbox, t["bbox"])
            if iou > best_iou:
                best_iou = iou
                best_idx = idx
        return best_idx if best_iou >= self._iou_threshold else None

    @staticmethod
    def _iou(box_a: BBox, box_b: tuple[float, float, float, float]) -> float:
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
        area_a = max(0.0, (ax2 - ax1)) * max(0.0, (ay2 - ay1))
        area_b = max(0.0, (bx2 - bx1)) * max(0.0, (by2 - by1))
        union = area_a + area_b - inter_area
        return inter_area / union if union else 0.0
