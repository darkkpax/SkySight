from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Protocol

from fire_uav.module_core.contract.v1 import CapabilitiesV1, CommandV1, RouteV1
from fire_uav.module_core.contract.mappers import route_v1_to_internal
from fire_uav.module_core.schema import Route, TelemetrySample


class IUavTelemetryConsumer(Protocol):
    async def on_telemetry(self, sample: TelemetrySample) -> None:
        """Called when new telemetry is received from the UAV."""


class IUavAdapter(ABC):
    """
    Abstract adapter between module_app and a concrete UAV backend
    (real autopilot, simulator like Unreal/AirSim, or client's software).
    """

    @abstractmethod
    async def start(self, telemetry_callback: IUavTelemetryConsumer) -> None:
        """
        Start the adapter and begin receiving telemetry.
        The adapter must call telemetry_callback.on_telemetry(sample)
        whenever new telemetry is available.
        """

    @abstractmethod
    async def stop(self) -> None:
        """Stop the adapter and release resources."""

    @abstractmethod
    async def push_route(self, route: Route) -> None:
        """
        Send or update the current route/mission on the UAV side.
        For a real autopilot this may upload a mission, for a simulator it may
        set a path to follow.
        """

    @abstractmethod
    async def send_simple_command(self, command: str, payload: dict | None = None) -> None:
        """
        Send a simple control command, e.g.:
          - "ARM", "DISARM"
          - "TAKEOFF", "LAND"
          - "ABORT_MISSION"
        Exact mapping is up to concrete adapters.
        """

    async def connect(self, telemetry_callback: IUavTelemetryConsumer) -> None:
        """Compatibility shim for driver-style naming."""
        await self.start(telemetry_callback)

    async def disconnect(self) -> None:
        """Compatibility shim for driver-style naming."""
        await self.stop()

    async def send_route(self, route: Route) -> None:
        """Driver-style alias for push_route."""
        await self.push_route(route)

    async def send_route_v1(self, route: RouteV1) -> None:
        """Send a v1 route by mapping it to the internal Route model."""
        await self.push_route(route_v1_to_internal(route))

    async def send_command(self, cmd: CommandV1) -> None:
        """Send a structured command using the legacy simple-command API."""
        await self.send_simple_command(cmd.type, cmd.params)

    async def get_capabilities(self) -> CapabilitiesV1:
        """Return default capabilities when the adapter cannot report them."""
        return CapabilitiesV1(
            supports_waypoints=True,
            supports_rtl=True,
            supports_orbit=True,
            supports_set_speed=False,
            supports_camera=True,
            max_waypoints=None,
            notes="default",
        )

    async def read_telemetry(self) -> TelemetrySample | None:
        """Optional polling hook; push-based adapters can return None."""
        return None
