# SkySight — UAV Fire & Smoke Detection System

[![CI](https://github.com/darkkpax/fire-uav/actions/workflows/ci.yml/badge.svg)](https://github.com/darkkpax/fire-uav/actions/workflows/ci.yml)
[![Python](https://img.shields.io/badge/python-3.11%20%7C%203.12-blue)](https://www.python.org/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Система обнаружения пожаров, дыма и людей с борта БПЛА в реальном времени. Включает наземную станцию управления с интерактивной картой, бортовой модуль с нейросетевой детекцией и симулятор на Unreal Engine 5.

---

## Содержание

- [Возможности](#возможности)
- [Архитектура](#архитектура)
- [Быстрый старт](#быстрый-старт)
- [Запуск](#запуск)
- [Unreal Engine симулятор](#unreal-engine-симулятор)
- [Конфигурация](#конфигурация)
- [Структура проекта](#структура-проекта)
- [Разработка](#разработка)
- [Развёртывание на ARM / Jetson](#развёртывание-на-arm--jetson)

---

## Возможности

| Компонент | Описание |
|---|---|
| **YOLOv11** | Детекция пожара, дыма и людей на кадрах с камеры дрона |
| **Геопроекция** | Пересчёт bbox в мировые координаты (WGS-84) с учётом крена/тангажа/курса |
| **K/N агрегация** | Подтверждение цели только после N показаний в M кадрах — фильтрация ложных срабатываний |
| **Планировщик маршрутов** | Grid lawn-mower + TSP (OR-Tools) + энергомодель с учётом заряда батареи |
| **Облёт целей** | Умный орбитальный манёвр: тангенциальный вход, обход нескольких целей одним маршрутом |
| **Наземная станция** | GUI на PySide6/Qt Quick с картой (OpenLayers), видеопотоком и маркерами объектов |
| **C++ ядро** | Опциональные pybind11-ускорения: геопроекция, трекер bbox, энергомодель |
| **REST API + метрики** | FastAPI + Prometheus, защита токеном |
| **Unreal Engine 5** | Полноценный симулятор с физической моделью БПЛА, дымом и огнём |

---

## Архитектура

```
┌─────────────────────────────────────────────────────────────────┐
│                      БОРТ (module role)                         │
│                                                                 │
│  Камера ──► YOLO ──► BBoxSmoother ──► GeoProjector             │
│                                            │                    │
│                                       Aggregator               │
│                                      (K/N voting)              │
│                                            │                    │
│                                     TargetTracker              │
│                                            │                    │
│                                     ObjectRegistry             │
│                                            │                    │
│           RoutePlanner ◄──────── Confirmed Detections          │
│         (Grid+TSP+Orbit)                   │                    │
│                                       Transmitter ──► Ground   │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                   ЗЕМЛЯ (ground role)                           │
│                                                                 │
│  REST API ──► EventBus ──► ConfirmedObjectsStore               │
│                                  │                              │
│              Qt/QML GUI ◄────────┤                              │
│           ┌──────────────────────┤                              │
│           │  Карта (OpenLayers)  │  Видеопоток                  │
│           │  Маркеры объектов    │  Bbox-оверлей                │
│           │  Планировщик        │  Логи / телеметрия            │
│           └──────────────────────┘                              │
└─────────────────────────────────────────────────────────────────┘
```

---

## Быстрый старт

### Требования

- Python 3.11 или 3.12
- Poetry (рекомендуется) или pip
- Для GUI: Windows или Linux с Qt WebEngine
- Для детекции: CUDA-совместимая видеокарта (опционально, работает и на CPU)

### Установка

**Способ 1 — скриптом (рекомендуется)**

```powershell
# Windows
powershell -ExecutionPolicy Bypass -File scripts/setup_env.ps1
```

```bash
# macOS / Linux
bash scripts/setup_env.sh
```

Скрипт создаст `.venv`, установит все зависимости через Poetry и проверит Qt WebEngine.

**Способ 2 — вручную через pip**

```bash
# Только бортовой модуль (без GUI)
pip install -e ".[module,detect]"

# Полная установка с GUI и детекцией
pip install -e ".[ground,module,detect,dev]"
```

> **Примечание по av:** используйте `pip install av==15.1.0`, а **не** `pip install pyav` — это разные пакеты. `av` нужен для H.264-стриминга из Unreal Engine.

### Модель YOLOv11

Положите файл модели в `data/models/best_yolo11.pt`. Путь можно изменить в `settings_default.json` (ключ `yolo_model`).

---

## Запуск

### Наземная станция (GUI)

```powershell
poetry run python -m fire_uav.main
```

Запускает десктопный интерфейс: карта, видеопоток с дрона, панель управления полётом, маркеры обнаруженных объектов.

### Бортовой модуль (headless)

```powershell
# Windows PowerShell
$env:FIRE_UAV_ROLE = "module"
poetry run python -m fire_uav.main
```

```bash
# Linux / macOS
FIRE_UAV_ROLE=module poetry run python -m fire_uav.main
```

Запускает только бортовое ядро: телеметрия, детекция, планировщик, передача на землю. Без GUI.

### REST API

```bash
poetry run uvicorn fire_uav.api.main_rest:app --host 0.0.0.0 --port 8000
```

Основные эндпоинты:

| Метод | Путь | Описание |
|---|---|---|
| `POST` | `/api/detections` | Принять батч детекций с телеметрией |
| `POST` | `/api/camera/start` | Запустить камеру |
| `GET` | `/api/status` | Статус системы |
| `GET` | `/plan` | Текущий маршрут |
| `GET` | `/metrics` | Prometheus метрики |

Защита API: установите `FIRE_UAV_API_TOKEN` в окружении — все эндпоинты потребуют `X-API-Key` или `Authorization: Bearer`.

### Проверка окружения

```bash
python -m fire_uav.tools.env_check --profile module
python -m fire_uav.tools.env_check --profile module,detect
```

---

## Unreal Engine симулятор

Директория `unrealbridge/` содержит исходный код проекта симулятора **SkySight** на Unreal Engine 5 (без тяжёлых ассетов — только C++ и Blueprint исходники).

```
unrealbridge/
├── SkySight.uproject       # Дескриптор проекта UE5
├── SkySight.sln            # Visual Studio solution
├── Source/                 # C++ исходники симулятора
└── native_core/            # Нативные расширения (pybind11)
```

Симулятор предоставляет HTTP API, которое Python-адаптер использует для получения телеметрии и управления дроном:

| Эндпоинт | Описание |
|---|---|
| `GET /sim/v1/telemetry` | Позиция, ориентация, заряд батареи БПЛА |
| `GET /sim/v1/video.ts` | H.264 видеопоток с камеры |
| `POST /sim/v1/route` | Загрузить маршрут для автопилота |
| `POST /sim/v1/command` | Команды управления |

**Без Unreal Engine** — заглушка для локальной разработки:

```bash
UNREAL_BRIDGE_PORT=9000 python scripts/unreal_bridge_stub.py
```

Реализует тот же API с синтетической телеметрией. Позволяет полностью тестировать пайплайн детекции и планировщик без запуска движка.

Настройте в `settings_default.json`:

```json
{
  "uav_backend": "unreal",
  "unreal_base_url": "http://127.0.0.1:9000"
}
```

---

## Конфигурация

Все параметры — в [`fire_uav/config/settings_default.json`](fire_uav/config/settings_default.json). Переопределение через переменную окружения `FIRE_UAV_SETTINGS` (путь к своему JSON) или `FIRE_UAV_PROFILE` (`dev` / `demo` / `jetson`).

### Ключевые параметры

```json
{
  "role": "ground",
  "uav_backend": "unreal",
  "unreal_base_url": "http://127.0.0.1:9000",

  "yolo_model": "data/models/best_yolo11.pt",
  "yolo_conf": 0.15,

  "agg_window": 3,
  "agg_votes_required": 1,
  "agg_min_confidence": 0.4,

  "orbit_radius_m": 50.0,
  "cruise_speed_mps": 12.0,
  "battery_wh": 77.0,
  "min_return_percent": 20.0,

  "dedup_geo_distance_m": 80.0,
  "object_registry_match_radius_m": 80.0
}
```

### Профили

| Профиль | Лог-уровень | Визуализатор | Native core |
|---|---|---|---|
| `dev` | DEBUG | вкл | выкл |
| `demo` | INFO | вкл | выкл |
| `jetson` | INFO | выкл | вкл |

### Бэкенды БПЛА

| `uav_backend` | Описание |
|---|---|
| `unreal` | Unreal Engine симулятор по HTTP |
| `stub` | Фиктивная телеметрия (для тестов без дрона) |
| `mavlink` | Реальный дрон через MAVLink |
| `custom` | Внешний SDK через integration\_service |

---

## Структура проекта

```
fire_uav/
├── api/                    # FastAPI REST эндпоинты
├── config/                 # Настройки, settings_default.json
├── core/                   # Протоколы, telemetry helpers
├── ground_app/             # Точка входа наземной станции
├── gui/                    # PySide6 / Qt Quick интерфейс
│   ├── qml/                # QML разметка
│   ├── viewmodels/         # Python ↔ QML bridge
│   └── windows/            # Главное окно + контроллер полёта
├── module_app/             # Точка входа бортового модуля
├── module_core/            # Ядро: детекция, маршруты, геометрия
│   ├── detections/         # Pipeline, aggregator, registry
│   ├── fusion/             # GeoProjector (Python)
│   └── route/              # Планировщик, энергомодель, манёвры
├── services/               # EventBus, хранилища, трансмиттер
└── utils/

unrealbridge/               # Unreal Engine 5 проект (без ассетов)
cpp/native_core/            # C++ pybind11 ускорения
integration_service/        # Брокер для внешних SDK
scripts/                    # setup_env, unreal_bridge_stub
docs/                       # Документация
tests/                      # Pytest тесты
```

---

## Разработка

### Тесты

```bash
pytest
pytest --cov=fire_uav --cov-report=term-missing
```

### Линтер

```bash
ruff check fire_uav/
ruff check fire_uav/ --fix   # автоисправление
```

### Полезные команды (Makefile)

```bash
make install            # установить зависимости
make test               # pytest
make lint               # ruff
make fmt                # форматирование (black + ruff)
make run-ground         # запустить GUI
make run-module-unreal  # запустить бортовой модуль с Unreal
make run-bridge         # запустить стаб Unreal Bridge
```

### C++ Native core (опционально)

Даёт прирост производительности для геопроекции, трекера bbox и энергомодели. Без него всё работает на Python.

```bash
cd cpp/native_core
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# скопировать собранный .so рядом с fire_uav/module_core/
```

Включение: `"use_native_core": true` в `settings_default.json`.

Зависимости для сборки: `cmake`, `libpython3-dev`, `pybind11`.

---

## Развёртывание на ARM / Jetson

Подробный гайд: [`docs/DEPLOYMENT_ARM.md`](docs/DEPLOYMENT_ARM.md)

### Сборка Docker-образа

```bash
# NVIDIA Jetson (L4T PyTorch)
docker buildx build --platform linux/arm64 \
  --build-arg BASE_IMAGE=nvcr.io/nvidia/l4t-pytorch:r35.4.1-pth2.3-py3 \
  --build-arg INSTALL_TORCH=0 \
  -t fire-uav:jetson .

# Rockchip / CPU arm64
docker buildx build --platform linux/arm64 -t fire-uav:arm64 .
```

### Запуск контейнера

```bash
# Jetson с GPU
docker run --rm --network host --runtime nvidia --gpus all \
  -e FIRE_UAV_ROLE=module -e FIRE_UAV_PROFILE=jetson \
  fire-uav:jetson

# CPU
docker run --rm --network host \
  -e FIRE_UAV_ROLE=module -e FIRE_UAV_PROFILE=dev \
  fire-uav:arm64
```

### Автозапуск через systemd

```bash
sudo cp scripts/fire_uav.service.example /etc/systemd/system/fire_uav.service
sudo systemctl enable --now fire_uav.service
```

---

## CI / CD

GitHub Actions:

- **[CI](.github/workflows/ci.yml)** — каждый push/PR в `main`/`develop`: lint (ruff) + тесты на Python 3.11 и 3.12
- **[Release](.github/workflows/release.yml)** — на тег `v*.*.*`: тесты → wheel (Linux) + `.exe` (Windows PyInstaller) → GitHub Release

```bash
# Создать релиз
git tag v1.0.0 && git push origin v1.0.0
```

---

## Документация

| Файл | Описание |
|---|---|
| [`docs/HOW_IT_WORKS.md`](docs/HOW_IT_WORKS.md) | Архитектура, потоки данных, компоненты |
| [`docs/operator_workflow.md`](docs/operator_workflow.md) | Инструкция для оператора |
| [`docs/DEPLOYMENT_ARM.md`](docs/DEPLOYMENT_ARM.md) | Развёртывание на ARM / Jetson |
| [`docs/contract_v1.md`](docs/contract_v1.md) | REST API контракт v1 |
| [`docs/unreal_visualizer_api.md`](docs/unreal_visualizer_api.md) | API Unreal Engine симулятора |
| [`docs/how_to_add_new_drone.md`](docs/how_to_add_new_drone.md) | Подключение нового типа БПЛА |
