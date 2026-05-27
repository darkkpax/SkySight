from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Optional

from fire_uav.module_core.schema import TelemetrySample


class IGeoProjector(ABC):
    """Abstract interface for projecting image-space detections to geographic coordinates."""

    @abstractmethod
    def compute_distance(self, lat1: float, lon1: float, lat2: float, lon2: float) -> float:
        """Return distance in meters between two WGS84 points."""

    @abstractmethod
    def project_bbox_to_ground(
        self,
        telemetry: TelemetrySample,
        bbox: tuple[float, float, float, float],
        image_width: int,
        image_height: int,
    ) -> Optional[tuple[float, float]]:
        """Project a bounding-box center from image space to ground lat/lon or return None."""

