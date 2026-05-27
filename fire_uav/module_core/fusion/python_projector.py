from __future__ import annotations

import logging
import math
from typing import Optional, Tuple

from fire_uav.domain.video.camera import CameraParams
from fire_uav.module_core.geometry import haversine_m, offset_latlon
from fire_uav.module_core.interfaces.geo import IGeoProjector
from fire_uav.module_core.schema import TelemetrySample, WorldCoord

log = logging.getLogger(__name__)


def _rot_x(angle_rad: float) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    c = math.cos(angle_rad)
    s = math.sin(angle_rad)
    return (
        (1.0, 0.0, 0.0),
        (0.0, c, -s),
        (0.0, s, c),
    )


def _rot_y(angle_rad: float) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    c = math.cos(angle_rad)
    s = math.sin(angle_rad)
    return (
        (c, 0.0, s),
        (0.0, 1.0, 0.0),
        (-s, 0.0, c),
    )


def _rot_z(angle_rad: float) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    c = math.cos(angle_rad)
    s = math.sin(angle_rad)
    return (
        (c, -s, 0.0),
        (s, c, 0.0),
        (0.0, 0.0, 1.0),
    )


def _mat_mul(
    a: tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]],
    b: tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]],
) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    return (
        (
            a[0][0] * b[0][0] + a[0][1] * b[1][0] + a[0][2] * b[2][0],
            a[0][0] * b[0][1] + a[0][1] * b[1][1] + a[0][2] * b[2][1],
            a[0][0] * b[0][2] + a[0][1] * b[1][2] + a[0][2] * b[2][2],
        ),
        (
            a[1][0] * b[0][0] + a[1][1] * b[1][0] + a[1][2] * b[2][0],
            a[1][0] * b[0][1] + a[1][1] * b[1][1] + a[1][2] * b[2][1],
            a[1][0] * b[0][2] + a[1][1] * b[1][2] + a[1][2] * b[2][2],
        ),
        (
            a[2][0] * b[0][0] + a[2][1] * b[1][0] + a[2][2] * b[2][0],
            a[2][0] * b[0][1] + a[2][1] * b[1][1] + a[2][2] * b[2][1],
            a[2][0] * b[0][2] + a[2][1] * b[1][2] + a[2][2] * b[2][2],
        ),
    )


def _mat_vec_mul(
    mat: tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]],
    vec: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (
        mat[0][0] * vec[0] + mat[0][1] * vec[1] + mat[0][2] * vec[2],
        mat[1][0] * vec[0] + mat[1][1] * vec[1] + mat[1][2] * vec[2],
        mat[2][0] * vec[0] + mat[2][1] * vec[1] + mat[2][2] * vec[2],
    )


def _normalize(vec: tuple[float, float, float]) -> tuple[float, float, float]:
    norm = math.sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2])
    if norm <= 1e-12:
        return 1.0, 0.0, 0.0
    inv = 1.0 / norm
    return vec[0] * inv, vec[1] * inv, vec[2] * inv


def _rot_zyx(yaw_deg: float, pitch_deg: float, roll_deg: float) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    return _mat_mul(_rot_z(math.radians(yaw_deg)), _mat_mul(_rot_y(math.radians(pitch_deg)), _rot_x(math.radians(roll_deg))))


class PythonGeoProjector(IGeoProjector):
    """Pure-Python geo projector using camera ray to ground-plane intersection."""

    def __init__(self, camera: CameraParams | None = None) -> None:
        self._camera = camera or CameraParams()

    def set_camera_params(self, camera: CameraParams) -> None:
        self._camera = camera

    @staticmethod
    def _resolve_mount_angles(
        telemetry: TelemetrySample,
        camera: CameraParams,
    ) -> tuple[float, float, float]:
        pitch = getattr(telemetry, "camera_mount_pitch_deg", None)
        yaw = getattr(telemetry, "camera_mount_yaw_deg", None)
        roll = getattr(telemetry, "camera_mount_roll_deg", None)
        return (
            float(camera.mount_yaw_deg if yaw is None else yaw),
            float(camera.mount_pitch_deg if pitch is None else pitch),
            float(camera.mount_roll_deg if roll is None else roll),
        )

    def compute_distance(self, lat1: float, lon1: float, lat2: float, lon2: float) -> float:
        return haversine_m((lat1, lon1), (lat2, lon2))

    def project_bbox_to_ground(
        self,
        telemetry: TelemetrySample,
        bbox: tuple[float, float, float, float],
        image_width: int,
        image_height: int,
    ) -> Optional[tuple[float, float]]:
        x1, y1, x2, y2 = bbox
        u = (x1 + x2) / 2.0
        v = (y1 + y2) / 2.0

        fx, fy = self._camera.focal_lengths_px(image_width, image_height)
        cx, cy = self._camera.principal_point_px(image_width, image_height)

        # Unreal camera convention: +X forward, +Y right, +Z up.
        ray_cam = _normalize((1.0, (u - cx) / max(fx, 1e-9), -(v - cy) / max(fy, 1e-9)))

        mount_yaw_deg, mount_pitch_deg, mount_roll_deg = self._resolve_mount_angles(
            telemetry,
            self._camera,
        )
        mount_rot = _rot_zyx(mount_yaw_deg, mount_pitch_deg, mount_roll_deg)
        uav_rot = _rot_zyx(telemetry.yaw, telemetry.pitch, telemetry.roll)

        ray_mount = _mat_vec_mul(mount_rot, ray_cam)
        ray_enu = _normalize(_mat_vec_mul(uav_rot, ray_mount))

        alt_source = getattr(telemetry, "alt_agl", None)
        if alt_source is None:
            alt_source = telemetry.alt
        alt = max(float(alt_source), 0.0)
        ray_z = ray_enu[2]
        # Reject rays pointing upward or shallower than ~3° below horizontal.
        # A threshold of -0.05 corresponds to sin(~3°); rays closer to horizontal
        # produce ground-intersection distances of alt/0.05 = 20×alt metres at
        # minimum, yielding wildly inaccurate geo-projections.
        _MIN_RAY_Z = -0.05
        if ray_z >= _MIN_RAY_Z:
            log.debug(
                "Projection rejected: ray does not intersect ground (ray_z=%.6f, mount_pitch=%.2f, uav_ypr=(%.2f, %.2f, %.2f))",
                ray_z,
                mount_pitch_deg,
                telemetry.yaw,
                telemetry.pitch,
                telemetry.roll,
            )
            return None

        # Ray origin is (0, 0, alt) in ENU local frame, ground is z=0.
        t = -alt / ray_z
        if t <= 0.0:
            log.debug(
                "Projection rejected: invalid intersection distance t=%.6f (ray_z=%.6f alt=%.2f)",
                t,
                ray_z,
                alt,
            )
            return None

        dx_east = ray_enu[0] * t
        dy_north = ray_enu[1] * t
        return offset_latlon(telemetry.lat, telemetry.lon, dx_east, dy_north)

    # Legacy helper used by existing call sites
    def project(
        self, bbox: Tuple[int, int, int, int], frame_size: Tuple[int, int], telemetry: TelemetrySample
    ) -> WorldCoord:
        projected = self.project_bbox_to_ground(telemetry, bbox, frame_size[0], frame_size[1])
        if projected is None:
            return WorldCoord(lat=float("nan"), lon=float("nan"))
        lat, lon = projected
        return WorldCoord(lat=lat, lon=lon)


# Backwards-compatible alias
GeoProjector = PythonGeoProjector

__all__ = ["PythonGeoProjector", "GeoProjector"]
