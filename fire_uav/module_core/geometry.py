"""
Чистые геометрические функции без внешних зависимостей —
можно переиспользовать из flight-планировщика и из детектора.
"""

from __future__ import annotations

import math
from typing import Tuple

EARTH_RADIUS_M: float = 6_378_137.0  # WGS-84 semi-major ось, метры


def haversine_m(p1: Tuple[float, float], p2: Tuple[float, float]) -> float:
    """Great-circle distance **в метрах** между двумя (lat, lon) точками."""
    lat1, lon1 = p1
    lat2, lon2 = p2
    phi1, phi2 = map(math.radians, (lat1, lat2))
    dphi = phi2 - phi1
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlambda / 2) ** 2
    return 2 * EARTH_RADIUS_M * math.asin(math.sqrt(a))


def offset_latlon(lat: float, lon: float, dx_m: float, dy_m: float) -> Tuple[float, float]:
    d_lat = dy_m / EARTH_RADIUS_M
    cos_lat = math.cos(math.radians(lat))
    # Guard against near-pole singularity (|lat| > ~89.9°) where cos → 0.
    cos_lat = math.copysign(max(abs(cos_lat), 1e-9), cos_lat) if cos_lat != 0 else 1e-9
    d_lon = dx_m / (EARTH_RADIUS_M * cos_lat)
    return lat + math.degrees(d_lat), lon + math.degrees(d_lon)


def interpolate_path_point(
    path: list[Tuple[float, float]], progress: float
) -> Tuple[float, float] | None:
    """
    Return a point along the path for a given progress in [0, 1].
    Linear interpolation by segment length; returns the last point if progress >= 1.
    """
    if not path:
        return None
    if len(path) == 1:
        return path[0]

    progress = max(0.0, min(1.0, float(progress)))
    segments: list[float] = []
    total = 0.0
    for a, b in zip(path[:-1], path[1:]):
        dist = haversine_m(a, b)
        segments.append(dist)
        total += dist

    if total == 0:
        return path[-1]

    target = total * progress
    acc = 0.0
    for (lat1, lon1), (lat2, lon2), seg_len in zip(path[:-1], path[1:], segments):
        if acc + seg_len >= target:
            if seg_len == 0:
                return lat2, lon2
            ratio = (target - acc) / seg_len
            lat = lat1 + (lat2 - lat1) * ratio
            lon = lon1 + (lon2 - lon1) * ratio
            return lat, lon
        acc += seg_len

    return path[-1]
