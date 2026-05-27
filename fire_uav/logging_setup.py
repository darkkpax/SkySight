# mypy: ignore-errors
"""
Единая точка включения логов.
Читаем YAML-конфиг `fire_uav/config/logging.yaml`,
правим путь до data/artifacts/logs/fire_uav_debug.log,
добавляем каталог, и запускаем dictConfig.
"""

from __future__ import annotations

from fire_uav.config.logging_config import setup_logging as _setup_logging
from fire_uav.config.settings import settings


def setup_logging(custom_settings=None) -> None:
    """
    Backwards-compatible wrapper that configures logging using Settings.
    """
    cfg = custom_settings or settings
    _setup_logging(cfg)
