# развёртывание

этот документ описывает развёртывание SkySight на рабочей машине, сервере, Jetson и ARM-устройствах.

## профили

профиль выбирается через `FIRE_UAV_PROFILE`.

| профиль | назначение |
|---|---|
| `dev` | локальная разработка, подробные логи |
| `demo` | демонстрационный запуск |
| `jetson` | запуск на Jetson или похожем edge-устройстве |

роль выбирается через `FIRE_UAV_ROLE`:

```bash
FIRE_UAV_ROLE=ground
FIRE_UAV_ROLE=module
```

## локальный запуск

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e ".[ground,module,detect]"
python -m fire_uav.main
```

## REST API

```bash
uvicorn fire_uav.api.main_rest:app --host 0.0.0.0 --port 8000
```

для защиты API:

```bash
export FIRE_UAV_API_TOKEN="change-me"
```

## Docker для x86_64

```bash
docker build -t fire-uav:local .

docker run --rm --network host \
  -e FIRE_UAV_ROLE=module \
  -e FIRE_UAV_PROFILE=demo \
  fire-uav:local
```

## Jetson

для Jetson лучше использовать L4T/PyTorch образ, чтобы не собирать PyTorch вручную:

```bash
docker buildx build --platform linux/arm64 \
  --build-arg BASE_IMAGE=nvcr.io/nvidia/l4t-pytorch:r35.4.1-pth2.3-py3 \
  --build-arg INSTALL_TORCH=0 \
  -t fire-uav:jetson .
```

запуск:

```bash
docker run --rm --name fire-uav \
  --network host --ipc host \
  --runtime nvidia --gpus all \
  -e FIRE_UAV_ROLE=module \
  -e FIRE_UAV_PROFILE=jetson \
  fire-uav:jetson
```

## ARM / Rockchip / CPU

```bash
docker buildx build --platform linux/arm64 -t fire-uav:arm64 .
```

```bash
docker run --rm --name fire-uav \
  --network host --ipc host \
  -e FIRE_UAV_ROLE=module \
  -e FIRE_UAV_PROFILE=dev \
  fire-uav:arm64
```

## systemd

пример unit-файла лежит в:

```text
scripts/fire_uav.service.example
```

установка:

```bash
sudo cp scripts/fire_uav.service.example /etc/systemd/system/fire_uav.service
sudo systemctl daemon-reload
sudo systemctl enable --now fire_uav.service
```

проверка:

```bash
sudo systemctl status fire_uav.service
journalctl -u fire_uav.service -f
```

## постоянные данные

для реального развёртывания лучше монтировать конфиги и данные отдельно:

```bash
-v /var/lib/fire-uav/config:/app/fire_uav/config/user:ro \
-v /var/lib/fire-uav/data:/app/data
```

## безопасность

- не хранить production-токены в репозитории;
- задавать `FIRE_UAV_API_TOKEN` через окружение;
- закрывать API firewall-ом;
- для внешних подключений использовать VPN или reverse proxy с TLS;
- логировать ошибки, но не сохранять чувствительные ключи.

## проверка после развёртывания

- API отвечает на `/api/status`;
- `/metrics` открывается для Prometheus;
- модель загружается без ошибок;
- телеметрия обновляется;
- видеопоток доступен;
- события появляются в наземной станции;
- systemd перезапускает сервис после падения.
