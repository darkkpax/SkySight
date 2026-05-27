from __future__ import annotations

import logging
from typing import Any, List

from fire_uav.config import settings as app_settings
from fire_uav.module_core.factories import get_energy_model
from fire_uav.module_core.interfaces.energy import EnergyInsufficientError, IEnergyModel
from fire_uav.module_core.interfaces.route_planner import IRoutePlanner
from fire_uav.module_core.route.base_location import resolve_base_location
from fire_uav.module_core.route.maneuvers import build_maneuver, build_multi_target_maneuver, build_rejoin, OrbitParams
from fire_uav.module_core.schema import Route, TelemetrySample, Waypoint, WorldCoord
from fire_uav.module_core.route.planner import build_route

log = logging.getLogger(__name__)


class PythonRoutePlanner(IRoutePlanner):
    """Route planner backed by the existing grid/coverage generator."""

    def __init__(self, *, energy_model: IEnergyModel | None = None, settings: Any | None = None) -> None:
        self.energy_model = energy_model or get_energy_model(app_settings)
        self.settings = settings or app_settings
        self.latest_telemetry: TelemetrySample | None = None

    def _resolve_base_location(
        self, route: Route, telemetry: TelemetrySample | None
    ) -> WorldCoord | None:
        return resolve_base_location(self.settings, route, telemetry)

    def plan_route(self, geom_wkt: str, gsd_cm: int | float = 0) -> Route:
        missions = build_route(geom_wkt, int(gsd_cm) if gsd_cm else 0, settings=self.settings)
        wps: List[Waypoint] = [
            Waypoint(lat=lat, lon=lon, alt=alt) for mission in missions for (lat, lon, alt) in mission
        ]
        route = Route(version=1, waypoints=wps, active_index=0 if wps else None)
        telemetry = self.latest_telemetry
        if telemetry is None:
            return route

        base_location = self._resolve_base_location(route, telemetry)
        if base_location is None:
            log.warning("EnergyModel: base location unavailable; allowing route without checks.")
            return route

        try:
            estimate = self.energy_model.estimate_route_feasibility(telemetry, route, base_location)
        except Exception as exc:  # noqa: BLE001
            log.warning("EnergyModel: feasibility estimate failed; allowing route (%s)", exc)
            return route

        if not estimate.can_complete:
            available = telemetry.battery_percent
            available_str = "unknown" if available is None else f"{available:.1f}%"
            msg = (
                "EnergyModel: route is not feasible with current battery "
                f"(required {estimate.required_percent:.1f}%, available {available_str})"
            )
            log.warning(msg)
            raise EnergyInsufficientError(msg)

        return route

    def plan_maneuver(
        self,
        current_state: TelemetrySample,
        target_lat: float,
        target_lon: float,
        base_route: Route,
        *,
        allow_unsafe: bool = False,
    ) -> Route | None:
        return build_maneuver(
            current_state=current_state,
            target_lat=target_lat,
            target_lon=target_lon,
            base_route=base_route,
            energy_model=self.energy_model,
            settings=self.settings,
            allow_unsafe=allow_unsafe,
        )

    def plan_multi_target_maneuver(
        self,
        current_state: TelemetrySample,
        targets: list[tuple[float, float]],
        base_route: Route,
        *,
        allow_unsafe: bool = False,
    ) -> Route | None:
        raw_altitude = getattr(self.settings, "maneuver_alt_m", None)
        try:
            altitude = float(raw_altitude) if raw_altitude is not None else float(current_state.alt)
        except (TypeError, ValueError):
            altitude = float(current_state.alt)
        altitude = max(0.0, altitude)
        try:
            radius = max(1.0, float(getattr(self.settings, "orbit_radius_m", 50.0) or 50.0))
        except (TypeError, ValueError):
            radius = 50.0
        try:
            points_per_circle = max(3, int(getattr(self.settings, "orbit_points_per_circle", 12) or 12))
        except (TypeError, ValueError):
            points_per_circle = 12
        try:
            loops = max(1, int(getattr(self.settings, "orbit_loops", 1) or 1))
        except (TypeError, ValueError):
            loops = 1
        orbit_params = OrbitParams(
            radius_m=radius,
            altitude_m=altitude,
            points_per_circle=points_per_circle,
            loops=loops,
        )
        from fire_uav.module_core.route.base_location import resolve_base_location
        base_location = resolve_base_location(self.settings, base_route, current_state)
        if base_location is None:
            from fire_uav.module_core.schema import WorldCoord
            base_location = WorldCoord(lat=float(current_state.lat), lon=float(current_state.lon))
        return build_multi_target_maneuver(
            current_state=current_state,
            targets=targets,
            base_route=base_route,
            base_location=base_location,
            energy_model=self.energy_model,
            orbit_params=orbit_params,
            allow_unsafe=allow_unsafe,
        )

    def plan_rejoin(self, exit_wp: Waypoint, base_route: Route) -> list[Waypoint]:
        return build_rejoin(exit_wp, base_route)


__all__ = ["PythonRoutePlanner"]
