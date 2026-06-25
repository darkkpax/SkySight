# быстрый старт

этот документ описывает минимальный путь от клонирования репозитория до запуска наземной станции, бортового модуля и локального стаба Unreal Bridge.

## 1. требования

- Python 3.10 или новее;
- Git;
- Poetry, если используется poetry-запуск;
- Windows или Linux для графической наземной станции;
- CUDA-совместимая видеокарта желательно, но не обязательно;
- Unreal Engine 5.x только для полноценного симулятора.

## 2. установка

### вариант через скрипт

```powershell
powershell -ExecutionPolicy Bypass -File scripts/setup_env.ps1
```

```bash
bash scripts/setup_env.sh
```

### вариант через pip

```bash
python -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install -e ".[ground,module,detect,dev]"
```

для Windows:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -U pip
pip install -e ".[ground,module,detect,dev]"
```

## 3. модель

положите веса модели в:

```text
data/models/best_yolo11.pt
```

или укажите свой путь в `fire_uav/config/settings_default.json`:

```json
{
  "yolo_model": "data/models/my_model.pt"
}
```

## 4. запуск без Unreal Engine

для локальной разработки можно запустить стаб, который имитирует API симулятора:

```bash
UNREAL_BRIDGE_PORT=9000 python scripts/unreal_bridge_stub.py
```

после этого запускается наземная станция:

```bash
poetry run python -m fire_uav.main
```

или без Poetry:

```bash
python -m fire_uav.main
```

## 5. запуск бортового модуля

```bash
FIRE_UAV_ROLE=module python -m fire_uav.main
```

Windows PowerShell:

```powershell
$env:FIRE_UAV_ROLE = "module"
python -m fire_uav.main
```

## 6. запуск REST API

```bash
uvicorn fire_uav.api.main_rest:app --host 0.0.0.0 --port 8000
```

если нужен токен:

```bash
export FIRE_UAV_API_TOKEN="change-me"
```

после этого запросы должны передавать один из заголовков:

```text
X-API-Key: change-me
Authorization: Bearer change-me
```

## 7. проверка окружения

```bash
python -m fire_uav.tools.env_check --profile module
python -m fire_uav.tools.env_check --profile module,detect
```

## 8. типовые проблемы

| проблема | что проверить |
|---|---|
| GUI не открывается | установлен ли PySide6 и Qt WebEngine |
| модель не найдена | существует ли файл из `yolo_model` |
| детекция идёт на CPU | доступна ли CUDA и корректно ли установлен PyTorch |
| нет телеметрии | запущен ли Unreal Bridge или стаб на нужном порту |
| API отвечает 401 | задан ли правильный `FIRE_UAV_API_TOKEN` |
