import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Dialogs
import QtWebEngine 1.10
import Qt5Compat.GraphicalEffects

ApplicationWindow {
    id: root
    width: 1800
    height: 1020
    minimumWidth: 1400
    minimumHeight: 820
    visible: true
    title: "fire_uav // тактический экран"
    color: "#000000"

    property bool hasApp: typeof app !== "undefined" && app !== null
    property string missionState: hasApp ? app.missionState : "PREFLIGHT"
    property bool isPreflight: missionState === "PREFLIGHT"
    property bool isReady: missionState === "READY"
    property bool isInFlight: missionState === "IN_FLIGHT"
    property bool isRtl: missionState === "RTL"
    property bool isPostflight: missionState === "POSTFLIGHT"
    property color pageBg: "#000000"
    property color panelBg: "#151515"
    property color panelBgSoft: "#1c1c1c"
    property color panelInset: "#222222"
    property color textPrimary: "#edf3f7"
    property color textMuted: "#9aa7b5"
    property color accent: "#7bc6ff"
    property color accentStrong: "#f7b55c"
    property color borderColor: Qt.rgba(1, 1, 1, 0.10)
    property color borderSoft: Qt.rgba(1, 1, 1, 0.06)
    property color mapGlassFill: Qt.rgba(0.08, 0.08, 0.08, 0.35)
    property color mapGlassFillStrong: Qt.rgba(0.04, 0.04, 0.04, 0.52)
    property color mapGlassBorder: Qt.rgba(1, 1, 1, 0.18)
    property color mapGlassShadow: Qt.rgba(0, 0, 0, 0.24)
    property bool homePickModeActive: hasApp ? app.homePickModeEnabled : false
    property bool manualTargetModeActive: hasApp ? app.objectSpawnModeEnabled : false
    property bool showVideoFeed: true
    property bool toastVisible: false
    property string toastText: ""
    property bool confirmPlanSheetVisible: false
    property bool orbitSelectionSheetVisible: false
    property var orbitSelection: []
    property bool mapNeedsRefresh: hasApp ? app.mapRefreshNeeded : false
    property bool lastRouteEditMode: false
    property string routePanelMode: "view"
    property string currentTimeText: Qt.formatTime(new Date(), "hh:mm:ss")
    property var pendingConsoleMessages: []
    property bool mapBridgeInjected: false

    function runMapTool(name, arg) {
        if (!mapView) return;
        var callArg = (arg === undefined) ? "" : JSON.stringify(arg);
        var js = "window.__mapTools && window.__mapTools." + name
               + " && window.__mapTools." + name + "(" + callArg + ")";
        mapView.runJavaScript(js);
    }

    function missionTone() {
        if (!hasApp) return "#9aa7b5";
        if (app.missionState === "IN_FLIGHT") return "#7bc6ff";
        if (app.missionState === "RTL") return "#f7b55c";
        if (app.missionState === "POSTFLIGHT") return "#78d79a";
        return "#d9e1e8";
    }

    function missionStateRu(state) {
        if (state === "PREFLIGHT") return "ПРЕДПОЛЕТ";
        if (state === "READY") return "ГОТОВ";
        if (state === "IN_FLIGHT") return "В ПОЛЕТЕ";
        if (state === "RTL") return "ВОЗВРАТ";
        if (state === "POSTFLIGHT") return "ПОСЛЕ ПОЛЕТА";
        return state || "--";
    }

    function linkStatusRu(value) {
        if (value === "OK") return "НОРМА";
        if (value === "DISCONNECTED") return "НЕТ СВЯЗИ";
        return value || "--";
    }

    function cameraStatusRu(value) {
        if (!value) return "Камера офлайн";
        var text = String(value);
        if (text.indexOf("Camera stream disconnected") !== -1) return "Поток камеры отключен";
        if (text.indexOf("Camera not found") !== -1) return "Камера не найдена";
        if (text.indexOf("Camera offline") !== -1) return "Камера офлайн";
        return text;
    }

    function routeBatteryTextRu(value) {
        if (!value) return "--";
        var text = String(value);
        if (text.indexOf("--") !== -1) return "--";
        if (text.indexOf("n/a") !== -1) return "н/д";
        var match = text.match(/-?\d+(\.\d+)?%/);
        if (match && match.length > 0) return match[0];
        return text.replace("Remaining:", "Остаток:");
    }

    function waypointLabelRu(label, index) {
        var fallbackIndex = (index === undefined || index === null) ? "" : String(index);
        var text = label ? String(label) : ("WP" + fallbackIndex);
        if (text.indexOf("WP") === 0) return "Точка " + text.slice(2);
        return text;
    }

    function enterRouteDrawMode() {
        routePanelMode = "draw";
        runMapTool("setRemoveMode", false);
        runMapTool("drawPath");
    }

    function enterRouteEditMode() {
        routePanelMode = "edit";
        if (hasApp) app.regenerateMap();
    }

    function showRouteList() {
        routePanelMode = "view";
        runMapTool("setRemoveMode", false);
        runMapTool("stopDraw");
    }

    function currentUavLabel() {
        if (!hasApp || !app.uavStates || app.uavStates.length === 0) return "--";
        var state = app.uavStates[0];
        if (!state || !state.uavId) return "--";
        return state.uavId;
    }

    function showToastMessage(message) {
        toastText = message;
        toastVisible = true;
        toastTimer.restart();
    }

    function requestConfirmPlan() {
        if (!hasApp) return;
        confirmPlanSheetVisible = true;
    }

    function requestOrbit() {
        if (!hasApp) return;
        if (app.confirmedObjectCount > 1) {
            orbitSelection = [];
            orbitSelectionSheetVisible = true;
        } else {
            app.orbitConfirmedObject();
        }
    }

    function toggleOrbitSelection(objectId) {
        var next = orbitSelection.slice(0);
        var idx = next.indexOf(objectId);
        if (idx === -1) next.push(objectId);
        else next.splice(idx, 1);
        orbitSelection = next;
    }

    onShowVideoFeedChanged: {
        if (hasApp) app.setVideoVisible(showVideoFeed);
    }

    Timer {
        id: toastTimer
        interval: 2600
        repeat: false
        onTriggered: root.toastVisible = false
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: root.currentTimeText = Qt.formatTime(new Date(), "hh:mm:ss")
    }

    Connections {
        target: hasApp ? app : null
        function onToastRequested(message) { root.showToastMessage(message); }
        function onFrameReady(url) { videoView.source = url; }
        function onObjectNotificationReceived(objectId, classId, confidence, message, trackId) {
            var idLabel = objectId && objectId.length ? ("#" + objectId) : "н/д";
            var clsLabel = classId >= 0 ? classId : "н/д";
            var confLabel = confidence > 0 ? (confidence * 100).toFixed(1) + "%" : "н/д";
            var trackLabel = (trackId !== null && trackId !== undefined) ? trackId : "н/д";
            root.showToastMessage("Объект " + idLabel + " трек " + trackLabel + " класс " + clsLabel + " дост. " + confLabel);
        }
        function onLogsChanged() { if (logView) logView.positionViewAtEnd(); }
        function onMapRefreshNeededChanged() {
            if (!hasApp) return;
            if (app.mapRefreshNeeded) app.refreshMapView();
        }
        function onFlightControlsChanged() {
            if (!hasApp) return;
            var next = app.routeEditMode;
            if (next === root.lastRouteEditMode) return;
            root.lastRouteEditMode = next;
            root.routePanelMode = next ? "edit" : "view";
            runMapTool("setAppendMode", next);
            if (next) runMapTool("drawPath");
            else runMapTool("stopDraw");
        }
    }

    Rectangle {
        anchors.fill: parent
        color: pageBg
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: Qt.rgba(1, 1, 1, 0.04)
        border.width: 1
    }

    component GlassButton: Item {
        id: glassButtonRoot
        property string label: ""
        property var action
        property bool accentButton: false
        property bool warningButton: false
        property bool hovered: false
        property bool pressed: false
        property bool enabled: true
        property int px: 13
        implicitWidth: Math.max(96, labelText.implicitWidth + 28)
        implicitHeight: 40
        opacity: enabled ? 1.0 : 0.42
        scale: !enabled ? 1.0 : (pressed ? 0.965 : (hovered ? 1.025 : 1.0))
        clip: true
        transformOrigin: Item.Center
        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }
        Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.42; mass: 0.75 } }

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            clip: true
            antialiasing: true
            color: !glassButtonRoot.enabled ? "transparent"
                   : warningButton
                     ? Qt.rgba(0.34, 0.10, 0.08, glassButtonRoot.hovered || glassButtonRoot.pressed ? 0.58 : 0.38)
                     : accentButton
                       ? Qt.rgba(0.20, 0.48, 0.78, glassButtonRoot.hovered || glassButtonRoot.pressed ? 0.42 : 0.28)
                       : (glassButtonRoot.pressed ? Qt.rgba(0.20, 0.20, 0.20, 0.55)
                                                  : (glassButtonRoot.hovered ? Qt.rgba(0.16, 0.16, 0.16, 0.35)
                                                                             : "transparent"))
            border.color: warningButton
                          ? Qt.rgba(1.0, 0.62, 0.58, glassButtonRoot.hovered ? 0.36 : 0.22)
                          : accentButton
                            ? Qt.rgba(0.62, 0.84, 1.0, glassButtonRoot.hovered ? 0.42 : 0.24)
                            : "transparent"
            border.width: 1
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        Text {
            id: labelText
            anchors.centerIn: parent
            text: (glassButtonRoot.label || "").toUpperCase()
            color: !glassButtonRoot.enabled ? textMuted
                   : warningButton
                     ? (glassButtonRoot.hovered || glassButtonRoot.pressed ? "#ffb3a7" : textPrimary)
                     : (glassButtonRoot.hovered || glassButtonRoot.pressed || accentButton ? "#7bc6ff" : textPrimary)
            font.pixelSize: glassButtonRoot.px
            font.family: "Inter"
            font.bold: accentButton || warningButton || glassButtonRoot.hovered
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            enabled: glassButtonRoot.enabled
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onEntered: glassButtonRoot.hovered = true
            onExited: {
                glassButtonRoot.hovered = false;
                glassButtonRoot.pressed = false;
            }
            onPressed: glassButtonRoot.pressed = true
            onReleased: glassButtonRoot.pressed = false
            onClicked: if (glassButtonRoot.action) glassButtonRoot.action()
        }
    }

    component StatChip: Rectangle {
        property string chipLabel: ""
        property string chipValue: ""
        property color valueColor: textPrimary
        implicitWidth: Math.max(96, chipColumn.implicitWidth + 20)
        implicitHeight: 48
        radius: 16
        color: panelBgSoft
        border.color: borderSoft
        border.width: 1
        clip: true
        antialiasing: true

        Column {
            id: chipColumn
            anchors.centerIn: parent
            spacing: 2

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: (chipLabel || "").toUpperCase()
                color: textMuted
                font.pixelSize: 11
                font.family: "Inter"
                font.letterSpacing: 1.2
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: chipValue
                color: valueColor
                font.pixelSize: 16
                font.family: "Inter"
                font.bold: true
            }
        }
    }

    component SectionCard: Rectangle {
        radius: 22
        color: panelBg
        border.color: borderColor
        border.width: 1
        clip: true
        antialiasing: true
    }

    component PillSwitch: Item {
        id: switchRoot
        property bool checked: false
        signal toggled(bool checked)
        width: 44
        height: 24

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: switchRoot.checked ? Qt.rgba(0.16, 0.56, 0.84, 0.85) : Qt.rgba(1, 1, 1, 0.16)
            border.color: switchRoot.checked ? Qt.rgba(0.65, 0.86, 1.0, 0.45) : Qt.rgba(1, 1, 1, 0.22)
            border.width: 1
            antialiasing: true
        }

        Rectangle {
            width: 18
            height: 18
            radius: 9
            y: 3
            x: switchRoot.checked ? (switchRoot.width - width - 3) : 3
            color: "#f5fbff"
            antialiasing: true
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                switchRoot.checked = !switchRoot.checked;
                switchRoot.toggled(switchRoot.checked);
            }
        }
    }

    component MapGlassPane: Item {
        id: mapGlassPane
        property Item blurSource: null
        property color color: mapGlassFill
        property color borderColor: mapGlassBorder
        property real radius: 20
        property real blurRadius: 16
        property real highlightOpacity: 0.25
        property bool glassBlurEnabled: blurSource !== null
        property point blurOrigin: blurSource ? blurSource.mapFromItem(mapGlassPane, 0, 0) : Qt.point(0, 0)
        clip: true
        layer.enabled: true
        layer.smooth: true
        layer.effect: OpacityMask {
            maskSource: Rectangle {
                width: mapGlassPane.width
                height: mapGlassPane.height
                radius: mapGlassPane.radius
            }
        }

        ShaderEffectSource {
            id: mapGlassSlice
            anchors.fill: parent
            sourceItem: mapGlassPane.glassBlurEnabled ? mapGlassPane.blurSource : null
            sourceRect: Qt.rect(mapGlassPane.blurOrigin.x, mapGlassPane.blurOrigin.y, mapGlassPane.width, mapGlassPane.height)
            recursive: true
            live: mapGlassPane.glassBlurEnabled && root.visible
            opacity: 0.0
        }

        FastBlur {
            id: mapGlassBlur
            anchors.fill: parent
            source: mapGlassSlice
            radius: mapGlassPane.blurRadius
            transparentBorder: true
            visible: mapGlassPane.glassBlurEnabled
            z: -3
        }

        OpacityMask {
            anchors.fill: parent
            source: mapGlassBlur
            maskSource: Rectangle {
                width: mapGlassPane.width
                height: mapGlassPane.height
                radius: mapGlassPane.radius
            }
            visible: mapGlassPane.glassBlurEnabled
            z: -2
        }

        Rectangle {
            anchors.fill: parent
            radius: mapGlassPane.radius
            color: mapGlassPane.color
            border.color: mapGlassPane.borderColor
            border.width: 1
            antialiasing: true
            z: -1
        }

        Rectangle {
            anchors.fill: parent
            radius: mapGlassPane.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(1, 1, 1, 0.12) }
                GradientStop { position: 1.0; color: Qt.rgba(1, 1, 1, 0.06) }
            }
            opacity: mapGlassPane.highlightOpacity
            Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
            z: -0.5
        }
    }

    component CompactComboBox: ComboBox {
        id: comboRoot
        implicitHeight: 38
        font.pixelSize: 12
        font.family: "Inter"
        model: []
        scale: pressed ? 0.985 : (hovered ? 1.012 : 1.0)
        Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.45; mass: 0.8 } }

        delegate: ItemDelegate {
            width: ListView.view ? ListView.view.width : comboRoot.width
            height: 38
            padding: 0
            scale: highlighted ? 1.015 : 1.0
            Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.48; mass: 0.8 } }

            background: Rectangle {
                anchors.fill: parent
                anchors.margins: 3
                radius: height / 2
                color: highlighted ? Qt.rgba(0.16, 0.30, 0.42, 0.72) : Qt.rgba(1, 1, 1, 0.04)
                border.color: highlighted ? Qt.rgba(0.62, 0.84, 1.0, 0.28) : "transparent"
                border.width: highlighted ? 1 : 0
                antialiasing: true
                Behavior on color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
                Behavior on border.color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
            }

            contentItem: Text {
                leftPadding: highlighted ? 18 : 14
                rightPadding: 12
                text: modelData
                color: highlighted ? "#dff1ff" : textPrimary
                font.pixelSize: 12
                font.family: "Inter"
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Behavior on leftPadding { NumberAnimation { duration: 130; easing.type: Easing.OutQuad } }
                Behavior on color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
            }
        }

        indicator: Canvas {
            x: comboRoot.width - width - 12
            y: (comboRoot.height - height) / 2
            width: 10
            height: 6
            contextType: "2d"

            onPaint: {
                context.reset();
                context.beginPath();
                context.moveTo(0, 0);
                context.lineTo(width, 0);
                context.lineTo(width / 2, height);
                context.closePath();
                context.fillStyle = "#c6d4de";
                context.fill();
            }
        }

        contentItem: Text {
            leftPadding: 12
            rightPadding: comboRoot.indicator.width + 20
            text: comboRoot.displayText
            color: textPrimary
            font.pixelSize: 12
            font.family: "Inter"
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: height / 2
            color: comboRoot.hovered ? Qt.rgba(0.16, 0.16, 0.16, 0.62) : Qt.rgba(0.10, 0.10, 0.10, 0.46)
            border.color: Qt.rgba(1, 1, 1, comboRoot.hovered ? 0.22 : 0.14)
            border.width: 1
            antialiasing: true
            Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
            Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
        }

        popup: Popup {
            y: comboRoot.height + 6
            width: comboRoot.width
            padding: 6
            modal: false
            focus: true
            closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
            enter: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 150; easing.type: Easing.OutCubic }
                    NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: 170; easing.type: Easing.OutBack }
                }
            }
            exit: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 110; easing.type: Easing.OutQuad }
                    NumberAnimation { property: "scale"; from: 1.0; to: 0.97; duration: 110; easing.type: Easing.OutQuad }
                }
            }

            background: Rectangle {
                radius: 18
                color: Qt.rgba(0.05, 0.05, 0.05, 0.94)
                border.color: Qt.rgba(1, 1, 1, 0.18)
                border.width: 1
                antialiasing: true
            }

            contentItem: ListView {
                clip: true
                implicitHeight: Math.min(contentHeight, 220)
                model: comboRoot.popup.visible ? comboRoot.delegateModel : null
                currentIndex: comboRoot.highlightedIndex
                spacing: 1
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 14

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 78
            radius: 24
            color: panelBg
            border.color: borderColor
            border.width: 1
            clip: true

            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 14

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
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14

            Item {
                Layout.preferredWidth: Math.max(300, Math.min(340, root.width * 0.22))
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    SectionCard {
                        Layout.fillWidth: true
                        implicitHeight: actionCardColumn.implicitHeight + 20
                        color: Qt.rgba(0.08, 0.08, 0.08, 0.72)
                        border.color: Qt.rgba(1, 1, 1, 0.12)

                        ColumnLayout {
                            id: actionCardColumn
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 10

                            Text {
                                text: "Управление"
                                color: textPrimary
                                font.pixelSize: 18
                                font.family: "Inter"
                                font.bold: true
                            }

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
                        }
                    }

                    SectionCard {
                        Layout.fillWidth: true
                        implicitHeight: telemetryColumn.implicitHeight + 24
                        color: Qt.rgba(0.08, 0.08, 0.08, 0.72)
                        border.color: Qt.rgba(1, 1, 1, 0.12)

                        ColumnLayout {
                            id: telemetryColumn
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 10

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: "Миссия"
                                    color: textPrimary
                                    font.pixelSize: 18
                                    font.family: "Inter"
                                    font.bold: true
                                }

                                Rectangle {
                                    Layout.preferredWidth: planStatusText.implicitWidth + 20
                                    Layout.preferredHeight: 28
                                    radius: height / 2
                                    color: hasApp && app.planConfirmed ? Qt.rgba(0.18, 0.42, 0.26, 0.46) : Qt.rgba(0.45, 0.24, 0.10, 0.42)
                                    border.color: hasApp && app.planConfirmed ? Qt.rgba(0.55, 0.95, 0.68, 0.24) : Qt.rgba(1.0, 0.75, 0.46, 0.22)
                                    border.width: 1

                                    Text {
                                        id: planStatusText
                                        anchors.centerIn: parent
                                        text: hasApp && app.planConfirmed ? "ПЛАН ПОДТВЕРЖДЕН" : "ПЛАН НЕ ПОДТВЕРЖДЕН"
                                        color: hasApp && app.planConfirmed ? "#8fe9ad" : "#ffcf80"
                                        font.pixelSize: 10
                                        font.family: "Inter"
                                        font.bold: true
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 42
                                radius: height / 2
                                color: Qt.rgba(1, 1, 1, 0.06)
                                border.color: Qt.rgba(1, 1, 1, 0.10)
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: hasApp ? root.missionStateRu(app.missionState) : "--"
                                    color: root.missionTone()
                                    font.pixelSize: 15
                                    font.family: "Inter"
                                    font.bold: true
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: 8
                                rowSpacing: 8

                                StatChip { Layout.fillWidth: true; chipLabel: "КАДРЫ"; chipValue: hasApp ? app.fps.toFixed(1) : "--"; valueColor: textPrimary }
                                StatChip { Layout.fillWidth: true; chipLabel: "ЗАДЕРЖКА"; chipValue: hasApp ? (app.latencyMs.toFixed(0) + " мс") : "--"; valueColor: textPrimary }
                                StatChip { Layout.fillWidth: true; chipLabel: "ДОВЕРИЕ"; chipValue: hasApp ? (Math.round(app.detectionConfidence * 100) + "%") : "--"; valueColor: accent }
                                StatChip { Layout.fillWidth: true; chipLabel: "РЕЗЕРВ"; chipValue: hasApp ? root.routeBatteryTextRu(app.routeBatteryRemainingText) : "--"; valueColor: hasApp && app.routeBatteryWarning ? "#ff9a86" : textPrimary }
                            }
                        }
                    }

                    SectionCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ColumnLayout {
                            id: routeColumn
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 10

                            property bool orbitParamsExpanded: false

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: routePanelMode === "draw" ? "Рисование пути"
                                          : routePanelMode === "edit" ? "Редактирование пути"
                                          : "Маршрут / Точки"
                                    color: textPrimary
                                    font.pixelSize: 16
                                    font.family: "Inter"
                                    font.bold: true
                                }

                                Rectangle {
                                    Layout.preferredWidth: orbitToggleText.implicitWidth + 16
                                    Layout.preferredHeight: 26
                                    radius: height / 2
                                    color: routeColumn.orbitParamsExpanded ? Qt.rgba(0.28, 0.42, 0.18, 0.46) : Qt.rgba(1, 1, 1, 0.06)
                                    border.color: routeColumn.orbitParamsExpanded ? Qt.rgba(0.55, 0.95, 0.38, 0.30) : Qt.rgba(1, 1, 1, 0.10)
                                    border.width: 1
                                    visible: routePanelMode === "view"
                                    Behavior on color { ColorAnimation { duration: 150 } }

                                    Text {
                                        id: orbitToggleText
                                        anchors.centerIn: parent
                                        text: routeColumn.orbitParamsExpanded ? "ОРБИТА ▲" : "ОРБИТА ▼"
                                        color: routeColumn.orbitParamsExpanded ? "#a8e890" : textMuted
                                        font.pixelSize: 10
                                        font.family: "Inter"
                                        font.bold: true
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: routeColumn.orbitParamsExpanded = !routeColumn.orbitParamsExpanded
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: routeModeText.implicitWidth + 18
                                    Layout.preferredHeight: 26
                                    radius: height / 2
                                    color: routePanelMode === "view" ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0.12, 0.34, 0.52, 0.42)
                                    border.color: routePanelMode === "view" ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0.62, 0.84, 1.0, 0.24)
                                    border.width: 1

                                    Text {
                                        id: routeModeText
                                        anchors.centerIn: parent
                                        text: routePanelMode === "draw" ? "РИСОВАНИЕ"
                                              : routePanelMode === "edit" ? "ПРАВКА"
                                              : "ГОТОВО"
                                        color: routePanelMode === "view" ? textMuted : "#7bc6ff"
                                        font.pixelSize: 10
                                        font.family: "Inter"
                                        font.bold: true
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: routeColumn.orbitParamsExpanded && routePanelMode === "view"
                                opacity: visible ? 1 : 0
                                spacing: 10
                                Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }

                                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Qt.rgba(1, 1, 1, 0.08) }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: "Радиус орбиты"; color: textMuted; font.pixelSize: 11; font.family: "Inter" }
                                        Text { text: hasApp ? Math.round(app.orbitRadiusM) + " м" : "-- м"; color: accent; font.pixelSize: 11; font.family: "Inter"; font.bold: true }
                                    }
                                    Slider {
                                        id: orbitRadiusSliderAdditional
                                        Layout.fillWidth: true
                                        from: 10; to: 150; stepSize: 1
                                        value: hasApp ? app.orbitRadiusM : 50
                                        live: true
                                        onMoved: { if (hasApp) app.setOrbitRadiusM(value) }
                                        onPressedChanged: { if (!pressed && hasApp) app.setOrbitRadiusM(value) }
                                        Connections {
                                            target: hasApp ? app : null
                                            function onOrbitRadiusMChanged() { orbitRadiusSliderAdditional.value = app.orbitRadiusM }
                                        }
                                        background: Rectangle {
                                            x: orbitRadiusSliderAdditional.leftPadding; y: orbitRadiusSliderAdditional.topPadding + orbitRadiusSliderAdditional.availableHeight / 2 - height / 2
                                            implicitWidth: 200; implicitHeight: 4; width: orbitRadiusSliderAdditional.availableWidth; height: implicitHeight
                                            radius: 2; color: Qt.rgba(1, 1, 1, 0.12)
                                            Rectangle { width: orbitRadiusSliderAdditional.visualPosition * parent.width; height: parent.height; radius: 2; color: accent }
                                        }
                                        handle: Rectangle {
                                            x: orbitRadiusSliderAdditional.leftPadding + orbitRadiusSliderAdditional.visualPosition * orbitRadiusSliderAdditional.availableWidth - width / 2
                                            y: orbitRadiusSliderAdditional.topPadding + orbitRadiusSliderAdditional.availableHeight / 2 - height / 2
                                            implicitWidth: 14; implicitHeight: 14; radius: 7; color: accent
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: "Точек на окружность"; color: textMuted; font.pixelSize: 11; font.family: "Inter" }
                                        Text { text: hasApp ? app.orbitPointsPerCircle + " пт" : "-- пт"; color: accent; font.pixelSize: 11; font.family: "Inter"; font.bold: true }
                                    }
                                    Slider {
                                        id: orbitPointsSlider
                                        Layout.fillWidth: true
                                        from: 4; to: 36; stepSize: 1
                                        value: hasApp ? app.orbitPointsPerCircle : 12
                                        live: true
                                        onMoved: { if (hasApp) app.setOrbitPointsPerCircle(Math.round(value)) }
                                        onPressedChanged: { if (!pressed && hasApp) app.setOrbitPointsPerCircle(Math.round(value)) }
                                        Connections {
                                            target: hasApp ? app : null
                                            function onOrbitPointsPerCircleChanged() { orbitPointsSlider.value = app.orbitPointsPerCircle }
                                        }
                                        background: Rectangle {
                                            x: orbitPointsSlider.leftPadding; y: orbitPointsSlider.topPadding + orbitPointsSlider.availableHeight / 2 - height / 2
                                            implicitWidth: 200; implicitHeight: 4; width: orbitPointsSlider.availableWidth; height: implicitHeight
                                            radius: 2; color: Qt.rgba(1, 1, 1, 0.12)
                                            Rectangle { width: orbitPointsSlider.visualPosition * parent.width; height: parent.height; radius: 2; color: accent }
                                        }
                                        handle: Rectangle {
                                            x: orbitPointsSlider.leftPadding + orbitPointsSlider.visualPosition * orbitPointsSlider.availableWidth - width / 2
                                            y: orbitPointsSlider.topPadding + orbitPointsSlider.availableHeight / 2 - height / 2
                                            implicitWidth: 14; implicitHeight: 14; radius: 7; color: accent
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text { Layout.fillWidth: true; text: "Резерв батареи (возврат)"; color: textMuted; font.pixelSize: 11; font.family: "Inter" }
                                        Text { text: hasApp ? Math.round(app.minReturnPercent) + " %" : "-- %"; color: accentStrong; font.pixelSize: 11; font.family: "Inter"; font.bold: true }
                                    }
                                    Slider {
                                        id: minReturnSlider
                                        Layout.fillWidth: true
                                        from: 5; to: 50; stepSize: 1
                                        value: hasApp ? app.minReturnPercent : 20
                                        live: true
                                        onMoved: { if (hasApp) app.setMinReturnPercent(value) }
                                        onPressedChanged: { if (!pressed && hasApp) app.setMinReturnPercent(value) }
                                        Connections {
                                            target: hasApp ? app : null
                                            function onMinReturnPercentChanged() { minReturnSlider.value = app.minReturnPercent }
                                        }
                                        background: Rectangle {
                                            x: minReturnSlider.leftPadding; y: minReturnSlider.topPadding + minReturnSlider.availableHeight / 2 - height / 2
                                            implicitWidth: 200; implicitHeight: 4; width: minReturnSlider.availableWidth; height: implicitHeight
                                            radius: 2; color: Qt.rgba(1, 1, 1, 0.12)
                                            Rectangle { width: minReturnSlider.visualPosition * parent.width; height: parent.height; radius: 2; color: accentStrong }
                                        }
                                        handle: Rectangle {
                                            x: minReturnSlider.leftPadding + minReturnSlider.visualPosition * minReturnSlider.availableWidth - width / 2
                                            y: minReturnSlider.topPadding + minReturnSlider.availableHeight / 2 - height / 2
                                            implicitWidth: 14; implicitHeight: 14; radius: 7; color: accentStrong
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Qt.rgba(1, 1, 1, 0.08) }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: routePanelMode === "draw"
                                opacity: visible ? 1 : 0
                                spacing: 8
                                Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Кликайте по карте, чтобы добавлять точки маршрута. Используйте режим удаления, если нужно убрать лишние точки."
                                    color: textMuted
                                    font.pixelSize: 11
                                    font.family: "Inter"
                                    wrapMode: Text.WordWrap
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 2
                                    columnSpacing: 8
                                    rowSpacing: 8

                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Добавлять точки"; accentButton: true; action: function() { root.runMapTool("setRemoveMode", false); root.runMapTool("drawPath"); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Удалять точки"; warningButton: true; action: function() { root.runMapTool("setRemoveMode", true); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Точка дома"; action: function() { if (hasApp) { if (homePickModeActive) app.stopHomePickMode(); else app.startHomePickMode(); } } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Ручная цель"; action: function() { if (hasApp) { if (manualTargetModeActive) app.stopManualTargetMode(); else app.startManualTargetMode(); } } }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Сброс вида"; action: function() { root.runMapTool("resetView"); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Готово"; accentButton: true; action: function() { root.showRouteList(); } }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                visible: routePanelMode === "edit"
                                opacity: visible ? 1 : 0
                                spacing: 8
                                Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

                                Text {
                                    Layout.fillWidth: true
                                    text: "Настройки правки маршрута. Перестройте карту, включите редактирование или примените изменения после корректировки."
                                    color: textMuted
                                    font.pixelSize: 11
                                    font.family: "Inter"
                                    wrapMode: Text.WordWrap
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 2
                                    columnSpacing: 8
                                    rowSpacing: 8

                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Перестроить"; accentButton: true; action: function() { if (hasApp) app.regenerateMap(); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: hasApp && app.routeEditMode ? "Применить" : "Редактировать"; enabled: hasApp ? (app.routeEditMode ? app.canApplyRouteEdits : app.canEditRoute) : false; action: function() { if (!hasApp) return; if (app.routeEditMode) app.applyRouteEdits(); else app.editRoute(); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Отмена правки"; enabled: hasApp ? app.canCancelRouteEdits : false; action: function() { if (hasApp) app.cancelRouteEdits(); root.showRouteList(); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Удалять точки"; warningButton: true; action: function() { root.runMapTool("setRemoveMode", true); } }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8

                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Импорт GeoJSON"; action: function() { geojsonDialog.open(); } }
                                    GlassButton { Layout.fillWidth: true; implicitHeight: 38; label: "Импорт KML"; action: function() { kmlDialog.open(); } }
                                }

                                GlassButton {
                                    Layout.fillWidth: true
                                    implicitHeight: 38
                                    label: "Показать точки"
                                    action: function() { root.showRouteList(); }
                                }
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                visible: routePanelMode === "view"
                                clip: true
                                spacing: 4
                                model: hasApp ? app.routeWaypointItems : []

                                delegate: Rectangle {
                                    width: routeColumn.width
                                    radius: 10
                                    color: Qt.rgba(1, 1, 1, 0.04)
                                    border.color: borderSoft
                                    border.width: 1
                                    implicitHeight: 38

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 6
                                        spacing: 6

                                        Text { text: root.waypointLabelRu(modelData.label, modelData.index); color: textPrimary; font.pixelSize: 11; font.family: "Inter"; font.bold: true }
                                        Text { Layout.fillWidth: true; text: Number(modelData.lat || 0).toFixed(4) + ", " + Number(modelData.lon || 0).toFixed(4); color: textMuted; font.pixelSize: 10; font.family: "Inter"; elide: Text.ElideRight }
                                        Text { text: (modelData.distance_m === null || modelData.distance_m === undefined) ? "--" : (Math.round(modelData.distance_m) + " м"); color: "#cfe2ee"; font.pixelSize: 10; font.family: "Inter" }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                SectionCard {
                    anchors.fill: parent
                }

                Item {
                    id: stageLayer
                    anchors.fill: parent
                    anchors.margins: 12
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        radius: 20
                        color: "#090909"
                        border.color: Qt.rgba(1, 1, 1, 0.05)
                        border.width: 1
                        clip: true
                    }

                    WebEngineView {
                        id: mapView
                        anchors.fill: parent
                        anchors.margins: 4
                        url: hasApp ? app.mapUrl : ""
                        clip: true
                        settings.localContentCanAccessFileUrls: true
                        settings.localContentCanAccessRemoteUrls: true
                        settings.javascriptCanOpenWindows: false

                        onLoadingChanged: function(loadRequest) {
                            if (loadRequest.status === WebEngineView.LoadStartedStatus) {
                                root.mapBridgeInjected = false;
                                root.pendingConsoleMessages = [];
                            } else if (loadRequest.status === WebEngineView.LoadSucceededStatus) {
                                root.mapBridgeInjected = true;
                                if (hasApp) {
                                    mapView.runJavaScript(app.mapBridgeScript);
                                    for (var i = 0; i < root.pendingConsoleMessages.length; i++) {
                                        app.handleMapConsole(root.pendingConsoleMessages[i]);
                                    }
                                }
                                root.pendingConsoleMessages = [];
                            }
                        }

                        onJavaScriptConsoleMessage: function(level, message, lineNumber, sourceID) {
                            if (!root.mapBridgeInjected) {
                                root.pendingConsoleMessages.push(message);
                                return;
                            }
                            if (hasApp) app.handleMapConsole(message);
                            if (level === WebEngineView.ErrorMessageLevel) {
                                console.error("Ошибка JS карты", message, lineNumber, sourceID);
                            }
                        }
                    }

                    MapGlassPane {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 18
                        height: 60
                        radius: height / 2
                        blurSource: mapView
                        color: mapGlassFillStrong

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 8

                            GlassButton {
                                Layout.fillWidth: true
                                label: "Рисовать путь"
                                accentButton: routePanelMode === "draw"
                                action: function() { root.enterRouteDrawMode(); }
                            }

                            GlassButton {
                                Layout.fillWidth: true
                                label: "Удалить"
                                warningButton: routePanelMode === "draw" || routePanelMode === "edit"
                                action: function() {
                                    root.routePanelMode = root.routePanelMode === "view" ? "draw" : root.routePanelMode;
                                    root.runMapTool("setRemoveMode", true);
                                }
                            }

                            GlassButton {
                                Layout.fillWidth: true
                                label: "Сброс"
                                action: function() { root.runMapTool("resetView"); }
                            }

                            GlassButton {
                                Layout.fillWidth: true
                                label: "Перестроить"
                                accentButton: routePanelMode === "edit"
                                action: function() { root.enterRouteEditMode(); }
                            }

                            GlassButton {
                                Layout.fillWidth: true
                                label: "Обновить карту"
                                action: function() { if (hasApp) app.refreshMapView(); }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: root.mapNeedsRefresh ? "КАРТА ТРЕБУЕТ ОБНОВЛЕНИЯ" : "КАРТА ГОТОВА"
                                color: root.mapNeedsRefresh ? "#ffcf80" : textMuted
                                font.pixelSize: 11
                                font.family: "Inter"
                                font.bold: root.mapNeedsRefresh
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Item {
                        id: mapClickOverlay
                        anchors.fill: parent
                        visible: homePickModeActive || manualTargetModeActive
                        z: 4

                        MouseArea {
                            anchors.fill: parent
                            enabled: mapClickOverlay.visible
                            acceptedButtons: Qt.LeftButton
                            onClicked: {
                                var script = "window.__mapTools && window.__mapTools.screenToGeo(" + mouse.x + "," + mouse.y + ")";
                                mapView.runJavaScript(script, function(result) {
                                    if (!result || typeof result.lat !== "number" || typeof result.lon !== "number") return;
                                    if (homePickModeActive && hasApp) app.setHomeFromMap(result.lat, result.lon);
                                    if (manualTargetModeActive && hasApp) app.spawnManualTargetAt(result.lat, result.lon);
                                });
                            }
                        }
                    }

                    MapGlassPane {
                        id: videoDock
                        width: root.showVideoFeed ? Math.min(parent.width * 0.32, 360) : 0
                        height: root.showVideoFeed ? 214 : 0
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 18
                        radius: 20
                        blurSource: mapView
                        color: mapGlassFillStrong
                        visible: root.showVideoFeed || width > 2 || opacity > 0.02
                        opacity: root.showVideoFeed ? 1.0 : 0.0
                        scale: root.showVideoFeed ? 1.0 : 0.94
                        transformOrigin: Item.BottomRight

                        Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                        Behavior on height { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
                        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
                        Behavior on scale { SpringAnimation { spring: 4.5; damping: 0.42; mass: 0.9 } }

                        Image {
                            id: videoView
                            anchors.fill: parent
                            fillMode: Image.PreserveAspectCrop
                            cache: false
                            smooth: true
                            source: hasApp && app.cameraAvailable ? "image://video/live" : ""
                            visible: hasApp && app.cameraAvailable
                            opacity: root.showVideoFeed ? 1.0 : 0.0
                            Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
                        }

                        Rectangle {
                            anchors.fill: parent
                            color: Qt.rgba(0, 0, 0, 0.58)
                            visible: (!hasApp || !app.cameraAvailable) || videoView.status !== Image.Ready
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 48
                            color: Qt.rgba(0.04, 0.04, 0.04, 0.72)
                            clip: true

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 8

                                Text {
                                    Layout.fillWidth: true
                                    text: hasApp ? root.cameraStatusRu(app.cameraStatusDetail) : "Камера офлайн"
                                    color: textPrimary
                                    font.pixelSize: 12
                                    font.family: "Inter"
                                    elide: Text.ElideRight
                                }

                                GlassButton {
                                    label: "Скрыть"
                                    px: 12
                                    implicitWidth: 72
                                    implicitHeight: 32
                                    action: function() { root.showVideoFeed = false; }
                                }
                            }
                        }
                    }

                    MapGlassPane {
                        id: cameraRevealButton
                        property bool hovered: false
                        property bool pressed: false
                        width: 54
                        height: 54
                        radius: 27
                        visible: !root.showVideoFeed || opacity > 0.02
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 20
                        blurSource: mapView
                        color: mapGlassFillStrong
                        borderColor: mapGlassBorder
                        highlightOpacity: hovered ? 0.34 : 0.18
                        opacity: root.showVideoFeed ? 0.0 : 1.0
                        scale: root.showVideoFeed ? 0.82 : (pressed ? 0.92 : (hovered ? 1.08 : 1.0))
                        transformOrigin: Item.Center
                        z: 8
                        Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
                        Behavior on scale { SpringAnimation { spring: 5.0; damping: 0.38; mass: 0.75 } }

                        Item {
                            id: cameraRevealIcon
                            anchors.centerIn: parent
                            width: 22
                            height: 16
                            scale: cameraRevealButton.pressed ? 0.88 : (cameraRevealButton.hovered ? 1.12 : 1.0)
                            rotation: cameraRevealButton.hovered ? -2 : 0
                            Behavior on scale { SpringAnimation { spring: 6.0; damping: 0.40; mass: 0.7 } }
                            Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }

                            Rectangle {
                                anchors.fill: parent
                                radius: 4
                                color: "transparent"
                                border.color: cameraRevealButton.hovered || cameraRevealButton.pressed ? "#7bc6ff" : textPrimary
                                border.width: 2
                                antialiasing: true
                                Behavior on border.color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
                            }

                            Rectangle {
                                width: 6
                                height: 4
                                radius: 2
                                color: cameraRevealButton.hovered || cameraRevealButton.pressed ? "#7bc6ff" : textPrimary
                                anchors.left: parent.left
                                anchors.leftMargin: 2
                                anchors.top: parent.top
                                anchors.topMargin: -3
                                antialiasing: true
                                Behavior on color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
                            }

                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                anchors.centerIn: parent
                                color: "transparent"
                                border.color: cameraRevealButton.hovered || cameraRevealButton.pressed ? "#7bc6ff" : textPrimary
                                border.width: 2
                                antialiasing: true
                                Behavior on border.color { ColorAnimation { duration: 130; easing.type: Easing.OutQuad } }
                            }
                        }

                        MouseArea {
                            id: cameraRevealMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: cameraRevealButton.hovered = true
                            onExited: {
                                cameraRevealButton.hovered = false;
                                cameraRevealButton.pressed = false;
                            }
                            onPressed: cameraRevealButton.pressed = true
                            onReleased: cameraRevealButton.pressed = false
                            onClicked: root.showVideoFeed = true
                        }
                    }
                }
            }

            Item {
                Layout.preferredWidth: Math.max(300, Math.min(360, root.width * 0.24))
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    SectionCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(260, parent.height * 0.42)

                        ColumnLayout {
                            id: targetsColumn
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10

                            Text {
                                text: "Обнаруженные объекты"
                                color: textPrimary
                                font.pixelSize: 18
                                font.family: "Inter"
                                font.bold: true
                            }

                            ListView {
                                id: targetsView
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: hasApp ? app.confirmedObjects : []
                                spacing: 8

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

                                ScrollBar.vertical: ScrollBar {
                                    width: 8
                                    visible: size < 1.0
                                }
                            }
                        }
                    }

                    SectionCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        ColumnLayout {
                            id: logColumn
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 10

                            Text {
                                text: "Журнал системы"
                                color: textPrimary
                                font.pixelSize: 18
                                font.family: "Inter"
                                font.bold: true
                            }

                            ListView {
                                id: logView
                                Layout.fillWidth: true
                                Layout.preferredHeight: 360
                                clip: true
                                model: hasApp ? app.logs : []
                                spacing: 6
                                onCountChanged: positionViewAtEnd()

                                delegate: Rectangle {
                                    width: logColumn.width
                                    radius: 12
                                    color: Qt.rgba(1, 1, 1, 0.04)
                                    border.color: borderSoft
                                    border.width: 1
                                    implicitHeight: logText.implicitHeight + 18
                                    clip: true

                                    Text {
                                        id: logText
                                        anchors.fill: parent
                                        anchors.margins: 9
                                        text: modelData
                                        color: textMuted
                                        font.pixelSize: 11
                                        font.family: "Consolas"
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                ScrollBar.vertical: ScrollBar {
                                    width: 8
                                    visible: size < 1.0
                                }
                            }
                        }
                    }
                }
            }
        }

    }

    Component.onCompleted: {
        if (!hasApp) return;
        lastRouteEditMode = app.routeEditMode;
        app.setVideoVisible(showVideoFeed);
        runMapTool("setAppendMode", app.routeEditMode);
        if (app.routeEditMode) runMapTool("drawPath");
        if (app.mapRefreshNeeded) app.refreshMapView();
    }

    onClosing: function() {
        if (hasApp) app.setVideoVisible(false);
    }

    Component.onDestruction: {
        if (hasApp) app.setVideoVisible(false);
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: Math.min(420, toastTextLabel.implicitWidth + 34)
        height: 52
        radius: 18
        color: Qt.rgba(0.04, 0.05, 0.06, 0.88)
        border.color: Qt.rgba(1, 1, 1, 0.10)
        border.width: 1
        clip: true
        visible: toastVisible
        opacity: toastVisible ? 1 : 0
        z: 90

        Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutQuad } }

        Text {
            id: toastTextLabel
            anchors.centerIn: parent
            text: root.toastText
            color: textPrimary
            font.pixelSize: 13
            font.family: "Inter"
        }
    }

    Item {
        id: confirmPlanSheet
        anchors.fill: parent
        visible: confirmPlanSheetVisible
        opacity: confirmPlanSheetVisible ? 1 : 0
        z: 115

        Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.42)
        }

        SectionCard {
            width: Math.min(parent.width - 48, 500)
            height: confirmPlanColumn.implicitHeight + 36
            anchors.centerIn: parent
            color: panelBgSoft

            ColumnLayout {
                id: confirmPlanColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                Text {
                    text: hasApp && app.currentBackend === "unreal" ? "Подтвердить маршрут и начать полет" : "Подтвердить план"
                    color: textPrimary
                    font.pixelSize: 20
                    font.family: "Inter"
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: hasApp && app.currentBackend === "unreal"
                          ? "Маршрут будет принят, режим полета запустится сразу."
                          : "Принять текущий маршрут и зафиксировать его как активную миссию."
                    color: textMuted
                    font.pixelSize: 12
                    font.family: "Inter"
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Отмена"
                        action: function() { confirmPlanSheetVisible = false; }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: hasApp && app.currentBackend === "unreal" ? "Подтвердить и лететь" : "Подтвердить план"
                        accentButton: true
                        action: function() {
                            confirmPlanSheetVisible = false;
                            if (hasApp) app.confirmPlan();
                        }
                    }
                }
            }
        }
    }

    Item {
        id: orbitSelectionSheet
        anchors.fill: parent
        visible: orbitSelectionSheetVisible
        opacity: orbitSelectionSheetVisible ? 1 : 0
        z: 116

        Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.42)
        }

        SectionCard {
            width: Math.min(parent.width - 48, 560)
            height: Math.min(parent.height - 80, 620)
            anchors.centerIn: parent
            color: panelBgSoft

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                Text {
                    text: "Выбор целей для орбиты"
                    color: textPrimary
                    font.pixelSize: 20
                    font.family: "Inter"
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: "Выберите одну или несколько подтвержденных целей для облета."
                    color: textMuted
                    font.pixelSize: 12
                    font.family: "Inter"
                    wrapMode: Text.WordWrap
                }

                ListView {
                    id: orbitList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 8
                    model: hasApp ? app.confirmedObjects : []

                    delegate: Rectangle {
                        property bool selected: orbitSelection.indexOf(modelData.object_id) !== -1
                        width: orbitList.width
                        height: 74
                        radius: 14
                        color: selected ? Qt.rgba(0.22, 0.34, 0.44, 0.34) : Qt.rgba(1, 1, 1, 0.05)
                        border.color: selected ? Qt.rgba(0.73, 0.89, 1.0, 0.30) : borderSoft
                        border.width: 1
                        clip: true

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Text {
                                    text: "#" + modelData.object_id
                                    color: textPrimary
                                    font.pixelSize: 13
                                    font.family: "Inter"
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "Класс " + (modelData.class_id || 0) + "  Дост. " + (Number(modelData.confidence || 0) * 100).toFixed(1) + "%"
                                    color: textMuted
                                    font.pixelSize: 11
                                    font.family: "Inter"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                Text {
                                    text: "Шир " + Number(modelData.lat || 0).toFixed(5) + "  Долг " + Number(modelData.lon || 0).toFixed(5)
                                    color: textMuted
                                    font.pixelSize: 11
                                    font.family: "Inter"
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (hasApp) app.selectConfirmedObject(modelData.object_id);
                                toggleOrbitSelection(modelData.object_id);
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Отмена"
                        action: function() {
                            orbitSelectionSheetVisible = false;
                            orbitSelection = [];
                        }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Орбита по выбранным"
                        accentButton: true
                        enabled: orbitSelection.length > 0
                        action: function() {
                            if (!hasApp) return;
                            app.orbitSelectedObjects(orbitSelection);
                            orbitSelectionSheetVisible = false;
                            orbitSelection = [];
                        }
                    }
                }
            }
        }
    }

    Item {
        id: recoverableMissionSheet
        anchors.fill: parent
        visible: hasApp && app.recoverableMissionAvailable && app.currentBackend === "unreal" && app.unrealRuntimeStatus !== "disconnected"
        z: 114

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.22)
        }

        SectionCard {
            width: Math.min(parent.width - 64, 500)
            height: recoverableMissionColumn.implicitHeight + 30
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 68
            color: panelBgSoft

            ColumnLayout {
                id: recoverableMissionColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                Text {
                    text: "Сессия Unreal прервана"
                    color: textPrimary
                    font.pixelSize: 18
                    font.family: "Inter"
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: hasApp ? app.recoverableMissionText : ""
                    color: textMuted
                    font.pixelSize: 12
                    font.family: "Inter"
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Сбросить"
                        action: function() { if (hasApp) app.discardRecoverableMission(); }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Восстановить маршрут"
                        accentButton: true
                        action: function() { if (hasApp) app.restoreRecoverableMission(); }
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: hasApp && app.routeBatteryAdvisoryVisible
        z: 120

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.42)
        }

        SectionCard {
            width: Math.min(parent.width - 50, 520)
            height: routeBatteryColumn.implicitHeight + 34
            anchors.centerIn: parent
            color: panelBgSoft

            ColumnLayout {
                id: routeBatteryColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    text: "Предупреждение по батарее маршрута"
                    color: textPrimary
                    font.pixelSize: 20
                    font.family: "Inter"
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: hasApp ? app.routeBatteryAdvisoryText : ""
                    color: textMuted
                    font.pixelSize: 12
                    font.family: "Inter"
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Отмена"
                        action: function() { if (hasApp) app.respondRouteBatteryAdvisory("cancel"); }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Вернуться домой"
                        accentButton: true
                        enabled: hasApp ? app.routeBatteryReturnHomeAvailable : false
                        action: function() { if (hasApp) app.respondRouteBatteryAdvisory("rtl"); }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Продолжить"
                        warningButton: true
                        action: function() { if (hasApp) app.respondRouteBatteryAdvisory("proceed"); }
                    }
                }
            }
        }
    }

    Item {
        anchors.fill: parent
        visible: hasApp && app.orbitBatteryAdvisoryVisible
        z: 121

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.42)
        }

        SectionCard {
            width: Math.min(parent.width - 50, 520)
            height: orbitBatteryColumn.implicitHeight + 34
            anchors.centerIn: parent
            color: panelBgSoft

            ColumnLayout {
                id: orbitBatteryColumn
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Text {
                    text: "Предупреждение по батарее орбиты"
                    color: textPrimary
                    font.pixelSize: 20
                    font.family: "Inter"
                    font.bold: true
                }

                Text {
                    Layout.fillWidth: true
                    text: hasApp ? app.orbitBatteryAdvisoryText : ""
                    color: textMuted
                    font.pixelSize: 12
                    font.family: "Inter"
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Отмена"
                        action: function() { if (hasApp) app.respondOrbitBatteryAdvisory("cancel"); }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Вернуться домой"
                        accentButton: true
                        enabled: hasApp ? app.orbitBatteryReturnHomeAvailable : false
                        action: function() { if (hasApp) app.respondOrbitBatteryAdvisory("rtl"); }
                    }

                    GlassButton {
                        Layout.fillWidth: true
                        label: "Продолжить"
                        warningButton: true
                        action: function() { if (hasApp) app.respondOrbitBatteryAdvisory("proceed"); }
                    }
                }
            }
        }
    }

    FileDialog {
        id: geojsonDialog
        title: "ИМПОРТ GEOJSON"
        nameFilters: ["GeoJSON (*.geojson *.json)"]
        onAccepted: if (hasApp) app.importGeoJson(fileUrl.toLocalFile())
    }

    FileDialog {
        id: kmlDialog
        title: "ИМПОРТ KML"
        nameFilters: ["KML (*.kml)"]
        onAccepted: if (hasApp) app.importKml(fileUrl.toLocalFile())
    }
}




