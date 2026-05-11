"""Entry point for the ground-station GUI runtime."""

from __future__ import annotations

import os
import sys

# Avoid stale QML cache so UI tweaks are visible on restart.
os.environ.setdefault("QML_DISABLE_DISK_CACHE", "1")

import PySide6.QtWebEngineQuick  # noqa: F401  # registers QML types
from PySide6.QtGui import QGuiApplication

# QtWebEngine module name differs between PySide6 builds.
try:
    from PySide6.QtWebEngine import QtWebEngine  # PySide6 >= 6.8
except ImportError:
    try:
        from PySide6.QtWebEngineQuick import QtWebEngineQuick as QtWebEngine  # PySide6 <= 6.7
    except ImportError as exc:
        raise RuntimeError(
            "PySide6 WebEngine is missing. Reinstall with Qt WebEngine support (poetry install or pip install PySide6-Addons)."
        ) from exc

import fire_uav.infrastructure.providers as deps
from fire_uav.bootstrap import init_ground_core
from fire_uav.gui.windows.main_window import MainWindow
from fire_uav.config.logging_config import setup_logging
from fire_uav.ground_app.config import load_ground_settings


def main() -> None:  # noqa: D401
    cfg = load_ground_settings()
    setup_logging(cfg)
    init_ground_core()  # создаёт очереди, lifecycle, bus-binding

    QtWebEngine.initialize()
    app = QGuiApplication(sys.argv)

    # Ensure background threads stop when window closes to avoid Qt aborts.
    app.aboutToQuit.connect(lambda: deps.get_lifecycle().stop_all())  # type: ignore[arg-type]
    win = MainWindow(qml_file="additional.qml")
    app.aboutToQuit.connect(lambda: win.stop_services())  # type: ignore[arg-type]

    # регистрируем только реально существующие компоненты
    for comp in (getattr(win, "cam_thr", None), getattr(win, "det_thr", None)):
        if comp is not None:
            deps.get_lifecycle().register(comp)

    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":  # pragma: no cover
    main()
