"""
Lawn-mower grid generator + TSP-оптимизатор + деление на миссии.
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass
from typing import List, Tuple

from shapely import wkt
from shapely.affinity import rotate
from shapely.geometry import GeometryCollection, LineString, MultiLineString, Point, Polygon

from fire_uav.cpp import follow_path
from fire_uav.module_core.geometry import haversine_m
from fire_uav.module_core.route.energy import EnergyModel
from fire_uav.module_core.route.no_fly import load_no_fly
from fire_uav.module_core.schema import Waypoint as WaypointModel
from fire_uav.config import settings as app_settings

DEFAULT_SETTINGS = app_settings

try:
    from ortools.constraint_solver import pywrapcp, routing_enums_pb2
    ORTOOLS_AVAILABLE = True
except ImportError:
    ORTOOLS_AVAILABLE = False

_log = logging.getLogger(__name__)


# ─────────────────────── dataclasses ──────────────────────── #


@dataclass(slots=True)
class CameraSpec:
    sensor_width_mm: float = 6.3
    resolution_px: int = 8064
    focal_length_mm: float = 5.7
    fov_deg: float = 82.1

    def __post_init__(self) -> None:
        if self.sensor_width_mm <= 0:
            raise ValueError(f"sensor_width_mm must be positive, got {self.sensor_width_mm}")
        if self.focal_length_mm <= 0:
            raise ValueError(f"focal_length_mm must be positive, got {self.focal_length_mm}")
        if self.resolution_px <= 0:
            raise ValueError(f"resolution_px must be positive, got {self.resolution_px}")

    def swath_m(self, alt_m: float) -> float:
        return 2 * alt_m * math.tan(math.radians(self.fov_deg / 2))


@dataclass(slots=True)
class GridParams:
    side_overlap: float = 0.7
    front_overlap: float = 0.8
    gsd_target_cm: float = 2.5
    orientation_deg: float = 0


Waypoint = WaypointModel


# ───────────────────────── FlightPlanner ───────────────────── #


class FlightPlanner:
    def __init__(
        self,
        aoi: Polygon,
        cam: CameraSpec | None = None,
        grid: GridParams | None = None,
        energy: EnergyModel | None = None,
        settings=DEFAULT_SETTINGS,
    ):
        self._settings = settings or DEFAULT_SETTINGS
        # вычитаем запретные зоны
        nfz = load_no_fly(getattr(self._settings, "no_fly_geojson", ""))
        if nfz:
            aoi = aoi.difference(nfz)

        self.aoi = aoi
        self.cam = cam or CameraSpec()
        self.grid = grid or GridParams()
        self.energy = energy or EnergyModel()

        self.altitude_m = self._altitude_for_gsd(self.grid.gsd_target_cm)
        self.line_spacing_m = self._line_spacing()
        self.forward_spacing_m = self._forward_spacing()

    # ───── helpers ───── #
    def _altitude_for_gsd(self, gsd_cm: float) -> float:
        """Высота (м) для заданного GSD (см/пикс)."""
        return (
            gsd_cm
            * self.cam.focal_length_mm
            * self.cam.resolution_px
            / (100 * self.cam.sensor_width_mm)
        )

    def _line_spacing(self) -> float:
        return self.cam.swath_m(self.altitude_m) * (1 - self.grid.side_overlap)

    def _forward_spacing(self) -> float:
        return self.cam.swath_m(self.altitude_m) * (1 - self.grid.front_overlap)

    # ───── grid ───── #
    def build_grid(self) -> List[LineString]:
        """
        Генерирует гребёнчатую сетку.
        Шаг задаётся в метрах, но пересчитывается в градусы.
        """
        if self.line_spacing_m <= 0:
            raise ValueError(
                f"line_spacing_m must be positive (got {self.line_spacing_m}). "
                "Check that side_overlap < 1.0 and gsd_target_cm > 0."
            )
        rot = rotate(
            self.aoi,
            -self.grid.orientation_deg,
            origin="centroid",
            use_radians=False,
        )
        minx, miny, maxx, maxy = rot.bounds

        step_deg = self.line_spacing_m / 111_000  # метр → градус
        lines: list[LineString] = []
        y = miny
        while y <= maxy:
            seg = LineString([(minx, y), (maxx, y)])
            inter = seg.intersection(rot)
            if not inter.is_empty:
                if isinstance(inter, LineString):
                    lines.append(inter)
                elif isinstance(inter, MultiLineString):
                    lines.extend(inter.geoms)
                elif isinstance(inter, GeometryCollection):
                    lines.extend(g for g in inter.geoms if isinstance(g, LineString))
            y += step_deg

        # возвращаем исходный угол
        lines = [
            rotate(line, self.grid.orientation_deg, origin="centroid", use_radians=False)
            for line in lines
        ]
        # зиг-заг
        for i in range(1, len(lines), 2):
            lines[i] = LineString(list(lines[i].coords)[::-1])
        return lines

    # ───── lines → waypoints ───── #
    def lines_to_waypoints(self, lines: List[LineString]) -> List[Waypoint]:
        """
        Превращает линии в точки съёмки.
        Шаг снова переводится в градусы, чтобы «кадров» стало адекватно.
        """
        if self.forward_spacing_m <= 0:
            raise ValueError(
                f"forward_spacing_m must be positive (got {self.forward_spacing_m}). "
                "Check that front_overlap < 1.0 and gsd_target_cm > 0."
            )
        step_deg = self.forward_spacing_m / 111_000  # ≈ град/шаг
        wps: list[Waypoint] = []
        for ln in lines:
            length_deg = ln.length  # длина в °
            n = max(2, math.ceil(length_deg / step_deg))
            for i in range(n):
                pt: Point = ln.interpolate(i / (n - 1), normalized=True)
                wps.append(Waypoint(lat=pt.y, lon=pt.x, alt=self.altitude_m))
        return wps

    # ───── TSP ───── #
    def optimise(self, wps: List[Waypoint]) -> List[Waypoint]:
        """
        Упорядочивает точки по кратчайшему маршруту (TSP через OR-Tools).
        """
        n = len(wps)
        if n <= 3:
            return wps

        use_ortools_cfg = bool(getattr(self._settings, "use_ortools", True))
        if not ORTOOLS_AVAILABLE or not use_ortools_cfg:
            if not ORTOOLS_AVAILABLE:
                _log.warning("ortools not available, using simple sequential route (no TSP optimization).")
            else:
                _log.info("use_ortools is False; skipping TSP optimization and keeping sequential route.")
            return wps

        # матрица расстояний
        dist = [[0] * n for _ in range(n)]
        for i in range(n):
            for j in range(i + 1, n):
                d = int(haversine_m((wps[i].lat, wps[i].lon), (wps[j].lat, wps[j].lon)))
                dist[i][j] = dist[j][i] = d

        mgr = pywrapcp.RoutingIndexManager(n, 1, 0)
        rt = pywrapcp.RoutingModel(mgr)

        cb = rt.RegisterTransitCallback(lambda a, b: dist[mgr.IndexToNode(a)][mgr.IndexToNode(b)])
        rt.SetArcCostEvaluatorOfAllVehicles(cb)

        p = pywrapcp.DefaultRoutingSearchParameters()
        p.time_limit.seconds = 5
        p.first_solution_strategy = routing_enums_pb2.FirstSolutionStrategy.PATH_CHEAPEST_ARC

        sol = rt.SolveWithParameters(p)
        if not sol:
            return wps

        route: list[Waypoint] = []
        idx = rt.Start(0)
        while not rt.IsEnd(idx):
            route.append(wps[mgr.IndexToNode(idx)])
            idx = sol.Value(rt.NextVar(idx))
        return route

    # ───── разделение по батареям ───── #
    def split_missions(self, ordered: List[Waypoint]) -> List[List[Waypoint]]:
        """
        Разбивает длинный маршрут на миссии по ёмкости аккумулятора.
        """
        missions: list[list[Waypoint]] = []
        cur: list[Waypoint] = []
        dist: float = 0.0
        prev: Waypoint | None = None

        for wp in ordered:
            if prev:
                dist += haversine_m((prev.lat, prev.lon), (wp.lat, wp.lon))
            cur.append(wp)
            prev = wp

            if self.energy.energy_used_wh(dist) >= self.energy.battery_wh * 0.9:
                missions.append(cur)
                cur, dist, prev = [], 0.0, None

        if cur:
            missions.append(cur)
        return missions

    # ───── pipeline ───── #
    def generate(self) -> List[List[Waypoint]]:
        grid = self.build_grid()
        wps = self.lines_to_waypoints(grid)
        ordered = self.optimise(wps)
        return self.split_missions(ordered)


# ───────── API-обёртка для GUI ───────── #


WaypointT = Tuple[float, float, float]  # lat, lon, alt


def build_route(geom_wkt: str, gsd_cm: int = 0, settings=DEFAULT_SETTINGS) -> List[List[WaypointT]]:
    """
    Принимает WKT-строку:

      • LineString → возвращает точный путь (`follow_path`, alt = 120 м)
      • Polygon    → генерирует lawn-mower сетку через `FlightPlanner`

    GUI вызывает именно эту функцию.
    """
    geom = wkt.loads(geom_wkt)

    if geom.geom_type == "LineString":
        path_latlon = [(lat, lon) for lon, lat in geom.coords]
        try:
            wps = follow_path(path_latlon, altitude_m=120.0)
            return [[(wp.lat, wp.lon, wp.alt) for wp in wps]]
        except Exception as exc:  # noqa: BLE001
            _log.warning("C++ follow_path unavailable, using simple passthrough: %s", exc)
            return [[(lat, lon, 120.0) for lat, lon in path_latlon]]

    if geom.geom_type == "Polygon":
        effective_gsd = gsd_cm or int(getattr(settings, "gsd_cm", 0) or 0)
        grid = GridParams(
            gsd_target_cm=effective_gsd or GridParams().gsd_target_cm,
            side_overlap=float(getattr(settings, "side_overlap", GridParams().side_overlap)),
            front_overlap=float(getattr(settings, "front_overlap", GridParams().front_overlap)),
        )
        fp = FlightPlanner(geom, grid=grid, settings=settings)
        missions = fp.generate()  # list[list[Waypoint]]
        return [[(wp.lat, wp.lon, wp.alt) for wp in ms] for ms in missions]

    raise ValueError("build_route: ожидался LineString или Polygon")
