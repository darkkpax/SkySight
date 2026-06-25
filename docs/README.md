# документация SkySight

эта папка содержит отдельные документы по проекту. Основной `README.md` остаётся витриной проекта, а здесь лежат подробные инструкции для разработки, запуска, оператора и развёртывания.

| документ | описание |
|---|---|
| [`GETTING_STARTED.md`](GETTING_STARTED.md) | установка и первый запуск |
| [`HOW_IT_WORKS.md`](HOW_IT_WORKS.md) | архитектура и поток данных |
| [`UNREAL_ENGINE.md`](UNREAL_ENGINE.md) | симулятор Unreal Engine и его API |
| [`MODEL.md`](MODEL.md) | YOLO-модель, классы, веса, пороги, экспорт |
| [`ADD_NEW_UAV.md`](ADD_NEW_UAV.md) | подключение нового БПЛА или внешнего SDK |
| [`DEPLOYMENT.md`](DEPLOYMENT.md) | Docker, systemd, production-запуск |
| [`DEPLOYMENT_ARM.md`](DEPLOYMENT_ARM.md) | Jetson, Rockchip и ARM |
| [`OPERATOR_GUIDE.md`](OPERATOR_GUIDE.md) | инструкция для оператора |
| [`API.md`](API.md) | REST API и интеграционные endpoints |

## скриншоты

для красивого README рекомендуется добавить реальные изображения в папку:

```text
docs/screenshots/
├── ground-station.png
├── detection-overlay.png
├── unreal-simulator.png
└── route-planner.png
```

после добавления файлов блок скриншотов в основном README начнёт выглядеть как полноценная презентационная секция проекта.
