from __future__ import annotations

import sys
import time
from types import SimpleNamespace
from types import ModuleType

_debug_sim_stub = ModuleType("fire_uav.services.debug_sim")
_debug_sim_stub.DebugSimulationService = object
sys.modules.setdefault("fire_uav.services.debug_sim", _debug_sim_stub)

import fire_uav.infrastructure.providers as deps  # noqa: E402
from fire_uav.gui.windows.main_window import AppController, OrbitFlowState  # noqa: E402
from fire_uav.module_core.energy.python_energy_model import PythonEnergyModel  # noqa: E402
from fire_uav.module_core.route.maneuvers import build_rejoin  # noqa: E402
from fire_uav.module_core.schema import Route, TelemetrySample, Waypoint  # noqa: E402
from fire_uav.services.mission.state import MissionState  # noqa: E402
from fire_uav.services.objects_store import ConfirmedObject  # noqa: E402
from fire_uav.utils.time import utc_now  # noqa: E402


class _Signal:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def emit(self, *args) -> None:
        self.calls.append(args)


class _Planner:
    def __init__(self, route: Route | None, route_map: dict[tuple[float, float], Route | None] | None = None) -> None:
        self._route = route
        self._route_map = route_map or {}
        self.calls: list[tuple[float, float]] = []

    def plan_maneuver(
        self,
        *,
        current_state: TelemetrySample,
        target_lat: float,
        target_lon: float,
        base_route: Route,
        allow_unsafe: bool = False,
    ) -> Route | None:
        self.calls.append((target_lat, target_lon))
        key = (round(target_lat, 6), round(target_lon, 6))
        if key in self._route_map:
            return self._route_map[key]
        return self._route

    def plan_rejoin(self, exit_wp: Waypoint, base_route: Route) -> list[Waypoint]:
        return build_rejoin(exit_wp, base_route)


class _UnrealLink:
    def __init__(self, *, send_route_ok: bool = True, route_results: list[bool] | None = None) -> None:
        self.send_route_ok = send_route_ok
        self.route_results = list(route_results or [])
        self.commands: list[tuple[str, dict | None]] = []
        self.routes: list[dict] = []

    def send_command(self, command: str, payload: dict | None = None) -> bool:
        self.commands.append((command, payload))
        return True

    def send_route(self, route: dict) -> bool:
        self.routes.append(route)
        if self.route_results:
            return self.route_results.pop(0)
        return self.send_route_ok


class _MapBridge:
    def __init__(self) -> None:
        self.render_count = 0

    def render_map(self) -> None:
        self.render_count += 1


class _ActivePath:
    def __init__(self) -> None:
        self.mode = "NORMAL"
        self.orbit_paths: list[list[tuple[float, float]]] = []
        self.rtl_paths: list[list[tuple[float, float]]] = []
        self.normal_count = 0
        self._current_path: list[tuple[float, float]] = [(47.6060, -122.3350), (47.6070, -122.3340)]

    def set_orbit(self, pts: list[tuple[float, float]] | None) -> None:
        self.mode = "ORBIT" if pts else "NORMAL"
        self.orbit_paths.append(list(pts or []))
        if pts:
            self._current_path = list(pts)

    def set_normal(self) -> None:
        self.mode = "NORMAL"
        self.normal_count += 1

    def set_rtl(self, pts: list[tuple[float, float]] | None) -> None:
        self.mode = "RTL" if pts else "NORMAL"
        self.rtl_paths.append(list(pts or []))
        if pts:
            self._current_path = list(pts)

    def get_active_path(self) -> list[tuple[float, float]]:
        return list(self._current_path)


class _TargetTracker:
    def __init__(self) -> None:
        self.in_orbit: list[int] = []
        self.orbited: list[int] = []
        self.zones: list[tuple[float, float]] = []

    def mark_in_orbit(self, track_id: int) -> bool:
        self.in_orbit.append(track_id)
        return True

    def mark_orbited(self, track_id: int) -> bool:
        self.orbited.append(track_id)
        return True

    def add_suppression_zone(self, *, lat: float, lon: float) -> None:
        self.zones.append((lat, lon))


class _ObjectsStore:
    def __init__(self, target: ConfirmedObject, extra: list[ConfirmedObject] | None = None) -> None:
        self._target = target
        self._objects = {target.object_id: target}
        for item in extra or []:
            self._objects[item.object_id] = item
        self.selected_id: str | None = None

    def get(self, object_id: str) -> ConfirmedObject | None:
        return self._objects.get(object_id)

    def latest(self) -> ConfirmedObject:
        return self._target

    def set_selected(self, object_id: str | None) -> None:
        self.selected_id = object_id


def _telemetry() -> TelemetrySample:
    return TelemetrySample(
        lat=47.6060,
        lon=-122.3350,
        alt=120.0,
        alt_agl=20.0,
        yaw=90.0,
        pitch=0.0,
        roll=0.0,
        battery=0.97,
        battery_percent=97.0,
        timestamp=utc_now(),
    )


def _confirmed_target() -> ConfirmedObject:
    return ConfirmedObject(
        object_id="track-1",
        class_id=1,
        confidence=0.9,
        lat=47.6065,
        lon=-122.3345,
        track_id=11,
        timestamp=utc_now(),
    )


def _confirmed_target2() -> ConfirmedObject:
    return ConfirmedObject(
        object_id="track-2",
        class_id=1,
        confidence=0.91,
        lat=47.6068,
        lon=-122.3342,
        track_id=12,
        timestamp=utc_now(),
    )


def _orbit_route() -> Route:
    return Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=47.6061, lon=-122.3349, alt=120.0),
            Waypoint(lat=47.6065, lon=-122.3341, alt=120.0),
            Waypoint(lat=47.6070, lon=-122.3340, alt=120.0),
        ],
    )


def _orbit_route_with_rejoin() -> Route:
    return Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=47.6061, lon=-122.3349, alt=120.0),
            Waypoint(lat=47.6065, lon=-122.3341, alt=120.0),
            Waypoint(lat=47.6070, lon=-122.3340, alt=120.0),
            Waypoint(lat=47.6070, lon=-122.3340, alt=120.0),
        ],
    )


def _orbit_route_for_target2() -> Route:
    return Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=47.6068, lon=-122.3342, alt=120.0),
            Waypoint(lat=47.6072, lon=-122.3336, alt=120.0),
            Waypoint(lat=47.6076, lon=-122.3331, alt=120.0),
        ],
    )


def _build_controller(
    *,
    planner_route: Route | None,
    send_route_ok: bool = True,
    route_results: list[bool] | None = None,
    planner_route_map: dict[tuple[float, float], Route | None] | None = None,
) -> tuple[AppController, _UnrealLink]:
    controller = AppController.__new__(AppController)
    deps.selected_object_id = "track-1"
    deps.debug_target = None
    deps.debug_orbit_path = None
    deps.debug_orbit_preview_paths = []
    deps.debug_orbit_targets = []
    deps.debug_orbit_sequence = []
    controller._mission_state = MissionState.IN_FLIGHT
    controller._commands_enabled = True
    controller._latest_telemetry = _telemetry()
    controller._backend = "unreal"
    controller._unreal_uav_id = "sim"
    controller._unreal_link = _UnrealLink(send_route_ok=send_route_ok, route_results=route_results)
    controller._rtl_forced = False
    controller._rtl_route_sent = False
    controller._plan_vm = SimpleNamespace(
        _route_planner=_Planner(planner_route, route_map=planner_route_map),
        get_path=lambda: [(47.6060, -122.3350), (47.6070, -122.3340)],
    )
    controller._mission = SimpleNamespace(confirmed_plan=[(47.6060, -122.3350), (47.6070, -122.3340)])
    controller._reaction_target_id = None
    controller._reaction_started_monotonic = 0.0
    controller._reaction_speed_override_active = False
    controller._route_edit_mode = False
    controller._orbit_active = False
    controller._orbit_rejoin_wp = None
    controller._orbit_route_end_wp = None
    controller._orbit_resume_route = None
    controller._orbit_resume_min_index = None
    controller._orbit_started_monotonic = 0.0
    controller._orbit_min_complete_time_s = 0.0
    controller._orbit_rejoin_close_hits = 0
    controller._orbit_rejoin_threshold_m = 15.0
    controller._orbit_target_track_ids = set()
    controller._orbit_target_centers = []
    controller._orbit_flow_state = OrbitFlowState.NORMAL_FLIGHT
    controller._pending_orbit_queue = []
    controller._pending_orbit_ids = set()
    controller._orbit_battery_advisory_visible = False
    controller._orbit_battery_advisory_text = ""
    controller._orbit_battery_rtl_available = False
    controller._route_battery_advisory_visible = False
    controller._route_battery_advisory_text = ""
    controller._route_battery_rtl_available = False
    controller._pending_route_battery_action = ""
    controller._pending_route_battery_path = []
    controller._pending_orbit_battery_targets = []
    controller._pending_orbit_battery_source = "manual"
    controller._pending_orbit_advisory_route = None
    controller._pending_orbit_advisory_base_route = None
    controller._pending_orbit_advisory_target = None
    controller._auto_orbit_enabled = False
    controller._reaction_window_s = 0.01
    controller._reaction_slow_speed_mps = 1.0
    controller._debug_disable_detector_during_orbit = False
    controller._detector_running = False
    controller._detector_forced_paused_by_orbit = False
    controller._camera_monitor = SimpleNamespace(age_s=lambda: 0.0, fps=10.0)
    controller._fps = 0.0
    controller._bus_alive = False
    controller._detection_conf = 0.0
    controller._last_detection_ts = None
    controller._active_path = _ActivePath()
    controller._target_tracker = _TargetTracker()
    controller._objects_store = _ObjectsStore(_confirmed_target())
    controller.det_vm = SimpleNamespace(start=lambda: None, stop=lambda: None)
    controller.video_bridge = SimpleNamespace(clear_overlays=lambda: None)
    controller.detectorRunningChanged = _Signal()
    controller.routeBatteryAdvisoryChanged = _Signal()
    controller.orbitBatteryAdvisoryChanged = _Signal()
    controller.toastRequested = _Signal()
    controller.planConfirmedChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.statsChanged = _Signal()
    controller.map_bridge = _MapBridge()
    controller._on_objects_changed = lambda: None
    controller._emit_warning = lambda **kwargs: None
    controller._estimate_route_energy = lambda route, telemetry=None, base_route=None: SimpleNamespace(
        can_complete=True,
        margin_percent=12.0,
        required_percent=30.0,
    )
    return controller, controller._unreal_link


def test_manual_orbit_pipeline_sends_route_and_enters_orbit() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    target = _confirmed_target()

    controller._orbit_targets([target], source="manual")

    assert len(unreal.routes) == 1
    assert unreal.routes[0]["orbit_target"]["lat"] == target.lat
    assert unreal.routes[0]["orbit_target"]["lon"] == target.lon
    assert ("RESUME", None) in unreal.commands
    assert controller._orbit_active is True
    assert controller._orbit_flow_state == OrbitFlowState.ORBIT_ACTIVE
    assert controller._active_path.mode == "ORBIT"
    assert controller._reaction_speed_override_active is False
    assert controller._target_tracker.in_orbit == [11]


def test_manual_orbit_uses_only_orbit_segment_for_ui_and_exit() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route_with_rejoin())
    target = _confirmed_target()

    controller._orbit_targets([target], source="manual")
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    assert controller._active_path.orbit_paths[-1] == [
        (47.6061, -122.3349),
        (47.6065, -122.3341),
        (47.6070, -122.3340),
    ]
    assert controller._orbit_route_end_wp is not None
    assert controller._orbit_route_end_wp.lat == 47.6070
    assert controller._orbit_route_end_wp.lon == -122.3340

    near_orbit_end = TelemetrySample(
        lat=47.6070,
        lon=-122.3340,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_orbit_end)
    controller._maybe_restore_route_after_orbit(near_orbit_end)

    assert any(cmd == "ORBIT_STOP" for cmd, _ in unreal.commands)
    assert controller._orbit_active is False
    assert controller._active_path.mode == "NORMAL"
    assert deps.debug_orbit_path is None
    assert deps.debug_target is None
    assert deps.selected_object_id is None


def test_auto_orbit_failure_recovers_from_slowdown() -> None:
    controller, unreal = _build_controller(planner_route=None)
    target = _confirmed_target()
    controller._auto_orbit_enabled = True

    controller._start_reaction_window(target)
    controller._objects_store = _ObjectsStore(target)
    controller._reaction_started_monotonic = time.monotonic() - 1.0
    controller._tick_reaction_window()

    assert ("SET_SPEED", {"speed_mps": float(controller._reaction_slow_speed_mps)}) in unreal.commands
    assert any(cmd == "CLEAR_VELOCITY_OVERRIDE" for cmd, _ in unreal.commands)
    assert controller._reaction_speed_override_active is False
    assert controller._orbit_active is False
    assert controller._orbit_flow_state == OrbitFlowState.NORMAL_FLIGHT


def test_failed_route_send_does_not_leave_orbit_or_slowdown_active() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route(), send_route_ok=False)
    target = _confirmed_target()
    controller._reaction_speed_override_active = True

    controller._orbit_targets([target], source="manual")

    assert len(unreal.routes) == 1
    assert any(cmd == "CLEAR_VELOCITY_OVERRIDE" for cmd, _ in unreal.commands)
    assert controller._reaction_speed_override_active is False
    assert controller._orbit_active is False
    assert controller._orbit_flow_state == OrbitFlowState.NORMAL_FLIGHT
    assert controller._active_path.mode == "NORMAL"


def test_orbit_resume_failure_keeps_orbit_state_and_reports_error() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route(), route_results=[True, False])
    target = _confirmed_target()
    controller._orbit_targets([target], source="manual")
    controller._orbit_rejoin_wp = Waypoint(lat=47.6070, lon=-122.3340, alt=120.0)
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_rejoin = TelemetrySample(
        lat=47.6070,
        lon=-122.3340,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_rejoin)
    controller._maybe_restore_route_after_orbit(near_rejoin)

    assert any(cmd == "ORBIT_STOP" for cmd, _ in unreal.commands)
    assert controller._orbit_active is True
    assert controller._active_path.mode == "ORBIT"
    assert controller.toastRequested.calls[-1] == ("Orbit complete; failed to resume mission route",)


def test_orbit_is_blocked_while_route_edit_is_active() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    controller._route_edit_mode = True

    controller._orbit_targets([_confirmed_target()], source="manual")

    assert unreal.routes == []
    assert controller._orbit_active is False
    assert controller.toastRequested.calls[-1] == ("Apply or cancel route edit before orbit",)


def test_orbit_rejoin_stops_orbit_and_restores_normal_path() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    target = _confirmed_target()
    controller._orbit_targets([target], source="manual")
    controller._orbit_rejoin_wp = Waypoint(lat=47.6070, lon=-122.3340, alt=120.0)
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_rejoin = TelemetrySample(
        lat=47.6070,
        lon=-122.3340,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_rejoin)
    controller._maybe_restore_route_after_orbit(near_rejoin)

    assert any(cmd == "ORBIT_STOP" for cmd, _ in unreal.commands)
    assert len(unreal.routes) == 2
    assert unreal.routes[-1]["active_index"] == 0
    resumed_first = unreal.routes[-1]["waypoints"][0]
    assert abs(resumed_first["lat"] - near_rejoin.lat) < 0.001
    assert abs(resumed_first["lon"] - near_rejoin.lon) < 0.001
    assert controller._orbit_active is False
    assert controller._orbit_flow_state == OrbitFlowState.NORMAL_FLIGHT
    assert controller._active_path.mode == "NORMAL"
    assert controller._target_tracker.orbited == [11]


def test_orbit_resume_does_not_jump_back_before_rejoin_index() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    target = _confirmed_target()
    controller._orbit_targets([target], source="manual")
    controller._orbit_resume_min_index = 1
    controller._orbit_route_end_wp = Waypoint(lat=47.6060, lon=-122.3350, alt=120.0)
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_start = TelemetrySample(
        lat=47.6060,
        lon=-122.3350,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_start)
    controller._maybe_restore_route_after_orbit(near_start)

    assert unreal.routes[-1]["active_index"] == 0
    resumed_first = unreal.routes[-1]["waypoints"][0]
    assert abs(resumed_first["lat"] - near_start.lat) < 0.001
    assert abs(resumed_first["lon"] - near_start.lon) < 0.001


def test_orbit_debug_detector_pause_stops_and_restores_detector() -> None:
    controller, _unreal = _build_controller(planner_route=_orbit_route())
    controller._debug_disable_detector_during_orbit = True
    controller._detector_running = True
    detector_calls: list[str] = []
    overlay_clears: list[str] = []
    controller.det_vm = SimpleNamespace(
        start=lambda: detector_calls.append("start"),
        stop=lambda: detector_calls.append("stop"),
        reset=lambda: detector_calls.append("reset"),
    )
    controller.video_bridge = SimpleNamespace(clear_overlays=lambda: overlay_clears.append("clear"))
    target = _confirmed_target()

    controller._orbit_targets([target], source="manual")

    assert detector_calls == ["stop", "reset"]
    assert overlay_clears == ["clear"]
    assert controller._detector_forced_paused_by_orbit is True
    assert controller._detector_running is False

    near_rejoin = TelemetrySample(
        lat=47.6070,
        lon=-122.3340,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0
    controller._maybe_restore_route_after_orbit(near_rejoin)
    controller._maybe_restore_route_after_orbit(near_rejoin)

    assert detector_calls == ["stop", "reset", "start"]
    assert controller._detector_forced_paused_by_orbit is False
    assert controller._detector_running is True


def test_orbit_completion_starts_only_next_pending_target() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    first = _confirmed_target()
    second = _confirmed_target2()
    controller._objects_store = _ObjectsStore(first, extra=[second])
    controller._pending_orbit_queue = [(second.object_id, "manual")]
    controller._pending_orbit_ids = {second.object_id}

    controller._orbit_targets([first], source="manual")
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_orbit_end = TelemetrySample(
        lat=47.6070,
        lon=-122.3340,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_orbit_end)
    controller._maybe_restore_route_after_orbit(near_orbit_end)

    assert len(unreal.routes) == 2
    assert unreal.routes[-1]["orbit_target"]["lat"] == second.lat
    assert controller._pending_orbit_queue == []
    assert not any(cmd == "ORBIT_STOP" for cmd, _ in unreal.commands)


def test_orbit_all_targets_builds_preview_paths_for_each_target() -> None:
    first = _confirmed_target()
    second = _confirmed_target2()
    route_map = {
        (round(first.lat, 6), round(first.lon, 6)): _orbit_route(),
        (round(second.lat, 6), round(second.lon, 6)): _orbit_route_for_target2(),
    }
    controller, unreal = _build_controller(planner_route=_orbit_route(), planner_route_map=route_map)
    controller._objects_store = _ObjectsStore(first, extra=[second])

    controller._orbit_targets([first, second], source="manual")

    assert len(unreal.routes) == 1
    assert len(deps.debug_orbit_targets) == 2
    assert len(deps.debug_orbit_preview_paths) == 2
    assert deps.debug_orbit_preview_paths[0] == [
        (47.6061, -122.3349),
        (47.6065, -122.3341),
        (47.6070, -122.3340),
    ]
    assert deps.debug_orbit_preview_paths[1] == [
        (47.6068, -122.3342),
        (47.6072, -122.3336),
        (47.6076, -122.3331),
    ]


def test_orbit_does_not_resume_too_early_when_crossing_resume_point() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    target = _confirmed_target()
    controller._orbit_targets([target], source="manual")
    controller._orbit_route_end_wp = Waypoint(lat=47.6061, lon=-122.3349, alt=120.0)
    controller._orbit_started_monotonic = time.monotonic()
    controller._orbit_min_complete_time_s = 30.0

    early_touch = TelemetrySample(
        lat=47.6061,
        lon=-122.3349,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(early_touch)
    controller._maybe_restore_route_after_orbit(early_touch)

    assert controller._orbit_active is True
    assert not any(cmd == "ORBIT_STOP" for cmd, _ in unreal.commands)


def test_confirm_plan_renders_map_after_route_estimate() -> None:
    controller = AppController.__new__(AppController)
    controller._plan_vm = SimpleNamespace(get_path=lambda: [(47.6060, -122.3350), (47.6070, -122.3340)])
    controller._telemetry_available = lambda: True
    controller._backend = "unreal"
    controller._mission = SimpleNamespace(
        confirm_plan=lambda path: True,
        current_state=MissionState.READY,
    )
    controller._active_path = SimpleNamespace(
        set_normal=lambda: None,
        get_active_path=lambda: [(47.6060, -122.3350), (47.6070, -122.3340)],
    )
    updates: list[str] = []
    controller._update_route_estimate = lambda path: updates.append("estimate")
    bridge = _MapBridge()
    bridge._render_map_now = bridge.render_map
    controller.map_bridge = bridge
    controller.planConfirmedChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._latest_telemetry = None
    controller._route_energy_ok = lambda path: True
    controller._unreal_link = None

    controller.confirmPlan()

    assert updates == ["estimate"]
    assert controller.map_bridge.render_count == 1


def test_confirm_plan_in_unreal_autostarts_via_start_flight() -> None:
    controller = AppController.__new__(AppController)
    controller._plan_vm = SimpleNamespace(get_path=lambda: [(47.6060, -122.3350), (47.6070, -122.3340)])
    controller._telemetry_available = lambda: True
    controller._backend = "unreal"
    controller._mission = SimpleNamespace(
        confirm_plan=lambda path: True,
        current_state=MissionState.READY,
    )
    controller._active_path = SimpleNamespace(
        set_normal=lambda: None,
        get_active_path=lambda: [(47.6060, -122.3350), (47.6070, -122.3340)],
    )
    controller._update_route_estimate = lambda path: None
    controller.map_bridge = _MapBridge()
    controller.planConfirmedChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._latest_telemetry = None
    controller._route_energy_ok = lambda path: True
    controller._unreal_link = _UnrealLink()
    controller._unreal_uav_id = "sim"
    start_calls: list[tuple[bool, bool]] = []
    controller.startFlight = (
        lambda *, skip_checks=False, allow_unsafe_energy=False: start_calls.append(
            (bool(skip_checks), bool(allow_unsafe_energy))
        )
    )

    controller.confirmPlan()

    assert start_calls == [(True, False)]


def test_final_orbit_resume_uses_nearest_point_on_original_route() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    target = _confirmed_target()
    controller._mission = SimpleNamespace(
        confirmed_plan=[
            (47.6050, -122.3400),
            (47.6060, -122.3350),
            (47.6070, -122.3340),
            (47.6080, -122.3330),
        ]
    )

    controller._orbit_targets([target], source="manual")
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_orbit_end = TelemetrySample(
        lat=47.6066,
        lon=-122.3343,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )
    controller._orbit_route_end_wp = Waypoint(lat=near_orbit_end.lat, lon=near_orbit_end.lon, alt=120.0)

    controller._maybe_restore_route_after_orbit(near_orbit_end)
    controller._maybe_restore_route_after_orbit(near_orbit_end)

    assert len(unreal.routes) == 2
    resumed = unreal.routes[-1]
    assert resumed["active_index"] == 0
    assert resumed["waypoints"]
    first_wp = resumed["waypoints"][0]
    assert abs(first_wp["lat"] - 47.6066) < 0.001
    assert abs(first_wp["lon"] - -122.3344) < 0.002


def test_orbit_resume_tracks_current_segment_even_if_final_waypoint_is_closer() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    controller._mission = SimpleNamespace(
        confirmed_plan=[
            (47.6050, -122.3400),
            (47.6088, -122.3400),
            (47.6088, -122.3322),
            (47.6062, -122.3348),
        ]
    )
    controller._latest_telemetry = TelemetrySample(
        lat=47.6066,
        lon=-122.3344,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._orbit_targets([_confirmed_target()], source="manual")

    assert controller._orbit_resume_route is not None
    assert controller._orbit_resume_route.active_index == 2

    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0
    controller._orbit_route_end_wp = Waypoint(lat=47.6066, lon=-122.3344, alt=120.0)

    controller._maybe_restore_route_after_orbit(controller._latest_telemetry)
    controller._maybe_restore_route_after_orbit(controller._latest_telemetry)

    resumed = unreal.routes[-1]
    assert len(resumed["waypoints"]) >= 2
    first_wp = resumed["waypoints"][0]
    second_wp = resumed["waypoints"][1]
    assert abs(first_wp["lat"] - 47.6066) < 0.001
    assert abs(first_wp["lon"] - -122.3344) < 0.001
    assert abs(second_wp["lat"] - 47.6062) < 0.001
    assert abs(second_wp["lon"] - -122.3348) < 0.001


def test_route_energy_warning_emits_once_until_state_recovers() -> None:
    controller = AppController.__new__(AppController)
    controller._latest_telemetry = TelemetrySample(
        lat=47.6060,
        lon=-122.3350,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.25,
        battery_percent=25.0,
        timestamp=utc_now(),
    )
    controller._energy_model = PythonEnergyModel(max_flight_distance_m=1000.0, min_return_percent=20.0)
    controller._route_battery_warning = False
    controller._route_battery_text = "Route: --"
    controller._route_battery_remaining_text = "Remaining: --"
    controller._mission_state = MissionState.IN_FLIGHT
    controller._link_monitor = SimpleNamespace(is_link_ok=lambda: True)
    controller.routeBatteryChanged = _Signal()
    warnings: list[str] = []
    controller._emit_warning = lambda **kwargs: warnings.append(str(kwargs["key"]))

    path = [
        (47.6060, -122.3350),
        (47.6070, -122.3340),
        (47.6160, -122.3250),
    ]

    controller._update_route_estimate(path)
    controller._update_route_estimate(path)

    assert warnings == ["route_energy_insufficient"]
    assert controller._route_battery_warning is True

    controller._latest_telemetry = TelemetrySample(
        lat=47.6060,
        lon=-122.3350,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.95,
        battery_percent=95.0,
        timestamp=utc_now(),
    )
    controller._energy_model = PythonEnergyModel(max_flight_distance_m=5000.0, min_return_percent=20.0)
    controller._update_route_estimate(path)

    assert controller._route_battery_warning is False


def test_orbit_completion_picks_nearest_pending_target_from_current_position() -> None:
    planner_route = Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=47.6061, lon=-122.3349, alt=120.0),
            Waypoint(lat=47.6069, lon=-122.3338, alt=120.0),
            Waypoint(lat=47.6076, lon=-122.3331, alt=120.0),
        ],
    )
    controller, unreal = _build_controller(planner_route=planner_route)
    first = _confirmed_target()
    second = _confirmed_target2()
    third = ConfirmedObject(
        object_id="track-3",
        class_id=1,
        confidence=0.92,
        lat=47.6078,
        lon=-122.3330,
        track_id=13,
        timestamp=utc_now(),
    )
    controller._objects_store = _ObjectsStore(first, extra=[second, third])
    controller._pending_orbit_queue = [(second.object_id, "manual"), (third.object_id, "manual")]
    controller._pending_orbit_ids = {second.object_id, third.object_id}

    controller._orbit_targets([first], source="manual")
    controller._orbit_started_monotonic = time.monotonic() - 60.0
    controller._orbit_min_complete_time_s = 0.0

    near_orbit_end = TelemetrySample(
        lat=47.6076,
        lon=-122.3331,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    controller._maybe_restore_route_after_orbit(near_orbit_end)
    controller._maybe_restore_route_after_orbit(near_orbit_end)

    assert len(unreal.routes) == 2
    assert unreal.routes[-1]["orbit_target"]["lat"] == third.lat


def test_orbit_shows_battery_dialog_before_start_when_margin_is_low() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    controller._estimate_route_energy = lambda route, telemetry=None: SimpleNamespace(
        can_complete=True,
        margin_percent=2.0,
        required_percent=55.0,
    )

    controller._orbit_targets([_confirmed_target()], source="manual")

    assert len(unreal.routes) == 0
    assert controller._orbit_battery_advisory_visible is True
    assert "Orbit may not be completed safely" in controller._orbit_battery_advisory_text


def test_critical_battery_sends_rtl_route_to_unreal() -> None:
    controller, unreal = _build_controller(planner_route=_orbit_route())
    controller._energy_model = SimpleNamespace(is_critical=lambda sample: True)
    controller._route_battery_advisory_visible = False
    controller._orbit_battery_advisory_visible = False
    controller._mission = SimpleNamespace(trigger_rtl=lambda reason: None, confirmed_plan=[(47.6060, -122.3350), (47.6070, -122.3340)])

    controller._handle_battery(controller._latest_telemetry)

    assert len(unreal.routes) == 1
    sent = unreal.routes[0]
    assert sent["waypoints"][-1]["lat"] == 47.6060
    assert sent["waypoints"][-1]["lon"] == -122.3350
    assert controller._active_path.rtl_paths


def test_confirm_plan_shows_route_battery_dialog_when_route_is_unsafe() -> None:
    controller = AppController.__new__(AppController)
    controller._plan_vm = SimpleNamespace(get_path=lambda: [(47.6060, -122.3350), (47.6160, -122.3250)])
    controller._telemetry_available = lambda: True
    controller._latest_telemetry = _telemetry()
    controller._route_battery_advisory_visible = False
    controller._route_battery_advisory_text = ""
    controller._route_battery_rtl_available = False
    controller._pending_route_battery_action = ""
    controller._pending_route_battery_path = []
    controller.routeBatteryAdvisoryChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._emit_warning = lambda **kwargs: None
    controller._route_energy_summary = lambda path: {
        "can_complete": False,
        "required_percent": 88.0,
        "margin_percent": -13.0,
        "available_percent": 55.0,
        "reserved_percent": 20.0,
    }
    controller._rtl_route_available = lambda: False
    controller._mission = SimpleNamespace(confirm_plan=lambda path: (_ for _ in ()).throw(RuntimeError("should not confirm")))

    controller.confirmPlan()

    assert controller._route_battery_advisory_visible is True
    assert controller._pending_route_battery_action == "confirm_plan"
    assert "Route may not be completed safely" in controller._route_battery_advisory_text


def test_confirm_plan_can_show_route_battery_dialog_without_live_telemetry() -> None:
    controller = AppController.__new__(AppController)
    path = [(47.6060, -122.3350), (47.6160, -122.3250)]
    controller._plan_vm = SimpleNamespace(get_path=lambda: path)
    controller._telemetry_available = lambda: True
    controller._backend = "unreal"
    controller._latest_telemetry = None
    controller._route_battery_advisory_visible = False
    controller._route_battery_advisory_text = ""
    controller._route_battery_rtl_available = False
    controller._pending_route_battery_action = ""
    controller._pending_route_battery_path = []
    controller.routeBatteryAdvisoryChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._emit_warning = lambda **kwargs: None
    controller._rtl_route_available = lambda: False
    controller._route_from_points = lambda pts: Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=pts[0][0], lon=pts[0][1], alt=120.0),
            Waypoint(lat=pts[1][0], lon=pts[1][1], alt=120.0),
        ],
    )

    def _estimate(route, telemetry=None, base_route=None):
        assert telemetry is not None
        assert telemetry.battery_percent == 100.0
        return SimpleNamespace(can_complete=False, required_percent=108.0, margin_percent=-8.0)

    controller._estimate_route_energy = _estimate
    controller._mission = SimpleNamespace(confirm_plan=lambda pts: (_ for _ in ()).throw(RuntimeError("should not confirm")))

    controller.confirmPlan()

    assert controller._route_battery_advisory_visible is True
    assert controller._pending_route_battery_action == "confirm_plan"
    assert "available 100.0%" in controller._route_battery_advisory_text


def test_confirm_plan_battery_override_in_unreal_autostarts_unsafe_flight() -> None:
    controller = AppController.__new__(AppController)
    path = [(47.6060, -122.3350), (47.6070, -122.3340)]
    controller._backend = "unreal"
    controller._unreal_link = _UnrealLink()
    controller._mission = SimpleNamespace(confirm_plan=lambda pts: True)
    controller._active_path = SimpleNamespace(
        set_normal=lambda: None,
        get_active_path=lambda: path,
    )
    controller._update_route_estimate = lambda pts: None
    controller.map_bridge = _MapBridge()
    controller.planConfirmedChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.toastRequested = _Signal()
    start_calls: list[tuple[bool, bool]] = []
    controller.startFlight = (
        lambda *, skip_checks=False, allow_unsafe_energy=False: start_calls.append(
            (bool(skip_checks), bool(allow_unsafe_energy))
        )
    )

    controller._confirm_plan_after_battery_override(path)

    assert start_calls == [(True, True)]


def test_route_battery_capsule_uses_energy_summary_values() -> None:
    controller = AppController.__new__(AppController)
    controller._active_path = SimpleNamespace(get_active_path=lambda: [])
    controller._resolve_max_distance_m = lambda: 5000.0
    controller._latest_telemetry = TelemetrySample(
        lat=47.6060,
        lon=-122.3350,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )
    controller._route_battery_warning = False
    controller._route_battery_text = "Route: --"
    controller._route_battery_remaining_text = "Remaining: --"
    controller._mission_state = MissionState.PREFLIGHT
    controller._link_monitor = SimpleNamespace(is_link_ok=lambda: True)
    controller.routeBatteryChanged = _Signal()
    controller._emit_warning = lambda **kwargs: None
    controller._route_from_points = lambda path: Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=path[0][0], lon=path[0][1], alt=120.0),
            Waypoint(lat=path[1][0], lon=path[1][1], alt=120.0),
        ],
    )
    controller._resolve_base_location = lambda route, telemetry: SimpleNamespace(lat=47.6060, lon=-122.3350)
    controller._route_energy_summary = lambda path, telemetry=None: {
        "can_complete": False,
        "required_percent": 18.0,
        "margin_percent": -7.0,
        "available_percent": 90.0,
        "reserved_percent": 15.0,
    }

    controller._update_route_estimate([(47.6060, -122.3350), (47.6160, -122.3250)])

    assert controller._route_battery_warning is True
    assert controller._route_battery_text == "Route+RTL: 18.0%"
    assert controller._route_battery_remaining_text == "Remaining after route+reserve: -7.0%"


def test_runtime_battery_check_uses_mission_base_route_for_home_resolution() -> None:
    controller, _unreal = _build_controller(planner_route=_orbit_route())
    calls: list[Route | None] = []
    controller._energy_model = SimpleNamespace(is_critical=lambda sample: False)
    controller._last_unreal_route_sent_monotonic = 0.0
    controller._unreal_waiting_route_grace_s = 2.5
    controller._active_path._current_path = [
        (47.6075, -122.3335),
        (47.6080, -122.3330),
        (47.6085, -122.3325),
    ]
    controller._mission = SimpleNamespace(
        confirmed_plan=[
            (47.6060, -122.3350),
            (47.6070, -122.3340),
            (47.6080, -122.3330),
        ]
    )

    def _estimate(route, telemetry=None, *, base_route=None):
        calls.append(base_route)
        return SimpleNamespace(can_complete=True)

    controller._estimate_route_energy = _estimate

    controller._handle_battery(controller._latest_telemetry)

    assert calls
    assert calls[0] is not None
    assert calls[0].waypoints[0].lat == 47.6060
    assert calls[0].waypoints[0].lon == -122.3350


def test_runtime_battery_check_is_ignored_briefly_after_unreal_route_send() -> None:
    controller, _unreal = _build_controller(planner_route=_orbit_route())
    controller._energy_model = SimpleNamespace(is_critical=lambda sample: False)
    controller._last_unreal_route_sent_monotonic = time.monotonic()
    controller._unreal_waiting_route_grace_s = 2.5
    controller._estimate_route_energy = lambda route, telemetry=None, base_route=None: SimpleNamespace(
        can_complete=False
    )
    rtl_calls: list[tuple[str, str]] = []
    controller._initiate_rtl = lambda *, reason, user_message: rtl_calls.append((reason, user_message))

    controller._handle_battery(controller._latest_telemetry)

    assert controller._rtl_forced is False
    assert rtl_calls == []


def test_start_flight_skip_checks_still_blocks_unsafe_route() -> None:
    controller = AppController.__new__(AppController)
    controller._mission = SimpleNamespace(
        plan_confirmed=True,
        confirmed_plan=[(47.6060, -122.3350), (47.6160, -122.3250)],
        start_flight=lambda skip_checks=False: (_ for _ in ()).throw(RuntimeError("should not start")),
    )
    controller._link_monitor = SimpleNamespace(is_link_ok=lambda: True)
    controller._camera_monitor = SimpleNamespace(is_camera_ok=lambda: True)
    controller._allow_unsafe_start = False
    controller._recoverable_mission = None
    controller.recoverableMissionChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._route_complete_announced = False
    controller._rtl_forced = False
    controller._rtl_route_sent = False
    controller._commands_enabled = False
    controller._clear_route_battery_advisory = lambda: None
    controller._clear_orbit_battery_advisory = lambda: None
    prompts: list[tuple[list[tuple[float, float]], str]] = []
    controller._maybe_prompt_route_battery_advisory = lambda path, action: prompts.append((list(path), action)) or True

    controller.startFlight(skip_checks=True)

    assert prompts == [([(47.6060, -122.3350), (47.6160, -122.3250)], "start_flight")]


def test_start_flight_in_unreal_sends_confirmed_route() -> None:
    controller = AppController.__new__(AppController)
    confirmed_path = [(47.6060, -122.3350), (47.6070, -122.3340)]
    start_calls: list[bool] = []
    controller._mission = SimpleNamespace(
        plan_confirmed=True,
        confirmed_plan=confirmed_path,
        start_flight=lambda skip_checks=False: start_calls.append(bool(skip_checks)) or True,
    )
    controller._backend = "unreal"
    controller._unreal_uav_id = "sim"
    controller._unreal_link = _UnrealLink()
    controller._route_from_points = lambda path: Route(
        version=1,
        active_index=0,
        waypoints=[
            Waypoint(lat=path[0][0], lon=path[0][1], alt=120.0),
            Waypoint(lat=path[1][0], lon=path[1][1], alt=120.0),
        ],
    )
    controller._link_monitor = SimpleNamespace(is_link_ok=lambda: True)
    controller._camera_monitor = SimpleNamespace(is_camera_ok=lambda: True)
    controller._allow_unsafe_start = False
    controller._recoverable_mission = None
    controller.recoverableMissionChanged = _Signal()
    controller.flightControlsChanged = _Signal()
    controller.toastRequested = _Signal()
    controller._route_complete_announced = False
    controller._rtl_forced = False
    controller._rtl_route_sent = False
    controller._commands_enabled = False
    controller._active_path = SimpleNamespace(clear_overrides_on_new_flight=lambda: None)
    controller.map_bridge = _MapBridge()
    controller._map_refresh_needed = False
    controller.mapRefreshNeededChanged = _Signal()
    controller._maybe_prompt_route_battery_advisory = lambda path, action: False

    controller.startFlight(skip_checks=True)

    assert len(controller._unreal_link.routes) == 1
    assert start_calls == [True]


def test_waiting_for_route_is_ignored_briefly_after_unreal_route_send() -> None:
    controller = AppController.__new__(AppController)
    controller._backend = "unreal"
    controller._mission_state = MissionState.IN_FLIGHT
    controller._last_unreal_route_sent_monotonic = time.monotonic()
    controller._unreal_waiting_route_grace_s = 2.5
    controller._route_complete_announced = False
    controller._mission = SimpleNamespace(abort_to_preflight=lambda reason: (_ for _ in ()).throw(RuntimeError("should not abort")))
    controller._stash_recoverable_mission = lambda: (_ for _ in ()).throw(RuntimeError("should not stash"))

    controller._handle_mission_progress_from_telemetry(
        SimpleNamespace(flight_mode="WAITING_FOR_ROUTE", status="WAITING_FOR_ROUTE")
    )

    assert controller._mission_state == MissionState.IN_FLIGHT


def test_waiting_for_route_aborts_after_grace_window_expires() -> None:
    controller = AppController.__new__(AppController)
    controller._backend = "unreal"
    controller._mission_state = MissionState.IN_FLIGHT
    controller._last_unreal_route_sent_monotonic = time.monotonic() - 10.0
    controller._unreal_waiting_route_grace_s = 2.5
    controller._route_complete_announced = False
    stash_calls: list[str] = []
    abort_calls: list[str] = []
    controller._stash_recoverable_mission = lambda: stash_calls.append("stash")
    controller._mission = SimpleNamespace(abort_to_preflight=lambda reason: abort_calls.append(str(reason)))

    controller._handle_mission_progress_from_telemetry(
        SimpleNamespace(flight_mode="WAITING_FOR_ROUTE", status="WAITING_FOR_ROUTE")
    )

    assert stash_calls == ["stash"]
    assert abort_calls == ["sim_waiting_for_route"]


def test_active_path_for_sim_does_not_prepend_far_telemetry() -> None:
    controller, _unreal = _build_controller(planner_route=_orbit_route())
    controller._mission_state = MissionState.IN_FLIGHT
    controller._active_path._current_path = [(47.6060, -122.3350), (47.6070, -122.3340)]
    controller._latest_telemetry = TelemetrySample(
        lat=55.0,
        lon=37.0,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    path = controller.get_active_path_for_sim()

    assert path == [(47.6060, -122.3350), (47.6070, -122.3340)]


def test_active_path_for_sim_trims_to_nearest_waypoint_before_prepending_telemetry() -> None:
    controller, _unreal = _build_controller(planner_route=_orbit_route())
    controller._mission_state = MissionState.IN_FLIGHT
    controller._active_path._current_path = [
        (47.6060, -122.3350),
        (47.6070, -122.3340),
        (47.6080, -122.3330),
    ]
    controller._latest_telemetry = TelemetrySample(
        lat=47.6072,
        lon=-122.3338,
        alt=120.0,
        yaw=0.0,
        pitch=0.0,
        roll=0.0,
        battery=0.9,
        battery_percent=90.0,
        timestamp=utc_now(),
    )

    path = controller.get_active_path_for_sim()

    assert path[0] == (47.6072, -122.3338)
    assert path[1:] == [(47.6070, -122.3340), (47.6080, -122.3330)]
