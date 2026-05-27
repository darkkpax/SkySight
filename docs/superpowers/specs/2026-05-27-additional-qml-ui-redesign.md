# additional.qml UI Redesign

**Date:** 2026-05-27  
**Scope:** `fire_uav/gui/qml/additional.qml` — тактический экран

---

## Цель

Три конкретные проблемы текущего UI:

1. Чипы верхней панели жмутся к левому краю — панель выглядит пустой справа.
2. Левая колонка перегружена кнопками; непонятно, что нажимать в данный момент.
3. Карточки обнаруженных объектов слабоинформативны — нет класса, дистанции, трека, времени.

---

## 1. Верхняя панель — растяжка чипов

**Решение:** добавить `Layout.fillWidth: true` каждому `StatChip` во внутреннем `RowLayout`.

Все 8 чипов (БПЛА, СТАТУС, БАТАРЕЯ, ВЫС, GPS, СВЯЗЬ, БЭКЕНД, ВРЕМЯ) занимают одинаковую долю ширины и равномерно заполняют всю панель.

Изменения только в QML, никакой Python-логики не требуется.

---

## 2. Левая колонка — контекстные кнопки по фазе полёта

### Принцип

Карточка «Управление» показывает разный набор кнопок в зависимости от `missionState`. Всегда видно одно-два главных действия для текущей фазы плюс приглушённые вспомогательные.

Комбо-бокс «быстрых действий» убирается; все его действия становятся прямыми кнопками в нужных фазах.

### Маппинг фаз → кнопки

| Фаза | Основные (акцент/предупреждение) | Вторичные (accent) | Утилиты (приглушённые) |
|---|---|---|---|
| **PREFLIGHT** | ✓ Подтвердить (green) | Маршрут | Детектор, Сохранить |
| **READY** | ✓ Подтвердить (green) | Маршрут, Орбита | Детектор, Сохранить |
| **IN_FLIGHT** | Орбита, Орбита всех\* | Возврат (warn), Отправить RTL, Посадка, Предполёт (abort) | Детектор, Сохранить |
| **RTL** | — | Посадка | Детектор, Сохранить |
| **POSTFLIGHT** | Назад к планированию (green) | — | Детектор, Сохранить |

\* «Орбита всех» видна только если `confirmedObjectCount > 1`.

### Подсказка-строка

Под кнопками — одна строка с пояснением текущего состояния (`textMuted`, 11px):
- PREFLIGHT: "Постройте маршрут и подтвердите план перед вылетом"
- READY: "План подтверждён. Можно начинать"
- IN_FLIGHT: "Дрон в воздухе. Выберите цель или инициируйте возврат"
- RTL: "Возврат на базу..."
- POSTFLIGHT: "Миссия завершена"

### Enabled-логика

Кнопки используют существующие `app.canConfirmPlan`, `app.canOpenOrbit`, `app.canRtl`, `app.canSendRtlRoute`, `app.canCompleteLanding`, `app.canAbortToPreflight` и проверку `isPostflight` — ничего нового в Python не добавляется.

---

## 3. Карточки объектов — расширенная информация

### Новые поля на карточке

| Поле | Источник | Примечание |
|---|---|---|
| Цветной тег класса | `modelData.class_id` + функция `classTagInfo(id)` | 🔥 ОГОНЬ / 💨 ДЫМ / 🧍 ЧЕЛОВЕК / ❓ КЛАСС N |
| Расстояние от дрона | QML: haversine от `app.uavStates[0]` до `modelData.lat/lon` | Обновляется при `uavStatesChanged` |
| Номер трека | `modelData.track_id` | Уже есть в данных |
| Время с обнаружения | QML: `property var seenAt: ({})`, заполняется при появлении объекта | Таймер 1 с обновляет `elapsedText` |
| Бар уверенности | `modelData.confidence` | Цвет совпадает с цветом тега класса |
| Кнопка Орбита | `app.selectConfirmedObject` + `app.orbitConfirmedObject` | Только если `app.canOpenOrbit` |

### Функция classTagInfo

```qml
function classTagInfo(classId) {
    if (classId === 0) return { label: "ОГОНЬ",   icon: "🔥", fill: "#3d1800", border: "#ff6a30", text: "#ff9a72" }
    if (classId === 1) return { label: "ДЫМ",     icon: "💨", fill: "#1e1e28", border: "#7878a0", text: "#c0c8d8" }
    if (classId === 2) return { label: "ЧЕЛОВЕК", icon: "🧍", fill: "#0d2414", border: "#3db060", text: "#90e8a8" }
    return { label: "КЛАСС " + classId, icon: "❓", fill: "#1a1a1a", border: "#555", text: "#9aa7b5" }
}
```

### Отслеживание времени в QML

```qml
property var _seenAt: ({})   // object_id → timestamp ms

// в Connections onConfirmedObjectsChanged:
var objs = app.confirmedObjects
for (var i = 0; i < objs.length; i++) {
    var id = objs[i].object_id
    if (!_seenAt[id]) {
        var m = {}; Object.assign(m, _seenAt); m[id] = Date.now(); _seenAt = m
    }
}
// Timer 1000ms: обновляет seenAt чтобы делегаты перерисовывались
```

### Функция elapsedText(ms)

```qml
function elapsedText(ms) {
    var s = Math.floor((Date.now() - ms) / 1000)
    if (s < 60) return s + " с"
    if (s < 3600) return Math.floor(s / 60) + " мин"
    return Math.floor(s / 3600) + " ч"
}
```

### Расстояние (QML haversine)

```qml
function distanceM(lat1, lon1, lat2, lon2) {
    var R = 6371000
    var dLat = (lat2 - lat1) * Math.PI / 180
    var dLon = (lon2 - lon1) * Math.PI / 180
    var a = Math.sin(dLat/2)*Math.sin(dLat/2)
          + Math.cos(lat1*Math.PI/180)*Math.cos(lat2*Math.PI/180)
          * Math.sin(dLon/2)*Math.sin(dLon/2)
    return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a))
}
```

---

## Что не меняется

- Карточка «Миссия» (статус, fps, задержка, доверие, резерв) — без изменений
- Карточка «Маршрут / Точки» (режимы draw/edit/view, тоггл параметров орбиты) — без изменений
- Журнал системы — без изменений
- Карта, видео, оверлеи — без изменений
- Все Python-бэкенд слоты и сигналы — без изменений

---

## Затронутые файлы

| Файл | Изменения |
|---|---|
| `fire_uav/gui/qml/additional.qml` | Верхняя панель, карточка управления, карточка объектов |
