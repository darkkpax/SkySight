from __future__ import annotations

import json
import logging
import tempfile
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable

import httpx
from PySide6.QtCore import QObject, QThread, QTimer, Signal
from PySide6.QtGui import QImage

from fire_uav.core.protocol import MapBounds, TelemetryMessage
from fire_uav.core.telemetry import telemetry_sample_from_message

log = logging.getLogger(__name__)

try:
    import av  # type: ignore
except Exception:  # noqa: BLE001
    av = None


def _interruptible_sleep(duration_s: float, should_continue: Callable[[], bool]) -> bool:
    """Sleep in short chunks so worker stop() reacts quickly."""
    end_ts = time.monotonic() + max(0.0, float(duration_s))
    while should_continue() and time.monotonic() < end_ts:
        time.sleep(min(0.1, max(0.0, end_ts - time.monotonic())))
    return should_continue()


class _JpegPollWorker(QThread):
    frameReady = Signal(QImage)
    statusChanged = Signal(str)

    def __init__(
        self,
        *,
        base_url: str,
        target_hz: float,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self._base_url = base_url.rstrip("/")
        self._target_hz = max(0.1, float(target_hz))
        self._poll_interval_s = 1.0 / target_hz if target_hz > 0 else 0.2
        self._running = True
        self._enabled = True
        self._last_status: str | None = None
        self._client = httpx.Client(timeout=2.5)
        self._reconnect_delays_s = (1.0, 2.0, 5.0)
        self._reconnect_idx = 0
        self._last_error_log_ts = 0.0
        self._last_recover_log_ts = 0.0
        self._preferred_suffix = "camera.jpg"

    def stop(self) -> None:
        self._running = False
        try:
            self._client.close()
        except Exception:  # noqa: BLE001
            pass

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = bool(enabled)

    def update_base_url(self, base_url: str) -> None:
        self._base_url = base_url.rstrip("/")

    def _camera_suffixes(self) -> tuple[str, ...]:
        preferred = self._preferred_suffix if self._preferred_suffix in ("camera.jpg", "camera.png") else "camera.jpg"
        other = "camera.png" if preferred == "camera.jpg" else "camera.jpg"
        return (preferred, other)

    def run(self) -> None:
        while self._running:
            if not self._enabled:
                self._set_status("paused")
                if not _interruptible_sleep(0.2, lambda: self._running):
                    break
                continue

            started = time.monotonic()
            try:
                frame = self._fetch_frame()
                if frame is None:
                    self._set_status("camera_not_found")
                    if not _interruptible_sleep(self._poll_interval_s, lambda: self._running):
                        break
                    continue
                self._reconnect_idx = 0
                self._set_status("streaming")
                self.frameReady.emit(frame)
                elapsed = time.monotonic() - started
                if not _interruptible_sleep(max(0.0, self._poll_interval_s - elapsed), lambda: self._running):
                    break
            except _JpegWaitingForRoute:
                self._reconnect_idx = 0
                self._set_status("waiting_for_route")
                if not _interruptible_sleep(self._poll_interval_s, lambda: self._running):
                    break
            except Exception as exc:  # noqa: BLE001
                self._set_status("disconnected")
                now = time.monotonic()
                if (now - self._last_error_log_ts) >= 2.0:
                    delay_s = self._reconnect_delays_s[min(self._reconnect_idx, len(self._reconnect_delays_s) - 1)]
                    log.warning(
                        "Unreal JPEG polling error (%s); reconnect in %.1fs",
                        exc,
                        delay_s,
                    )
                    self._last_error_log_ts = now
                delay_s = self._reconnect_delays_s[min(self._reconnect_idx, len(self._reconnect_delays_s) - 1)]
                self._reconnect_idx = min(self._reconnect_idx + 1, len(self._reconnect_delays_s) - 1)
                if not _interruptible_sleep(delay_s, lambda: self._running):
                    break

    def _fetch_frame(self) -> QImage | None:
        for suffix in self._camera_suffixes():
            resp = self._client.get(
                f"{self._base_url}/sim/v1/{suffix}",
                params={"hz": f"{self._target_hz:g}"},
            )
            if resp.status_code == 503:
                raise _JpegWaitingForRoute()
            if resp.status_code == 404:
                continue
            resp.raise_for_status()
            img = QImage.fromData(resp.content)
            if img.isNull():
                raise RuntimeError("Invalid image data")
            self._preferred_suffix = suffix
            now = time.monotonic()
            if (now - self._last_recover_log_ts) >= 5.0 and self._reconnect_idx > 0:
                log.info("Unreal JPEG polling recovered")
                self._last_recover_log_ts = now
            return img
        return None

    def _set_status(self, status: str) -> None:
        if status == self._last_status:
            return
        self._last_status = status
        self.statusChanged.emit(status)


class _JpegWaitingForRoute(RuntimeError):
    pass


class _H264StreamWorker(QThread):
    frameReady = Signal(QImage)
    statusChanged = Signal(str)

    def __init__(
        self,
        *,
        url: str,
        target_fps: float,
        reconnect_s: float,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self._url = url
        self._target_interval = 1.0 / target_fps if target_fps > 0 else 0.0
        self._base_reconnect_s = max(0.2, reconnect_s)
        self._running = True
        self._enabled = True
        self._last_status: str | None = None
        self._probe_client = httpx.Client(timeout=2.0)
        self._reconnect_delays_s = (
            self._base_reconnect_s,
            max(self._base_reconnect_s, 2.0),
            max(self._base_reconnect_s, 5.0),
        )
        self._reconnect_idx = 0
        self._last_error_log_ts = 0.0
        self._container = None

    def stop(self) -> None:
        self._running = False
        container = self._container
        if container is not None:
            try:
                container.close()
            except Exception:  # noqa: BLE001
                pass
        try:
            self._probe_client.close()
        except Exception:  # noqa: BLE001
            pass

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = bool(enabled)

    def update_url(self, url: str) -> None:
        self._url = url

    def run(self) -> None:
        next_emit_ts = 0.0
        while self._running:
            if not self._enabled:
                self._set_status("paused")
                if not _interruptible_sleep(0.2, lambda: self._running):
                    break
                continue

            container = None
            try:
                container = av.open(
                    self._url,
                    format="mpegts",
                    options={
                        # Keep reconnect/stop responsiveness reasonable on broken links.
                        "rw_timeout": "2000000",
                    },
                )
                self._container = container
                self._reconnect_idx = 0
                self._set_status("streaming")
                next_emit_ts = 0.0
                for frame in container.decode(video=0):
                    if not self._running or not self._enabled:
                        break
                    now = time.monotonic()
                    if self._target_interval > 0.0:
                        if next_emit_ts <= 0.0:
                            next_emit_ts = now
                        if now < next_emit_ts:
                            continue
                    arr = frame.to_ndarray(format="rgb24")
                    h, w, _ = arr.shape
                    img = QImage(arr.data, w, h, 3 * w, QImage.Format.Format_RGB888).copy()
                    self.frameReady.emit(img)
                    if self._target_interval > 0.0:
                        next_emit_ts += self._target_interval
                        if next_emit_ts < (now - self._target_interval):
                            next_emit_ts = now + self._target_interval
            except Exception as exc:  # noqa: BLE001
                status = self._probe_status()
                self._set_status(status)
                delay_s = self._reconnect_delays_s[min(self._reconnect_idx, len(self._reconnect_delays_s) - 1)]
                self._reconnect_idx = min(self._reconnect_idx + 1, len(self._reconnect_delays_s) - 1)
                if status == "disconnected":
                    now = time.monotonic()
                    if (now - self._last_error_log_ts) >= 2.0:
                        log.warning("Unreal H264 stream error (%s); reconnect in %.1fs", exc, delay_s)
                        self._last_error_log_ts = now
            finally:
                self._container = None
                if container is not None:
                    try:
                        container.close()
                    except Exception:  # noqa: BLE001
                        pass
            if not self._running:
                break
            delay_s = self._reconnect_delays_s[min(self._reconnect_idx, len(self._reconnect_delays_s) - 1)]
            if not _interruptible_sleep(delay_s, lambda: self._running):
                break

    def _set_status(self, status: str) -> None:
        if status == self._last_status:
            return
        self._last_status = status
        self.statusChanged.emit(status)

    def _probe_status(self) -> str:
        try:
            with self._probe_client.stream("GET", self._url) as resp:
                if resp.status_code == 503:
                    return "waiting_for_route"
                if resp.status_code >= 400:
                    return "disconnected"
        except Exception:
            return "disconnected"
        return "disconnected"


@dataclass
class _Backoff:
    base_s: float = 1.0
    max_s: float = 5.0
    current_s: float = 1.0
    next_ts: float = 0.0

    def allow(self) -> bool:
        return time.monotonic() >= self.next_ts

    def success(self) -> None:
        self.current_s = self.base_s
        self.next_ts = 0.0

    def fail(self) -> None:
        self.current_s = min(self.max_s, self.current_s * 2.0)
        self.next_ts = time.monotonic() + self.current_s


class UnrealLinkService(QObject):
    def __init__(
        self,
        *,
        base_url: str = "http://127.0.0.1:9000",
        uav_id: str = "sim",
        telemetry_hz: float = 6.0,
        detections_hz: float = 1.0,
        camera_hz: float = 8.0,
        camera_mode: str = "h264_stream",
        video_endpoint: str = "/sim/v1/video.ts",
        video_target_fps: float = 15.0,
        video_reconnect_s: float = 1.0,
        on_telemetry: Callable[[object], None] | None = None,
        on_detections: Callable[[object, list[dict[str, Any]]], None] | None = None,
        on_camera_frame: Callable[[QImage], None] | None = None,
        on_link_status: Callable[[str], None] | None = None,
        on_camera_status: Callable[[str], None] | None = None,
        on_camera_info: Callable[[dict[str, Any]], None] | None = None,
        on_map_ready: Callable[[str, MapBounds], None] | None = None,
        on_warning: Callable[[str], None] | None = None,
    ) -> None:
        super().__init__()
        self.base_url = base_url.rstrip("/")
        self.uav_id = uav_id
        self._on_telemetry = on_telemetry
        self._on_detections = on_detections
        self._on_camera_frame = on_camera_frame
        self._on_link_status = on_link_status
        self._on_camera_status = on_camera_status
        self._on_camera_info = on_camera_info
        self._on_map_ready = on_map_ready
        self._on_warning = on_warning
        self._client = httpx.Client(
            timeout=2.5,
            limits=httpx.Limits(max_keepalive_connections=8, max_connections=16),
        )

        self._telemetry_timer = QTimer(self)
        self._telemetry_timer.setInterval(max(50, int(1000 / max(1.0, telemetry_hz))))
        self._telemetry_timer.timeout.connect(self._poll_telemetry)

        self._detections_timer = QTimer(self)
        self._detections_timer.setInterval(max(200, int(1000 / max(0.5, detections_hz))))
        self._detections_timer.timeout.connect(self._poll_detections)
        self._detections_enabled = float(detections_hz) > 0.0

        self._camera_timer = QTimer(self)
        self._camera_timer.setInterval(max(50, int(1000 / max(1.0, camera_hz))))
        self._camera_timer.timeout.connect(self._poll_camera)
        self._camera_hz = max(1.0, float(camera_hz))

        self._camera_mode = camera_mode
        self._video_endpoint = video_endpoint
        self._video_target_fps = video_target_fps
        self._video_reconnect_s = video_reconnect_s
        self._camera_enabled = True
        self._jpeg_worker: _JpegPollWorker | None = None
        self._stream_worker: _H264StreamWorker | None = None

        self._map_timer = QTimer(self)
        self._map_timer.setSingleShot(True)
        self._map_timer.timeout.connect(self._poll_map)

        self._telemetry_backoff = _Backoff()
        self._detections_backoff = _Backoff()
        self._camera_backoff = _Backoff()
        self._map_backoff = _Backoff(base_s=2.0, max_s=10.0)

        self._last_status: str | None = None
        self._last_camera_status: str | None = None
        self._last_camera_error = False
        self._camera_frame_log_window_ts = 0.0
        self._camera_frame_log_count = 0
        self._camera_frame_log_window_s = 5.0
        self._map_ready = False
        self._object_emit_ts: dict[str, float] = {}
        self._camera_suffix_preference = "camera.jpg"
        self._pyav_missing_warned = False
        self._h264_disconnect_count = 0
        self._h264_fallback_threshold = 3
        self._h264_runtime_fallback_warned = False
        self._camera_info_fetched = False

    def start(self) -> None:
        self._telemetry_timer.start()
        if self._detections_enabled:
            self._detections_timer.start()
        else:
            log.info("Unreal detections polling disabled (detections_hz <= 0)")
        self._start_camera_pipeline()
        self._map_timer.start(0)

    def stop(self) -> None:
        self._telemetry_timer.stop()
        self._detections_timer.stop()
        self._camera_timer.stop()
        self._map_timer.stop()
        self._stop_jpeg_worker()
        self._stop_stream_worker()
        self._client.close()

    def set_camera_enabled(self, enabled: bool) -> None:
        self._camera_enabled = bool(enabled)
        if self._camera_mode == "h264_stream":
            if self._stream_worker is not None:
                self._stream_worker.set_enabled(self._camera_enabled)
        else:
            if self._jpeg_worker is not None:
                self._jpeg_worker.set_enabled(self._camera_enabled)
            elif self._camera_enabled and not self._camera_timer.isActive():
                # Legacy fallback if worker failed to start.
                self._camera_timer.start()
            elif not self._camera_enabled:
                self._camera_timer.stop()

    def set_camera_mode(self, mode: str) -> None:
        normalized = str(mode or "").strip().lower()
        if normalized not in ("jpeg_snapshots", "h264_stream"):
            return
        if normalized == self._camera_mode:
            return
        log.info("Unreal camera mode switch: %s -> %s", self._camera_mode, normalized)
        self._camera_mode = normalized
        self._h264_disconnect_count = 0
        self._h264_runtime_fallback_warned = False
        self._start_camera_pipeline()

    # ----------------------------- internal setup ----------------------------- #
    def _start_camera_pipeline(self) -> None:
        requested_mode = self._camera_mode
        log.info(
            "Restart Unreal camera pipeline (requested_mode=%s enabled=%s)",
            requested_mode,
            self._camera_enabled,
        )
        self._camera_timer.stop()
        self._stop_jpeg_worker()
        self._stop_stream_worker()
        self._h264_disconnect_count = 0
        if self._camera_mode == "h264_stream":
            if av is None:
                msg = "Unreal video mode 'h264_stream' requested, but PyAV is not installed; falling back to JPEG snapshots"
                self._set_camera_status("pyav_missing_jpeg_fallback")
                if not self._pyav_missing_warned:
                    log.warning(msg)
                    self._pyav_missing_warned = True
                    if self._on_warning:
                        try:
                            self._on_warning(msg)
                        except Exception:  # noqa: BLE001
                            pass
                self._camera_mode = "jpeg_snapshots"
            else:
                url = f"{self.base_url}{self._video_endpoint}"
                log.info("start h264 stream mode: %s (jpeg polling disabled)", url)
                self._stream_worker = _H264StreamWorker(
                    url=url,
                    target_fps=self._video_target_fps,
                    reconnect_s=self._video_reconnect_s,
                )
                self._stream_worker.frameReady.connect(self._handle_stream_frame)
                self._stream_worker.statusChanged.connect(self._handle_stream_status)
                self._stream_worker.set_enabled(self._camera_enabled)
                self._stream_worker.start()
                log.info("Unreal camera pipeline active mode=h264_stream (requested=%s)", requested_mode)
                return
        log.info("start jpeg polling mode: %s/sim/v1/camera.jpg", self.base_url)
        self._jpeg_worker = _JpegPollWorker(
            base_url=self.base_url,
            target_hz=self._camera_hz,
        )
        self._jpeg_worker.frameReady.connect(self._handle_jpeg_frame)
        self._jpeg_worker.statusChanged.connect(self._handle_jpeg_status)
        self._jpeg_worker.set_enabled(self._camera_enabled)
        self._jpeg_worker.start()
        log.info("Unreal camera pipeline active mode=jpeg_snapshots (requested=%s)", requested_mode)

    def _stop_jpeg_worker(self) -> None:
        if self._jpeg_worker is None:
            return
        log.info("stop jpeg polling")
        try:
            self._jpeg_worker.stop()
            if not self._jpeg_worker.wait(3000):
                log.warning("JPEG polling worker did not stop within timeout")
                self._jpeg_worker.terminate()
                if not self._jpeg_worker.wait(1500):
                    log.error("JPEG polling worker did not terminate cleanly")
        except Exception:  # noqa: BLE001
            pass
        self._jpeg_worker = None

    def _stop_stream_worker(self) -> None:
        if self._stream_worker is None:
            return
        log.info("stop h264 stream")
        try:
            self._stream_worker.stop()
            if not self._stream_worker.wait(5000):
                log.warning("H264 stream worker did not stop within timeout")
                self._stream_worker.terminate()
                if not self._stream_worker.wait(2000):
                    log.error("H264 stream worker did not terminate cleanly")
        except Exception:  # noqa: BLE001
            pass
        self._stream_worker = None

    def _handle_jpeg_frame(self, img: QImage) -> None:
        if self.sender() is not self._jpeg_worker:
            return
        self._emit_camera_frame(img)
        self._set_camera_status("streaming")

    def _handle_jpeg_status(self, status: str) -> None:
        if self.sender() is not self._jpeg_worker:
            return
        self._set_camera_status(status)

    def _handle_stream_frame(self, img: QImage) -> None:
        if self.sender() is not self._stream_worker:
            return
        self._emit_camera_frame(img)
        self._set_camera_status("streaming")

    def _handle_stream_status(self, status: str) -> None:
        if self.sender() is not self._stream_worker:
            return
        # Count both hard failures and "server not ready" (503) as H264 errors.
        # A persistent 503 from Unreal means the H264 encoder isn't initialised;
        # JPEG snapshots use a separate code path and may still work.
        if status in ("disconnected", "waiting_for_route") and self._camera_enabled:
            self._h264_disconnect_count += 1
            if self._h264_disconnect_count >= self._h264_fallback_threshold:
                self._fallback_to_jpeg_after_h264_errors()
                return
        elif status == "streaming":
            self._h264_disconnect_count = 0
        self._set_camera_status(status)

    def _fallback_to_jpeg_after_h264_errors(self) -> None:
        msg = (
            "Unreal H264 stream is not opening after repeated attempts; "
            "falling back to JPEG snapshots"
        )
        if not self._h264_runtime_fallback_warned:
            log.warning(msg)
            self._h264_runtime_fallback_warned = True
            if self._on_warning:
                try:
                    self._on_warning(msg)
                except Exception:  # noqa: BLE001
                    pass
        self._camera_mode = "jpeg_snapshots"
        self._set_camera_status("h264_runtime_jpeg_fallback")
        self._start_camera_pipeline()

    # ----------------------------- public API ----------------------------- #
    def send_route(self, route: dict) -> bool:
        try:
            resp = self._client.post(f"{self.base_url}/sim/v1/route", json=route)
            resp.raise_for_status()
            return True
        except Exception as exc:  # noqa: BLE001
            log.warning("Unreal route send failed: %s", exc)
            return False

    def send_command(self, command: str, payload: dict | None = None) -> bool:
        body: dict[str, Any] = {"uav_id": self.uav_id, "type": str(command), "command": str(command)}
        if payload:
            body.update(payload)
        try:
            resp = self._client.post(f"{self.base_url}/sim/v1/command", json=body)
            resp.raise_for_status()
            return True
        except Exception as exc:  # noqa: BLE001
            log.warning("Unreal command send failed: %s", exc)
            return False

    # ----------------------------- polling ----------------------------- #
    def _poll_map(self) -> None:
        if self._map_ready:
            return
        if not self._map_backoff.allow():
            self._map_timer.start(int(self._map_backoff.current_s * 1000))
            return
        try:
            info = self._client.get(f"{self.base_url}/sim/v1/map_info").json()
            bounds = self._extract_bounds(info)
            if bounds is None:
                map_info_keys = sorted(info.keys()) if isinstance(info, dict) else []
                raw_bounds = info.get("bounds") if isinstance(info, dict) else None
                bounds_keys = sorted(raw_bounds.keys()) if isinstance(raw_bounds, dict) else []
                log.error(
                    "Unreal map_info invalid or missing bounds; map_info_keys=%s bounds_keys=%s payload=%s",
                    map_info_keys,
                    bounds_keys,
                    json.dumps(info, default=str),
                )
                self._map_backoff.fail()
                self._map_timer.start(int(self._map_backoff.current_s * 1000))
                return
            img_resp = self._client.get(f"{self.base_url}/sim/v1/map.png", timeout=5.0)
            img_resp.raise_for_status()
            cache_dir = Path(tempfile.gettempdir()) / "fire_uav"
            cache_dir.mkdir(parents=True, exist_ok=True)
            map_path = cache_dir / "unreal_map.png"
            map_path.write_bytes(img_resp.content)
            self._map_ready = True
            self._map_backoff.success()
            if self._on_map_ready:
                self._on_map_ready(str(map_path), bounds)
            log.info("Unreal map snapshot loaded from %s", map_path)
            return
        except Exception as exc:  # noqa: BLE001
            if not self._map_ready:
                log.debug("Unreal map fetch failed: %s", exc)
            self._map_backoff.fail()
            self._map_timer.start(int(self._map_backoff.current_s * 1000))

    def _poll_telemetry(self) -> None:
        if not self._telemetry_backoff.allow():
            return
        url = f"{self.base_url}/sim/v1/telemetry"
        try:
            resp = self._client.get(url)
            if resp.status_code == 503:
                payload: dict[str, Any] = {}
                if resp.content:
                    try:
                        parsed = resp.json()
                        if isinstance(parsed, dict):
                            payload = parsed
                    except ValueError:
                        pass
                self._normalize_telemetry_payload(payload)
                payload.setdefault("status", "WAITING_FOR_ROUTE")
                self._set_link_state_waiting_for_route(payload)
                self._telemetry_backoff.success()
                return
            resp.raise_for_status()
            payload = resp.json()
            if not isinstance(payload, dict):
                raise TypeError("Telemetry payload must be an object")
            self._normalize_telemetry_payload(payload)
            msg = TelemetryMessage(**payload)
            sample = telemetry_sample_from_message(msg)
            sample.source = msg.uav_id
            self._maybe_fetch_camera_info()
            self._telemetry_backoff.success()
            self._set_status("connected")
            if self._on_telemetry:
                self._on_telemetry(sample)
        except Exception as exc:  # noqa: BLE001
            self._set_status("disconnected")
            self._telemetry_backoff.fail()
            if self._telemetry_backoff.current_s >= 4.0:
                log.debug("Unreal telemetry poll failed: %s", exc)

    def _poll_detections(self) -> None:
        if not self._detections_enabled:
            return
        if not self._detections_backoff.allow():
            return
        url = f"{self.base_url}/sim/v1/detections"
        try:
            resp = self._client.get(url)
            if resp.status_code == 404:
                self._detections_backoff.success()
                return
            resp.raise_for_status()
            payload = resp.json()
            batch, objects = self._normalize_detections(payload)
            self._detections_backoff.success()
            if self._on_detections:
                self._on_detections(batch, objects)
        except Exception as exc:  # noqa: BLE001
            self._detections_backoff.fail()
            if self._detections_backoff.current_s >= 4.0:
                log.debug("Unreal detections poll failed: %s", exc)

    def _poll_camera(self) -> None:
        # Legacy synchronous polling fallback (kept for compatibility). Main flow uses _JpegPollWorker.
        if self._camera_mode == "h264_stream":
            self._camera_timer.stop()
            return
        if not self._camera_enabled:
            return
        if not self._camera_backoff.allow():
            return
        suffixes = (
            (self._camera_suffix_preference, "camera.png")
            if self._camera_suffix_preference == "camera.jpg"
            else (self._camera_suffix_preference, "camera.jpg")
        )
        for suffix in suffixes:
            try:
                resp = self._client.get(f"{self.base_url}/sim/v1/{suffix}")
                if resp.status_code == 503:
                    self._camera_backoff.success()
                    self._set_camera_status("waiting_for_route")
                    return
                if resp.status_code == 404:
                    continue
                resp.raise_for_status()
                img = QImage.fromData(resp.content)
                if img.isNull():
                    raise RuntimeError("Invalid image data")
                self._camera_suffix_preference = suffix
                self._camera_backoff.success()
                if self._last_camera_error:
                    self._last_camera_error = False
                    log.info("Unreal camera stream recovered")
                self._set_camera_status("streaming")
                self._emit_camera_frame(img)
                return
            except Exception as exc:  # noqa: BLE001
                self._camera_backoff.fail()
                self._set_camera_status("disconnected")
                if not self._last_camera_error:
                    log.debug("Unreal camera poll failed: %s", exc)
                    self._last_camera_error = True
                return

    def _emit_camera_frame(self, img: QImage) -> None:
        if self._on_camera_frame:
            self._on_camera_frame(img)
        now = time.monotonic()
        if self._camera_frame_log_window_ts <= 0.0:
            self._camera_frame_log_window_ts = now
        self._camera_frame_log_count += 1
        elapsed = now - self._camera_frame_log_window_ts
        if elapsed >= self._camera_frame_log_window_s:
            fps = self._camera_frame_log_count / max(elapsed, 1e-3)
            log.info(
                "Unreal camera delivered fps=%.1f over %.1fs (mode=%s)",
                fps,
                elapsed,
                self._camera_mode,
            )
            self._camera_frame_log_window_ts = now
            self._camera_frame_log_count = 0

    # ----------------------------- helpers ----------------------------- #
    def _set_status(self, status: str) -> None:
        if status == self._last_status:
            return
        self._last_status = status
        if self._on_link_status:
            self._on_link_status(status)

    def _set_camera_status(self, status: str) -> None:
        if status == self._last_camera_status:
            return
        self._last_camera_status = status
        if self._on_camera_status:
            self._on_camera_status(status)

    @staticmethod
    def _extract_bounds(payload: dict) -> MapBounds | None:
        if not isinstance(payload, dict):
            return None
        raw = payload.get("bounds") if isinstance(payload.get("bounds"), dict) else None
        for source in (raw, payload):
            if not isinstance(source, dict):
                continue
            bounds = UnrealLinkService._parse_bounds_dict(source)
            if bounds is not None:
                return bounds
        return None

    @staticmethod
    def _parse_bounds_dict(source: dict[str, Any]) -> MapBounds | None:
        def _get_num(d: dict[str, Any], keys: list[str]) -> float | None:
            for key in keys:
                if key in d and d[key] is not None:
                    try:
                        return float(d[key])
                    except (TypeError, ValueError):
                        pass
            return None

        lat_min = _get_num(source, ["lat_min", "latMin", "LatMin"])
        lon_min = _get_num(source, ["lon_min", "lonMin", "LonMin"])
        lat_max = _get_num(source, ["lat_max", "latMax", "LatMax"])
        lon_max = _get_num(source, ["lon_max", "lonMax", "LonMax"])
        if None in (lat_min, lon_min, lat_max, lon_max):
            return None
        return MapBounds(
            lat_min=lat_min,
            lon_min=lon_min,
            lat_max=lat_max,
            lon_max=lon_max,
        )

    @staticmethod
    def _normalize_telemetry_payload(payload: dict[str, Any]) -> None:
        raw_yaw = payload.get("yaw")
        if raw_yaw is None:
            raw_yaw = payload.get("heading", 0.0)
        try:
            payload["yaw"] = float(raw_yaw)
        except (TypeError, ValueError):
            payload["yaw"] = 0.0

    def _maybe_fetch_camera_info(self) -> None:
        if self._camera_info_fetched:
            return
        try:
            resp = self._client.get(f"{self.base_url}/sim/v1/camera_info")
            if resp.status_code >= 400:
                return
            raw = resp.json()
            if not isinstance(raw, dict):
                return
            normalized = self._normalize_camera_info_payload(raw)
            if not normalized:
                return
            self._camera_info_fetched = True
            if self._on_camera_info:
                self._on_camera_info(normalized)
        except Exception:  # noqa: BLE001
            log.debug("Unreal camera_info fetch failed", exc_info=True)

    @staticmethod
    def _normalize_camera_info_payload(payload: dict[str, Any]) -> dict[str, Any]:
        def _f(*keys: str) -> float | None:
            for key in keys:
                if key in payload and payload[key] is not None:
                    try:
                        return float(payload[key])
                    except (TypeError, ValueError):
                        continue
            return None

        out: dict[str, Any] = {}
        fov = _f("fov_deg", "fov", "FOVAngle")
        if fov is not None:
            out["fov_deg"] = fov
        # Unreal reports downward-looking camera pitch as negative values.
        # Python projector expects downward pitch as positive.
        mount_pitch = _f(
            "base_mount_pitch_deg",
            "camera_base_mount_pitch_deg",
            "mount_pitch_deg",
            "camera_mount_pitch_deg",
            "mount_pitch",
        )
        mount_yaw = _f(
            "base_mount_yaw_deg",
            "camera_base_mount_yaw_deg",
            "mount_yaw_deg",
            "camera_mount_yaw_deg",
            "mount_yaw",
        )
        mount_roll = _f(
            "base_mount_roll_deg",
            "camera_base_mount_roll_deg",
            "mount_roll_deg",
            "camera_mount_roll_deg",
            "mount_roll",
        )
        if mount_pitch is not None:
            out["mount_pitch_deg"] = -mount_pitch
        if mount_yaw is not None:
            out["mount_yaw_deg"] = mount_yaw
        if mount_roll is not None:
            out["mount_roll_deg"] = mount_roll
        return out

    def _set_link_state_waiting_for_route(self, payload: dict[str, Any] | None = None) -> None:
        was_waiting = self._last_status == "waiting_for_route"
        self._set_status("waiting_for_route")
        if was_waiting:
            log.debug("Unreal telemetry waiting_for_route")
        else:
            log.info("Unreal telemetry waiting_for_route")

    @staticmethod
    def _class_id_from_name(name: str) -> int:
        if not name:
            return 0
        normalized = str(name).strip().lower()
        if normalized == "fire":
            return 1
        if normalized in ("human", "person"):
            return 2
        return 0

    def _normalize_detections(self, payload: Any) -> tuple[object, list[dict[str, Any]]]:
        items: list[Any] = []
        if isinstance(payload, dict):
            items = payload.get("detections") or payload.get("objects") or []
        elif isinstance(payload, list):
            items = payload
        det_list = []
        objects: list[dict[str, Any]] = []
        class_counts: Counter[int] = Counter()
        source_counts: Counter[str] = Counter()
        now = time.monotonic()
        for item in items:
            if not isinstance(item, dict):
                continue
            class_id = item.get("class_id")
            if class_id is None:
                class_id = item.get("cls")
            if class_id is None:
                class_id = self._class_id_from_name(str(item.get("class", "")))
            try:
                class_id = int(class_id)
            except Exception:  # noqa: BLE001
                class_id = self._class_id_from_name(str(item.get("class", "")))
            class_counts[class_id] += 1
            bbox = item.get("bbox")
            if bbox is None and all(k in item for k in ("x1", "y1", "x2", "y2")):
                bbox = [item.get("x1"), item.get("y1"), item.get("x2"), item.get("y2")]
            det = SimpleNamespace(
                bbox=tuple(bbox) if bbox else None,
                x1=item.get("x1"),
                y1=item.get("y1"),
                x2=item.get("x2"),
                y2=item.get("y2"),
                confidence=item.get("confidence", item.get("score", 0.0)),
                class_id=class_id,
            )
            det_list.append(det)
            if "lat" in item and "lon" in item:
                try:
                    lat = float(item.get("lat"))
                    lon = float(item.get("lon"))
                except (TypeError, ValueError):
                    continue
                source_id = item.get("source_id") or item.get("id") or item.get("object_id")
                source_id = str(source_id).strip() if source_id is not None else ""
                if not source_id:
                    source_id = f"auto:{class_id}:{lat:.6f}:{lon:.6f}"
                item["source_id"] = source_id
                source_counts[source_id] += 1
                last_ts = self._object_emit_ts.get(source_id, 0.0)
                if now - last_ts < 5.0:
                    continue
                self._object_emit_ts[source_id] = now
                obj = {
                    "object_id": str(item.get("object_id") or source_id),
                    "source_id": source_id,
                    "class_id": class_id,
                    "confidence": float(item.get("confidence", item.get("score", 0.0))),
                    "lat": lat,
                    "lon": lon,
                    "track_id": item.get("track_id"),
                    "timestamp": item.get("timestamp"),
                }
                objects.append(obj)
        log.debug(
            "Unreal detections normalized: detections=%d objects=%d class_counts=%s top_source_ids=%s",
            len(det_list),
            len(objects),
            dict(class_counts),
            source_counts.most_common(3),
        )
        batch = SimpleNamespace(detections=det_list)
        return batch, objects


__all__ = ["UnrealLinkService"]
