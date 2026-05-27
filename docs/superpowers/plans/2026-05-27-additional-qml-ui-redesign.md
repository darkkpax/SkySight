# Additional QML UI Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Улучшить тактический экран `additional.qml`: растянуть чипы верхней панели, заменить статичные кнопки управления на контекстные по фазе полёта, переделать карточки объектов с расширенной информацией.

**Architecture:** Все изменения — только в одном QML-файле `fire_uav/gui/qml/additional.qml`. Python-бэкенд не трогается — все нужные слоты и properties уже существуют. Новые QML-хелперы (classTagInfo, distanceM, elapsedText) добавляются как функции в корневой `ApplicationWindow`.

**Tech Stack:** Qt QML 2.15, QtQuick.Layouts 1.15, PySide6

---

## Файлы

- **Modify:** `fire_uav/gui/qml/additional.qml`

---

### Task 1: Растяжка чипов верхней панели

**Files:**
- Modify: `fire_uav/gui/qml/additional.qml:596-645`

- [ ] **Шаг 1: Добавить `Layout.fillWidth: true` ко всем 8 StatChip и убрать spacer**

Найти в файле внутренний `RowLayout` с 8 StatChip (около строки 592). Заменить весь блок от `RowLayout {` (spacing: 10) до закрывающего `Item { Layout.fillWidth: false... }` включительно:

**Было:**
```qml
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    StatChip {
                        chipLabel: "БПЛА"
                        chipValue: root.currentUavLabel()
                        valueColor: textPrimary
                    }

                    StatChip {
                        chipLabel: "СТАТУС"
                        chipValue: hasApp ? root.missionStateRu(app.missionState) : "--"
                        valueColor: root.missionTone()
                    }

                    StatChip {
                        chipLabel: "БАТАРЕЯ"
                        chipValue: hasApp ? app.currentBatteryText.replace("Battery: ", "") : "--"
                        valueColor: hasApp && app.routeBatteryWarning ? "#ff9a86" : textPrimary
                    }

                    StatChip {
                        chipLabel: "ВЫС"
                        chipValue: hasApp ? app.currentAltitudeText : "--"
                        valueColor: textPrimary
                    }

                    StatChip {
                        chipLabel: "GPS"
                        chipValue: hasApp ? app.currentGpsText : "--"
                        valueColor: textPrimary
                    }

                    StatChip {
                        chipLabel: "СВЯЗЬ"
                        chipValue: hasApp ? root.linkStatusRu(app.currentLinkText) : "--"
                        valueColor: hasApp && app.currentLinkText === "OK" ? "#8fe9ad" : textPrimary
                    }

                    StatChip {
                        chipLabel: "БЭКЕНД"
                        chipValue: hasApp ? app.currentBackendText : "--"
                        valueColor: accent
                    }

                    StatChip {
                        chipLabel: "ВРЕМЯ"
                        chipValue: hasApp ? app.currentTimeText : root.currentTimeText
                        valueColor: textPrimary
                    }
                }

                Item { Layout.fillWidth: false; Layout.preferredWidth: 1; Layout.fillHeight: true }
```

**Стало:**
```qml
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "БПЛА"
                        chipValue: root.currentUavLabel()
                        valueColor: textPrimary
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "СТАТУС"
                        chipValue: hasApp ? root.missionStateRu(app.missionState) : "--"
                        valueColor: root.missionTone()
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "БАТАРЕЯ"
                        chipValue: hasApp ? app.currentBatteryText.replace("Battery: ", "") : "--"
                        valueColor: hasApp && app.routeBatteryWarning ? "#ff9a86" : textPrimary
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "ВЫС"
                        chipValue: hasApp ? app.currentAltitudeText : "--"
                        valueColor: textPrimary
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "GPS"
                        chipValue: hasApp ? app.currentGpsText : "--"
                        valueColor: textPrimary
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "СВЯЗЬ"
                        chipValue: hasApp ? root.linkStatusRu(app.currentLinkText) : "--"
                        valueColor: hasApp && app.currentLinkText === "OK" ? "#8fe9ad" : textPrimary
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "БЭКЕНД"
                        chipValue: hasApp ? app.currentBackendText : "--"
                        valueColor: accent
                    }

                    StatChip {
                        Layout.fillWidth: true
                        chipLabel: "ВРЕМЯ"
                        chipValue: hasApp ? app.currentTimeText : root.currentTimeText
                        valueColor: textPrimary
                    }
                }
```

- [ ] **Шаг 2: Проверить запуск**

```powershell
cd k:\Python\git\BPLA_fire_smoke_human-in-the-forest
python -m fire_uav
```

Верхняя панель должна показывать 8 чипов одинаковой ширины на всю ширину окна. Закрыть приложение.

- [ ] **Шаг 3: Коммит**

```bash
git add fire_uav/gui/qml/additional.qml
git commit -m "ui: stretch top panel stat chips to fill full width"
```

---

### Task 2: Вспомогательные функции и трекинг объектов

**Files:**
- Modify: `fire_uav/gui/qml/additional.qml` — добавить после функции `toggleOrbitSelection`

- [ ] **Шаг 1: Добавить properties, функции и таймер после `toggleOrbitSelection`**

Найти в файле:
```qml
    function toggleOrbitSelection(objectId) {
        var next = orbitSelection.slice(0);
        var idx = next.indexOf(objectId);
        if (idx === -1) next.push(objectId);
        else next.splice(idx, 1);
        orbitSelection = next;
    }
```

Добавить сразу после закрывающей скобки этой функции:

```qml
    property var _seenAt: ({})
    property int _elapsedTick: 0

    function classTagInfo(classId) {
        if (classId === 0) return { label: "ОГОНЬ",   icon: "🔥", fill: Qt.rgba(0.24, 0.10, 0.00, 0.72), border: Qt.rgba(1.0, 0.42, 0.18, 0.35), text: "#ff9a72" }
        if (classId === 1) return { label: "ДЫМ",     icon: "💨", fill: Qt.rgba(0.12, 0.12, 0.18, 0.72), border: Qt.rgba(0.60, 0.60, 0.76, 0.30), text: "#c0c8d8" }
        if (classId === 2) return { label: "ЧЕЛОВЕК", icon: "🧍", fill: Qt.rgba(0.05, 0.18, 0.08, 0.72), border: Qt.rgba(0.24, 0.72, 0.38, 0.32), text: "#90e8a8" }
        return { label: "КЛАСС " + classId, icon: "❓", fill: Qt.rgba(0.10, 0.10, 0.10, 0.72), border: Qt.rgba(1, 1, 1, 0.14), text: "#9aa7b5" }
    }

    function distanceM(lat1, lon1, lat2, lon2) {
        if (!lat1 || !lon1 || !lat2 || !lon2) return "--"
        var R = 6371000
        var dLat = (lat2 - lat1) * Math.PI / 180
        var dLon = (lon2 - lon1) * Math.PI / 180
        var a = Math.sin(dLat/2)*Math.sin(dLat/2)
              + Math.cos(lat1*Math.PI/180)*Math.cos(lat2*Math.PI/180)
              * Math.sin(dLon/2)*Math.sin(dLon/2)
        var d = R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a))
        if (d < 1000) return Math.round(d) + " м"
        return (d / 1000).toFixed(1) + " км"
    }

    function elapsedText(ms, _tick) {
        var s = Math.floor((Date.now() - ms) / 1000)
        if (s < 60) return s + " с"
        if (s < 3600) return Math.floor(s / 60) + " мин"
        return Math.floor(s / 3600) + " ч"
    }

    Timer {
        id: elapsedRefreshTimer
        interval: 1000
        running: true
        repeat: true
        onTriggered: root._elapsedTick++
    }

    Connections {
        target: hasApp ? app : null
        function onConfirmedObjectsChanged() {
            if (!hasApp) return
            var objs = app.confirmedObjects
            for (var i = 0; i < objs.length; i++) {
                var oid = objs[i].object_id
                if (root._seenAt[oid] === undefined) {
                    var updated = Object.assign({}, root._seenAt)
                    updated[oid] = Date.now()
                    root._seenAt = updated
                }
            }
        }
    }
```

- [ ] **Шаг 2: Проверить синтаксис (запустить приложение)**

```powershell
python -m fire_uav
```

Приложение должно открыться без ошибок в консоли. Закрыть.

- [ ] **Шаг 3: Коммит**

```bash
git add fire_uav/gui/qml/additional.qml
git commit -m "ui: add classTagInfo, distanceM, elapsedText helpers and seen-at tracker"
```

---

### Task 3: Контекстные кнопки управления по фазе полёта

**Files:**
- Modify: `fire_uav/gui/qml/additional.qml` — блок `actionCardColumn` (~строка 668)

- [ ] **Шаг 1: Заменить содержимое actionCardColumn**

Найти и заменить блок от `GridLayout {` до конца `RowLayout` с combo включительно (весь контент внутри `actionCardColumn` после заголовка):

**Было** (от GridLayout до конца второго RowLayout):
```qml
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 8
                                rowSpacing: 8

                                GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Детектор"; action: function() { if (hasApp) app.startDetector(); } }
                                GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Сохранить"; action: function() { if (hasApp) app.savePlan(); } }

                                GlassButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    label: "Маршрут"
                                    action: function() { root.routePanelMode = "edit"; }
                                }
                                GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Подтвердить"; accentButton: true; enabled: hasApp ? app.canConfirmPlan : false; action: function() { root.requestConfirmPlan(); } }

                                GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Орбита"; enabled: hasApp ? app.canOpenOrbit : false; action: function() { root.requestOrbit(); } }
                                GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Возврат"; warningButton: true; enabled: hasApp ? app.canRtl : false; action: function() { if (hasApp) app.returnToHome(); } }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                CompactComboBox {
                                    id: quickActionCombo
                                    Layout.fillWidth: true
                                    model: [
                                        "Орбита всех", "Отправить маршрут RTL", "Посадка",
                                        "Предполет", "Назад к планированию"
                                    ]
                                }

                                GlassButton {
                                    implicitHeight: 38
                                    implicitWidth: 104
                                    label: "Выполнить"
                                    action: function() {
                                        if (!hasApp) return;
                                        var action = quickActionCombo.currentText;
                                        if (action === "Орбита всех" && app.canOpenOrbit && app.confirmedObjectCount > 1) {
                                            var ids = [];
                                            var objects = app.confirmedObjects;
                                            for (var i = 0; i < objects.length; i++) ids.push(objects[i].object_id);
                                            app.orbitSelectedObjects(ids);
                                        } else if (action === "Отправить маршрут RTL" && app.canSendRtlRoute) app.sendRtlRoute();
                                        else if (action === "Посадка" && app.canCompleteLanding) app.completeLanding();
                                        else if (action === "Предполет" && app.canAbortToPreflight) app.abortToPreflight();
                                        else if (action === "Назад к планированию" && isPostflight) app.backToPlanning();
                                    }
                                }
                            }
```

**Стало:**
```qml
                            Text {
                                Layout.fillWidth: true
                                text: {
                                    if (!hasApp) return "Ожидание подключения"
                                    if (isPreflight || isReady) return "Постройте маршрут и подтвердите план"
                                    if (isInFlight) return "Дрон в воздухе — выберите цель или инициируйте возврат"
                                    if (isRtl) return "Возврат на базу..."
                                    if (isPostflight) return "Миссия завершена"
                                    return ""
                                }
                                color: textMuted
                                font.pixelSize: 11
                                font.family: "Inter"
                                wrapMode: Text.WordWrap
                            }

                            // PREFLIGHT / READY
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: isPreflight || isReady
                                spacing: 8

                                GlassButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 42
                                    label: "Подтвердить"
                                    accentButton: true
                                    enabled: hasApp ? app.canConfirmPlan : false
                                    action: function() { root.requestConfirmPlan() }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Маршрут"
                                        action: function() { root.routePanelMode = "edit" }
                                    }
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Орбита"
                                        enabled: hasApp ? app.canOpenOrbit : false
                                        action: function() { root.requestOrbit() }
                                    }
                                }
                            }

                            // IN_FLIGHT
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: isInFlight
                                spacing: 8

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Орбита"
                                        accentButton: true
                                        enabled: hasApp ? app.canOpenOrbit : false
                                        action: function() { root.requestOrbit() }
                                    }
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Орбита всех"
                                        accentButton: true
                                        enabled: hasApp ? (app.canOpenOrbit && app.confirmedObjectCount > 1) : false
                                        action: function() {
                                            if (!hasApp) return
                                            var ids = []
                                            var objects = app.confirmedObjects
                                            for (var i = 0; i < objects.length; i++) ids.push(objects[i].object_id)
                                            app.orbitSelectedObjects(ids)
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Возврат"
                                        warningButton: true
                                        enabled: hasApp ? app.canRtl : false
                                        action: function() { if (hasApp) app.returnToHome() }
                                    }
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Маршрут RTL"
                                        enabled: hasApp ? app.canSendRtlRoute : false
                                        action: function() { if (hasApp) app.sendRtlRoute() }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Посадка"
                                        enabled: hasApp ? app.canCompleteLanding : false
                                        action: function() { if (hasApp) app.completeLanding() }
                                    }
                                    GlassButton {
                                        Layout.fillWidth: true; implicitHeight: 38; label: "Прервать"
                                        enabled: hasApp ? app.canAbortToPreflight : false
                                        action: function() { if (hasApp) app.abortToPreflight() }
                                    }
                                }
                            }

                            // RTL
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: isRtl
                                spacing: 8

                                GlassButton {
                                    Layout.fillWidth: true; implicitHeight: 38; label: "Посадка"
                                    accentButton: true
                                    enabled: hasApp ? app.canCompleteLanding : false
                                    action: function() { if (hasApp) app.completeLanding() }
                                }
                            }

                            // POSTFLIGHT
                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: isPostflight
                                spacing: 8

                                GlassButton {
                                    Layout.fillWidth: true; implicitHeight: 42; label: "Назад к планированию"
                                    accentButton: true
                                    action: function() { if (hasApp) app.backToPlanning() }
                                }
                            }

                            // Утилиты — всегда видны
                            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Qt.rgba(1, 1, 1, 0.08) }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                GlassButton {
                                    Layout.fillWidth: true; implicitHeight: 34; label: "Детектор"; px: 11
                                    action: function() { if (hasApp) app.startDetector() }
                                }
                                GlassButton {
                                    Layout.fillWidth: true; implicitHeight: 34; label: "Сохранить"; px: 11
                                    action: function() { if (hasApp) app.savePlan() }
                                }
                            }
```

- [ ] **Шаг 2: Проверить приложение**

```powershell
python -m fire_uav
```

В левой колонке должна быть карточка «Управление» с подсказкой и кнопками по текущей фазе. В PREFLIGHT видны «Подтвердить», «Маршрут», «Орбита» + «Детектор», «Сохранить» внизу. Combo-бокса нет. Закрыть.

- [ ] **Шаг 3: Коммит**

```bash
git add fire_uav/gui/qml/additional.qml
git commit -m "ui: contextual flight-phase control buttons, remove combo box"
```

---

### Task 4: Расширенные карточки объектов

**Files:**
- Modify: `fire_uav/gui/qml/additional.qml` — делегат `targetsView` (~строка 1459)

- [ ] **Шаг 1: Заменить делегат ListView targetsView**

Найти блок делегата внутри `targetsView`:

**Было** (весь блок `delegate: Rectangle { ... }` внутри targetsView):
```qml
                                delegate: Rectangle {
                                    property bool hovered: false
                                    width: targetsView.width
                                    radius: 16
                                    color: modelData.selected
                                           ? Qt.rgba(0.22, 0.34, 0.44, hovered ? 0.46 : 0.34)
                                           : (hovered ? Qt.rgba(1, 1, 1, 0.085) : Qt.rgba(1, 1, 1, 0.05))
                                    border.color: modelData.selected
                                                  ? Qt.rgba(0.73, 0.89, 1.0, hovered ? 0.34 : 0.22)
                                                  : Qt.rgba(1, 1, 1, hovered ? 0.14 : 0.06)
                                    border.width: 1
                                    implicitHeight: targetInfo.implicitHeight + 20
                                    clip: true
                                    scale: hovered ? 1.012 : 1.0
                                    Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
                                    Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
                                    Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.50; mass: 0.85 } }

                                    Column {
                                        id: targetInfo
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 4

                                        Text {
                                            text: modelData.object_id
                                            color: textPrimary
                                            font.pixelSize: 13
                                            font.family: "Inter"
                                            font.bold: true
                                        }

                                        Text {
                                            text: (modelData.label || "цель") + " • достоверность " + Number(modelData.confidence || 0).toFixed(2)
                                            color: textMuted
                                            font.pixelSize: 11
                                            font.family: "Inter"
                                        }

                                        Text {
                                            text: "Шир " + Number(modelData.lat || 0).toFixed(5) + "  Долг " + Number(modelData.lon || 0).toFixed(5)
                                            color: "#cfe2ee"
                                            font.pixelSize: 11
                                            font.family: "Inter"
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onEntered: parent.hovered = true
                                        onExited: parent.hovered = false
                                        onClicked: if (hasApp) app.selectConfirmedObject(modelData.object_id)
                                    }
                                }
```

**Стало:**
```qml
                                delegate: Rectangle {
                                    id: objDelegate
                                    property bool hovered: false
                                    property var tagInfo: root.classTagInfo(modelData.class_id)
                                    property string distFromUav: (hasApp && app.uavStates && app.uavStates.length > 0)
                                        ? root.distanceM(app.uavStates[0].lat, app.uavStates[0].lon, modelData.lat || 0, modelData.lon || 0)
                                        : "--"

                                    width: targetsView.width
                                    radius: 16
                                    color: modelData.selected
                                           ? Qt.rgba(0.22, 0.34, 0.44, hovered ? 0.46 : 0.34)
                                           : (hovered ? Qt.rgba(1, 1, 1, 0.085) : Qt.rgba(1, 1, 1, 0.05))
                                    border.color: modelData.selected
                                                  ? Qt.rgba(0.73, 0.89, 1.0, hovered ? 0.34 : 0.22)
                                                  : Qt.rgba(1, 1, 1, hovered ? 0.14 : 0.06)
                                    border.width: 1
                                    implicitHeight: objContent.implicitHeight + 20
                                    clip: true
                                    scale: hovered ? 1.012 : 1.0
                                    Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
                                    Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
                                    Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.50; mass: 0.85 } }

                                    // Hover/select handler — под контентом
                                    MouseArea {
                                        anchors.fill: parent
                                        z: 0
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onEntered: objDelegate.hovered = true
                                        onExited: objDelegate.hovered = false
                                        onClicked: if (hasApp) app.selectConfirmedObject(modelData.object_id)
                                    }

                                    Column {
                                        id: objContent
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 5
                                        z: 1

                                        // Строка 1: ID-пилюля + тег класса + дистанция
                                        RowLayout {
                                            width: parent.width
                                            spacing: 5

                                            Rectangle {
                                                radius: 6
                                                color: Qt.rgba(1, 1, 1, 0.08)
                                                implicitWidth: objIdText.implicitWidth + 10
                                                implicitHeight: 20
                                                Text {
                                                    id: objIdText
                                                    anchors.centerIn: parent
                                                    text: "#" + String(modelData.object_id || "").slice(-4)
                                                    color: textPrimary
                                                    font.pixelSize: 11
                                                    font.family: "Consolas"
                                                    font.bold: true
                                                }
                                            }

                                            Rectangle {
                                                radius: 6
                                                color: objDelegate.tagInfo.fill
                                                border.color: objDelegate.tagInfo.border
                                                border.width: 1
                                                implicitWidth: objTagText.implicitWidth + 12
                                                implicitHeight: 20
                                                Text {
                                                    id: objTagText
                                                    anchors.centerIn: parent
                                                    text: objDelegate.tagInfo.icon + " " + objDelegate.tagInfo.label
                                                    color: objDelegate.tagInfo.text
                                                    font.pixelSize: 10
                                                    font.family: "Inter"
                                                    font.bold: true
                                                }
                                            }

                                            Item { Layout.fillWidth: true }

                                            Text {
                                                text: "↗ " + objDelegate.distFromUav
                                                color: accentStrong
                                                font.pixelSize: 11
                                                font.family: "Inter"
                                                font.bold: true
                                                visible: objDelegate.distFromUav !== "--"
                                            }
                                        }

                                        // Строка 2: трек + время
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Text {
                                                text: (modelData.track_id !== null && modelData.track_id !== undefined)
                                                      ? ("Трек #" + modelData.track_id) : "Трек н/д"
                                                color: textMuted
                                                font.pixelSize: 10
                                                font.family: "Inter"
                                            }
                                            Text {
                                                text: root._seenAt[modelData.object_id]
                                                      ? ("• " + root.elapsedText(root._seenAt[modelData.object_id], root._elapsedTick))
                                                      : ""
                                                color: textMuted
                                                font.pixelSize: 10
                                                font.family: "Inter"
                                            }
                                        }

                                        // Строка 3: бар уверенности
                                        RowLayout {
                                            width: parent.width
                                            spacing: 8
                                            Rectangle {
                                                Layout.fillWidth: true
                                                implicitHeight: 4
                                                radius: 2
                                                color: Qt.rgba(1, 1, 1, 0.10)
                                                Rectangle {
                                                    width: parent.width * Math.min(1.0, Math.max(0.0, modelData.confidence || 0))
                                                    height: parent.height
                                                    radius: 2
                                                    color: objDelegate.tagInfo.text
                                                    Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutQuad } }
                                                }
                                            }
                                            Text {
                                                text: Math.round((modelData.confidence || 0) * 100) + "%"
                                                color: objDelegate.tagInfo.text
                                                font.pixelSize: 11
                                                font.family: "Inter"
                                                font.bold: true
                                            }
                                        }

                                        // Разделитель
                                        Rectangle { width: parent.width; implicitHeight: 1; color: Qt.rgba(1, 1, 1, 0.07) }

                                        // Строка 4: координаты + кнопка Орбита
                                        RowLayout {
                                            width: parent.width
                                            spacing: 6
                                            Text {
                                                Layout.fillWidth: true
                                                text: Number(modelData.lat || 0).toFixed(5) + "  " + Number(modelData.lon || 0).toFixed(5)
                                                color: "#cfe2ee"
                                                font.pixelSize: 10
                                                font.family: "Consolas"
                                                elide: Text.ElideRight
                                            }
                                            Rectangle {
                                                visible: hasApp ? app.canOpenOrbit : false
                                                implicitWidth: orbitCardBtnText.implicitWidth + 16
                                                implicitHeight: 22
                                                radius: 7
                                                color: Qt.rgba(0.20, 0.48, 0.78, 0.22)
                                                border.color: Qt.rgba(0.62, 0.84, 1.0, 0.28)
                                                border.width: 1
                                                Text {
                                                    id: orbitCardBtnText
                                                    anchors.centerIn: parent
                                                    text: "Орбита"
                                                    color: accent
                                                    font.pixelSize: 10
                                                    font.family: "Inter"
                                                    font.bold: true
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        if (!hasApp) return
                                                        app.selectConfirmedObject(modelData.object_id)
                                                        app.orbitConfirmedObject()
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
```

- [ ] **Шаг 2: Проверить приложение**

```powershell
python -m fire_uav
```

В правой колонке карточки объектов должны показывать: цветной тег класса, дистанцию, трек, время, бар уверенности, координаты и кнопку «Орбита». Если объектов нет — список пустой (норма). Закрыть.

- [ ] **Шаг 3: Коммит**

```bash
git add fire_uav/gui/qml/additional.qml
git commit -m "ui: rich object cards with class tag, distance, track, elapsed time, confidence bar"
```

---

### Task 5: Финальная проверка

- [ ] **Шаг 1: Проверить весь экран целиком**

```powershell
python -m fire_uav
```

Проверить визуально:
- [ ] Верхняя панель: 8 чипов одинаковой ширины, занимают всю ширину
- [ ] Левая колонка: карточка «Управление» показывает контекстную подсказку + кнопки фазы + «Детектор»/«Сохранить» внизу
- [ ] Карточка «Маршрут / Точки» работает как раньше (тоггл орбиты, режимы draw/edit/view)
- [ ] Правая колонка: карточки объектов с расширенной информацией (или пустой список)
- [ ] Журнал системы работает
- [ ] Карта загружается

- [ ] **Шаг 2: Финальный коммит**

```bash
git add fire_uav/gui/qml/additional.qml
git commit -m "ui: additional screen UI polish — all three areas complete"
```
