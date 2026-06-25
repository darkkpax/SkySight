# API

SkySight использует REST API для обмена детекциями, телеметрией, статусом и маршрутами между компонентами.

## базовый адрес

Локально:

```text
http://127.0.0.1:8000
```

Для Unreal Bridge обычно используется:

```text
http://127.0.0.1:9000
```

## авторизация

Если задана переменная окружения `FIRE_UAV_API_TOKEN`, запросы должны содержать один из заголовков:

```text
X-API-Key: <token>
Authorization: Bearer <token>
```

## основные endpoints SkySight

| метод | путь | назначение |
|---|---|---|
| `POST` | `/api/detections` | принять батч детекций с телеметрией |
| `POST` | `/api/camera/start` | запустить камеру |
| `GET` | `/api/status` | получить состояние системы |
| `GET` | `/plan` | получить текущий маршрут |
| `GET` | `/metrics` | Prometheus-метрики |

## пример отправки детекций

```bash
curl -X POST http://127.0.0.1:8000/api/detections \
  -H "Content-Type: application/json" \
  -d '{
    "frame": {
      "camera_id": "cam0",
      "width": 1920,
      "height": 1080
    },
    "detections": [
      {
        "camera_id": "cam0",
        "class_id": 0,
        "confidence": 0.82,
        "bbox": [420, 240, 620, 520]
      }
    ]
  }'
```

## Unreal Bridge API

| метод | путь | назначение |
|---|---|---|
| `GET` | `/sim/v1/telemetry` | телеметрия БПЛА |
| `GET` | `/sim/v1/video.ts` | H.264 видеопоток |
| `POST` | `/sim/v1/route` | загрузка маршрута |
| `POST` | `/sim/v1/command` | команда управления |

## рекомендации

- держать API за firewall или VPN;
- использовать токен в production;
- не смешивать API наземной станции и Unreal Bridge на одном порту;
- логировать ошибки обмена;
- для внешних SDK лучше делать отдельный adapter/service, а не менять ядро проекта.
