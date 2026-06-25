# Getting Started

Этот документ описывает быстрый запуск SkySight: установку, подготовку модели, запуск наземной станции, бортового модуля и локального стаба Unreal Bridge.

## требования

- Python 3.10 или новее;
- Git;
- Poetry или pip;
- Windows или Linux для графической наземной станции;
- CUDA-совместимая видеокарта желательно, но не обязательно;
- Unreal Engine 5.x, если нужен полноценный симулятор.

## установка

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_env.ps1
```

### Linux / macOS

```bash
bash scripts/setup_env.sh
```

### вручную через pip

```bash
python -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install -e ".[ground,module,detect,dev]"
```

## модель

По умолчанию проект ожидает веса модели по пути:

```text
data/models/best_yolo11.pt
```

Если используется другой файл, путь нужно изменить в `fire_uav/config/settings_default.json`.

## запуск без Unreal Engine

Для локальной разработки можно использовать стаб симулятора:

```bash
UNREAL_BRIDGE_PORT=9000 python scripts/unreal_bridge_stub.py
```

После этого можно запускать наземную станцию:

```bash
python -m fire_uav.main
```

## запуск бортового модуля

```bash
FIRE_UAV_ROLE=module python -m fire_uav.main
```

Windows PowerShell:

```powershell
$env:FIRE_UAV_ROLE = "module"
python -m fire_uav.main
```

## запуск REST API

```bash
uvicorn fire_uav.api.main_rest:app --host 0.0.0.0 --port 8000
```

Если задан `FIRE_UAV_API_TOKEN`, запросы должны передавать `X-API-Key` или `Authorization: Bearer`.

## проверка окружения

```bash
python -m fire_uav.tools.env_check --profile module
python -m fire_uav.tools.env_check --profile module,detect
```

## типовые проблемы

| проблема | что проверить |
|---|---|
| GUI не открывается | установлен ли PySide6 и Qt WebEngine |
| модель не найдена | существует ли файл из `yolo_model` |
| детекция идёт на CPU | доступна ли CUDA и корректно ли установлен PyTorch |
| нет телеметрии | запущен ли Unreal Bridge или стаб на нужном порту |
| API отвечает 401 | задан ли правильный `FIRE_UAV_API_TOKEN` |
