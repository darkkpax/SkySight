# mypy: ignore-errors
from __future__ import annotations

import json
from pathlib import Path
from typing import Tuple

import fire_uav.infrastructure.providers as deps
from fire_uav.module_core.geometry import interpolate_path_point
from fire_uav.module_core.route.python_planner import PythonRoutePlanner
from fire_uav.module_core.schema import Route, TelemetrySample, Waypoint
from fire_uav.utils.time import utc_now


class PlannerVM:
    """
    MVVM-слой: хранит/отдаёт маршрут, импортирует GeoJSON LineString
    и экспортирует QGroundControl-совместимый mission.plan.
    """

    def __init__(self) -> None:
        self._route_planner: PythonRoutePlanner = PythonRoutePlanner()
        self._path_backup: list[Tuple[float, float]] | None = None
        setattr(deps, "active_path_kind", "mission")

    # ------------------------------------------------------------------ #
    #                     базовые set / get
    # ------------------------------------------------------------------ #
    def save_plan(self, pts: list[Tuple[float, float]], *, path_kind: str = "mission") -> None:
        """Persist path (lat, lon) into shared deps storage."""
        deps.plan_data = {"path": pts, "path_kind": path_kind}
        setattr(deps, "active_path_kind", path_kind)
        self.persist_plan()

    def clear_plan(self) -> None:
        """Очистить маршрут и удалить сохранённый артефакт."""
        deps.plan_data = None
        setattr(deps, "active_path_kind", "mission")
        self._path_backup = None
        self._delete_persisted_plan()

    def get_path(self) -> list[Tuple[float, float]]:
        """Вернуть текущий путь или [] если его ещё нет."""
        return (deps.plan_data or {}).get("path", [])

    def get_path_kind(self) -> str:
        kind = (deps.plan_data or {}).get("path_kind")
        return kind if kind in ("mission", "maneuver") else "mission"

    def get_active_path(self) -> list[Tuple[float, float]]:
        """
        Вернуть текущий активный путь:
        - если есть построенный облет (debug_orbit_path) — показываем только его,
        - иначе обычный маршрут.
        """
        if deps.rtl_path:
            setattr(deps, "active_path_kind", "mission")
            return deps.rtl_path
        if deps.debug_target and deps.debug_orbit_path:
            setattr(deps, "active_path_kind", "maneuver")
            return deps.debug_orbit_path
        setattr(deps, "active_path_kind", self.get_path_kind())
        return self.get_path()

    # ------------------------------------------------------------------ #
    #                       generate (по кнопке)
    # ------------------------------------------------------------------ #
    def generate_path(self) -> Path:
        """
        Покликал — двойной клик завершил линию — жмём <Generate Path>.
        Здесь просто убеждаемся, что polyline действительно есть и складываем его на диск.
        """
        path = self.get_path()
        if not path:
            raise RuntimeError("Draw polyline first")
        return self.persist_plan()

    # ------------------------------------------------------------------ #
    #                    debug target / orbit preview
    # ------------------------------------------------------------------ #
    def set_debug_target(self, lat: float, lon: float) -> None:
        """
        Save a debug target (e.g. potential fire/human) for mid-flight maneuvers.
        """
        deps.debug_target = {"lat": lat, "lon": lon}

    def clear_debug_target(self) -> None:
        deps.debug_target = None
        deps.debug_orbit_path = None
        if self._path_backup:
            self.save_plan(self._path_backup)
        self._path_backup = None

    def compute_orbit_preview(self) -> None:
        """
        Computes an orbit maneuver around the current debug_target using the last planned route.
        """
        if deps.debug_target is None:
            return

        base_path = self.get_path()
        if not base_path:
            return
        if self._path_backup is None:
            self._path_backup = list(base_path)
        path = list(base_path)

        default_alt = 120.0
        current = self._current_position_on_path(path)
        if current is None:
            return
        cur_lat, cur_lon = current
        base_route = Route(
            version=1,
            waypoints=[Waypoint(lat=lat, lon=lon, alt=default_alt) for lat, lon in path],
            active_index=self._nearest_index(path, current),
        )

        sim_sample = TelemetrySample(
            lat=cur_lat,
            lon=cur_lon,
            alt=default_alt,
            yaw=0.0,
            battery=1.0,
            battery_percent=100.0,
            timestamp=utc_now(),
        )

        maneuver = self._route_planner.plan_maneuver(
            current_state=sim_sample,
            target_lat=deps.debug_target["lat"],
            target_lon=deps.debug_target["lon"],
            base_route=base_route,
        )
        if maneuver is None:
            deps.debug_orbit_path = None
            return

        deps.debug_orbit_path = [(wp.lat, wp.lon) for wp in maneuver.waypoints]
        if deps.debug_orbit_path:
            self.save_plan(deps.debug_orbit_path, path_kind="maneuver")
        self._persist_orbit_preview(deps.debug_orbit_path)

    def rebuild_route_from_current_geom(self, geom_wkt: str | None = None) -> None:
        """
        Placeholder for future route regeneration based on geometry.
        """
        if self.get_path():
            self.persist_plan()

    def _persist_orbit_preview(self, pts: list[tuple[float, float]] | None) -> None:
        """Persist orbit preview to artifacts for quick inspection while debugging."""
        try:
            root_dir = Path(__file__).resolve().parents[3]
            artifacts = root_dir / "data" / "artifacts"
            artifacts.mkdir(exist_ok=True)
            fn = artifacts / "plan_orbit.json"
            payload = {"orbit": pts or []}
            fn.write_text(json.dumps(payload, indent=2))
        except Exception:
            pass

    def _current_position_on_path(self, path: list[tuple[float, float]]) -> tuple[float, float] | None:
        """Return current UAV position on path based on debug progress (0..1)."""
        try:
            return interpolate_path_point(path, getattr(deps, "debug_flight_progress", 0.0))  # type: ignore[arg-type]
        except Exception:
            return None

    def _nearest_index(
        self, path: list[tuple[float, float]], point: tuple[float, float]
    ) -> int | None:
        if not path:
            return None
        best_idx = 0
        best_dist = float("inf")
        lat0, lon0 = point
        for idx, (lat, lon) in enumerate(path):
            dist = (lat - lat0) ** 2 + (lon - lon0) ** 2
            if dist < best_dist:
                best_dist = dist
                best_idx = idx
        return best_idx

    # ------------------------------------------------------------------ #
    #                        IMPORT / EXPORT
    # ------------------------------------------------------------------ #
    def import_geojson(self, fn: Path) -> None:
        """Импортировать LineString (или Feature-обёртку) из GeoJSON."""
        gj = json.loads(fn.read_text())
        if gj.get("type") == "Feature":
            gj = gj["geometry"]

        if gj["type"] != "LineString":
            raise RuntimeError("Only LineString supported")

        # Leaflet даёт (lon, lat) - разворачиваем в (lat, lon)
        pts = [(lat, lon) for lon, lat in gj["coordinates"]]
        self.save_plan(pts)

    def export_json(self, fn: Path) -> None:
        """Сохранить <сырой> JSON вида {"path": [[lat, lon], :]}."""
        Path(fn).write_text(json.dumps(deps.plan_data, indent=2))

    def persist_plan(self) -> Path:
        """Persist the last path to artifacts for reuse across sessions."""
        artifacts = Path(__file__).resolve().parents[3] / "data" / "artifacts"
        artifacts.mkdir(exist_ok=True)
        fn = artifacts / "plan.json"
        fn.write_text(json.dumps(deps.plan_data or {}, indent=2))
        return fn

    def _delete_persisted_plan(self) -> None:
        fn = Path(__file__).resolve().parents[3] / "data" / "artifacts" / "plan.json"
        try:
            if fn.exists():
                fn.unlink()
        except Exception:
            pass

    # ------------------------------------------------------------------ #
    #                QGroundControl mission.plan в artifacts/
    # ------------------------------------------------------------------ #
    def export_qgc_plan(self, alt_m: float = 120.0) -> Path:
        """
        Сформировать QGC-совместимый файл mission.plan и положить его
        в <root>/artifacts/mission.plan. Возвращает путь к файлу.
        """
        path = self.get_path()
        if not path:
            raise RuntimeError("Draw polyline first")

        items = []
        for idx, (lat, lon) in enumerate(path):
            items.append(
                {
                    "AMSLAltAboveTerrain": None,
                    "Altitude": alt_m,
                    "AltitudeMode": 1,
                    "Command": 16,  # MAV_CMD_NAV_WAYPOINT
                    "DoJumpId": idx,
                    "Frame": 3,  # MAV_FRAME_GLOBAL
                    "Params": [0, 0, 0, 0, lat, lon, alt_m],
                    "Type": "SimpleItem",
                }
            )

        qgc = {
            "fileType": "Plan",
            "version": 2,
            "geoFence": {},
            "rallyPoints": {},
            "mission": {"items": items},
        }

        # <root>/artifacts/
        root_dir = Path(__file__).resolve().parents[3]
        artifacts = root_dir / "data" / "artifacts"
        artifacts.mkdir(exist_ok=True)
        fn = artifacts / "mission.plan"
        fn.write_text(json.dumps(qgc, indent=2))
        return fn
