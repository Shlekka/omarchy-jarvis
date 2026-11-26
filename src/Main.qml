import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: win

    property bool hudExpanded: false
    property bool hudLarge: false
    property bool helpVisible: false
    property bool orbHovered: false

    readonly property bool anyHovered: orbHovered || rootHover.hovered || controlHover.hovered || agentPopup.visible
    readonly property bool showSpeakingBubble: !hudExpanded && backend.state === "speaking" && backend.caption.length > 0

    readonly property color activeAccent: {
        if (backend.state === "error") return "#EF4444"
        if (backend.state === "approval") return "#F59E0B"
        if (backend.state === "listening") return "#38BDF8"
        if (backend.state === "working") return "#22D3EE"
        if (backend.paletteMode === "cyan") return "#38BDF8"
        if (backend.paletteMode === "omarchy") return theme.accent
        return "#FFB020"
    }

    readonly property color paletteSwatch: {
        if (backend.paletteMode === "cyan") return "#38BDF8"
        if (backend.paletteMode === "omarchy") return theme.accent
        return "#FFB020"
    }

    readonly property int maxAvailWidth: Screen.desktopAvailableWidth > 0 ? Screen.desktopAvailableWidth - 48 : 1200
    readonly property int maxAvailHeight: Screen.desktopAvailableHeight > 0 ? Screen.desktopAvailableHeight - 64 : 920
    readonly property int compactWidth: Math.max(348, backend.orbSize + 52)
    readonly property int compactHeight: backend.orbSize + 124
    readonly property int expandedOrbSize: Math.min(156, Math.max(108, Math.round(backend.orbSize * 0.62)))
    readonly property int desiredHudWidth: hudLarge ? 860 : Math.max(660, expandedOrbSize + 480)
    readonly property int desiredHudHeight: hudLarge ? 860 : Math.max(740, expandedOrbSize + 560)
    readonly property int targetWidth: hudExpanded ? Math.min(maxAvailWidth, desiredHudWidth) : compactWidth
    readonly property int targetHeight: hudExpanded ? Math.min(maxAvailHeight, desiredHudHeight) : compactHeight

    function applyWindowSize() {
        var w = Math.max(220, Math.round(targetWidth))
        var h = Math.max(220, Math.round(targetHeight))
        // Relax min/max first so min never exceeds max while switching sizes on Wayland.
        minimumWidth = 220
        minimumHeight = 220
        maximumWidth = 16777215
        maximumHeight = 16777215
        width = w
        height = h
        minimumWidth = w
        maximumWidth = w
        minimumHeight = h
        maximumHeight = h
        backend.syncWindowSize(w, h)
    }

    width: targetWidth
    height: targetHeight
    visible: true
    title: "J.A.R.V.I.S."
    color: "transparent"

    flags: Qt.Window | Qt.FramelessWindowHint | (backend.alwaysOnTop ? Qt.WindowStaysOnTopHint : 0)

    Material.theme: Material.Dark
    Material.accent: activeAccent
    Material.roundedScale: Math.max(8, theme.radius)

    onTargetWidthChanged: applyWindowSize()
    onTargetHeightChanged: applyWindowSize()
    Component.onCompleted: applyWindowSize()

    Connections {
        target: backend
        function onApprovalRequested() {
            win.hudExpanded = true
            win.raise()
            win.requestActivate()
        }
    }

    Shortcut { sequence: "Ctrl+P"; context: Qt.ApplicationShortcut; onActivated: backend.toggleAlwaysOnTop() }
    Shortcut { sequence: "Ctrl+V"; context: Qt.ApplicationShortcut; onActivated: backend.toggleMic() }
    Shortcut { sequence: "Ctrl+M"; context: Qt.ApplicationShortcut; onActivated: backend.toggleMuted() }
    Shortcut { sequence: "Ctrl+H"; context: Qt.ApplicationShortcut; onActivated: win.hudExpanded = !win.hudExpanded }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; onActivated: backend.resetConversation() }
    Shortcut { sequence: "Ctrl++"; context: Qt.ApplicationShortcut; onActivated: backend.adjustOrbSize(20) }
    Shortcut { sequence: "Ctrl+="; context: Qt.ApplicationShortcut; onActivated: backend.adjustOrbSize(20) }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; onActivated: backend.adjustOrbSize(-20) }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: backend.resetOrbSize() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (agentPopup.visible) agentPopup.close()
            else if (win.helpVisible) win.helpVisible = false
            else if (backend.approvalPending) backend.cancelApproval()
            else if (win.hudExpanded) win.hudExpanded = false
        }
    }
    Shortcut {
        sequence: "?"
        context: Qt.ApplicationShortcut
        enabled: !promptInput.activeFocus
        onActivated: win.helpVisible = !win.helpVisible
    }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        HoverHandler { id: rootHover }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 8
            spacing: 6

            // Auto-hiding HUD control bar — completely invisible unless hovering or HUD is open.
            Rectangle {
                id: controlBar
                Layout.alignment: Qt.AlignHCenter
                implicitWidth: controlRow.implicitWidth + 14
                implicitHeight: 30
                radius: 15
                color: "#D60B0F17"
                border.width: 1
                border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.48)
                opacity: (win.anyHovered || win.hudExpanded) ? 1.0 : 0.0
                enabled: opacity > 0.1

                Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }

                HoverHandler { id: controlHover }

                RowLayout {
                    id: controlRow
                    anchors.centerIn: parent
                    spacing: 4

                    // Drag handle pill.
                    Rectangle {
                        implicitWidth: 22
                        implicitHeight: 22
                        radius: 11
                        color: "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "⋮⋮"
                            color: "#94A3B8"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.SizeAllCursor
                            onPressed: win.startSystemMove()
                        }

                        ToolTip.visible: dragHandleHover.hovered
                        ToolTip.text: qsTr("Drag to move · Scroll orb to resize")
                        HoverHandler { id: dragHandleHover }
                    }

                    // Agent selector button (follows Omarchy default agent or lets user pick).
                    Rectangle {
                        id: agentButton
                        implicitWidth: agentBtnLabel.implicitWidth + 14
                        implicitHeight: 22
                        radius: 11
                        color: agentPopup.visible ? "#44FFB020" : "#22FFFFFF"
                        border.width: 1
                        border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.5)

                        Text {
                            id: agentBtnLabel
                            anchors.centerIn: parent
                            text: backend.effectiveAgentShort + " ▾"
                            color: win.activeAccent
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.6
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                backend.refreshAgents()
                                if (agentPopup.visible) agentPopup.close()
                                else agentPopup.open()
                            }
                        }

                        ToolTip.visible: agentBtnHover.hovered
                        ToolTip.text: backend.selectedAgent === "auto"
                            ? qsTr("Agent: %1 (Omarchy default) — click to switch").arg(backend.effectiveAgentName)
                            : qsTr("Agent: %1 — click to switch").arg(backend.effectiveAgentName)
                        HoverHandler { id: agentBtnHover }
                    }

                    // Toggle hands-free microphone voice input.
                    Rectangle {
                        id: micButton
                        implicitWidth: micLabel.implicitWidth + 14
                        implicitHeight: 22
                        radius: 11
                        color: backend.micEnabled
                            ? (backend.micListening ? "#38BDF8" : "#4438BDF8")
                            : "#22FFFFFF"
                        border.width: 1
                        border.color: backend.micEnabled ? "#38BDF8" : "#33FFFFFF"

                        Text {
                            id: micLabel
                            anchors.centerIn: parent
                            text: backend.micEnabled ? qsTr("● MIC") : qsTr("MIC")
                            color: backend.micListening ? "#0B0F17" : (backend.micEnabled ? "#7DD3FC" : "#E2E8F0")
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.6
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.toggleMic()
                        }

                        ToolTip.visible: micHover.hovered
                        ToolTip.text: backend.micEnabled
                            ? qsTr("Microphone: LIVE — speak to J.A.R.V.I.S. (Ctrl+V)")
                            : qsTr("Microphone: OFF — click to speak directly (Ctrl+V)")
                        HoverHandler { id: micHover }
                    }

                    // Toggleable Always on Top button.
                    Rectangle {
                        id: pinButton
                        implicitWidth: pinLabel.implicitWidth + 12
                        implicitHeight: 22
                        radius: 11
                        color: backend.alwaysOnTop ? win.activeAccent : "#22FFFFFF"
                        border.width: 1
                        border.color: backend.alwaysOnTop ? win.activeAccent : "#33FFFFFF"

                        Text {
                            id: pinLabel
                            anchors.centerIn: parent
                            text: backend.alwaysOnTop ? qsTr("PINNED") : qsTr("TOP")
                            color: backend.alwaysOnTop ? "#0B0F17" : "#E2E8F0"
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.6
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.toggleAlwaysOnTop()
                        }

                        ToolTip.visible: pinHover.hovered
                        ToolTip.text: backend.alwaysOnTop
                            ? qsTr("Always on top: ON (Ctrl+P)")
                            : qsTr("Always on top: OFF (Ctrl+P)")
                        HoverHandler { id: pinHover }
                    }

                    // Mute / unmute offline British TTS.
                    Rectangle {
                        implicitWidth: 24
                        implicitHeight: 22
                        radius: 11
                        color: backend.muted ? "#55EF4444" : "#22FFFFFF"

                        Text {
                            anchors.centerIn: parent
                            text: backend.muted ? "✕" : "♪"
                            color: backend.muted ? "#FCA5A5" : "#E2E8F0"
                            font.pixelSize: 12
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.toggleMuted()
                        }

                        ToolTip.visible: muteHover.hovered
                        ToolTip.text: backend.muted ? qsTr("Unmute voice (Ctrl+M)") : qsTr("Mute voice (Ctrl+M)")
                        HoverHandler { id: muteHover }
                    }

                    // Toggle HUD chat drawer.
                    Rectangle {
                        implicitWidth: hudBtnText.implicitWidth + 12
                        implicitHeight: 22
                        radius: 11
                        color: win.hudExpanded ? "#44FFB020" : "#22FFFFFF"

                        Text {
                            id: hudBtnText
                            anchors.centerIn: parent
                            text: win.hudExpanded ? qsTr("ORB") : qsTr("HUD")
                            color: "#E2E8F0"
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.6
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                win.hudExpanded = !win.hudExpanded
                                if (win.hudExpanded) promptInput.forceActiveFocus()
                            }
                        }

                        ToolTip.visible: hudHover.hovered
                        ToolTip.text: qsTr("Toggle interactive HUD (Ctrl+H)")
                        HoverHandler { id: hudHover }
                    }

                    // Palette mode cycle (Gold / Cyan / Omarchy).
                    Rectangle {
                        implicitWidth: 22
                        implicitHeight: 22
                        radius: 11
                        color: "#22FFFFFF"

                        Rectangle {
                            anchors.centerIn: parent
                            width: 10
                            height: 10
                            radius: 5
                            color: win.paletteSwatch
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: backend.cyclePalette()
                        }

                        ToolTip.visible: palHover.hovered
                        ToolTip.text: qsTr("Theme: %1 (click to switch)").arg(backend.paletteMode.toUpperCase())
                        HoverHandler { id: palHover }
                    }

                    // Quit button.
                    Rectangle {
                        implicitWidth: 22
                        implicitHeight: 22
                        radius: 11
                        color: closeHover.hovered ? "#88EF4444" : "transparent"

                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: "#CBD5E1"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.quit()
                        }

                        ToolTip.visible: closeHover.hovered
                        ToolTip.text: qsTr("Quit (Ctrl+Q)")
                        HoverHandler { id: closeHover }
                    }
                }
            }

            // Holographic JARVIS core — drag to move, scroll wheel to resize, click to open HUD.
            Item {
                id: orbContainer
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: win.hudExpanded ? win.expandedOrbSize : backend.orbSize
                Layout.preferredHeight: win.hudExpanded ? win.expandedOrbSize : backend.orbSize

                Behavior on Layout.preferredWidth { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                Behavior on Layout.preferredHeight { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

                JarvisCore {
                    anchors.fill: parent
                    stateName: backend.state
                    audioLevel: backend.audioLevel
                    paletteMode: backend.paletteMode
                    themeAccent: theme.accent
                    hovered: win.orbHovered
                }

                // Status badge auto-hides completely when idle and not hovered.
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    implicitWidth: statusBadgeText.implicitWidth + 14
                    implicitHeight: 18
                    radius: 9
                    color: "#D90B0F17"
                    border.width: 1
                    border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.55)
                    opacity: (win.anyHovered || win.hudExpanded || backend.state !== "idle") ? 1.0 : 0.0

                    Behavior on opacity { NumberAnimation { duration: 180 } }

                    Text {
                        id: statusBadgeText
                        anchors.centerIn: parent
                        text: backend.activeTool.length > 0
                            ? backend.activeTool
                            : (backend.effectiveAgentShort + " · " + backend.state.toUpperCase())
                        color: win.activeAccent
                        font.pixelSize: 9
                        font.bold: true
                        font.letterSpacing: 1.0
                    }
                }

                // Drag to move, scroll to resize, clean click to toggle HUD.
                MouseArea {
                    id: orbMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.OpenHandCursor

                    property point pressPos: Qt.point(0, 0)
                    property bool moved: false

                    onEntered: win.orbHovered = true
                    onExited: win.orbHovered = false

                    onWheel: function(wheel) {
                        var delta = wheel.angleDelta.y > 0 ? 14 : (wheel.angleDelta.y < 0 ? -14 : 0)
                        if (delta !== 0) {
                            backend.adjustOrbSize(delta)
                            wheel.accepted = true
                        }
                    }

                    onPressed: function(mouse) {
                        pressPos = Qt.point(mouse.x, mouse.y)
                        moved = false
                    }

                    onPositionChanged: function(mouse) {
                        if (!pressed || moved) return
                        var dx = mouse.x - pressPos.x
                        var dy = mouse.y - pressPos.y
                        if ((dx * dx + dy * dy) > 36) {
                            moved = true
                            win.startSystemMove()
                        }
                    }

                    onReleased: {
                        if (!moved) {
                            win.hudExpanded = !win.hudExpanded
                            if (win.hudExpanded) promptInput.forceActiveFocus()
                        }
                    }
                }
            }

            // Speech bubble — ONLY appears while J.A.R.V.I.S. is actively speaking.
            Rectangle {
                id: speechBubble
                Layout.fillWidth: true
                Layout.maximumWidth: Math.max(256, backend.orbSize + 40)
                Layout.alignment: Qt.AlignHCenter
                implicitHeight: captionText.implicitHeight + 12
                radius: 10
                color: "#D90B0F17"
                border.width: 1
                border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.45)
                opacity: win.showSpeakingBubble ? 1.0 : 0.0
                visible: opacity > 0.01

                Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutQuad } }

                Text {
                    id: captionText
                    anchors.fill: parent
                    anchors.margins: 6
                    text: backend.caption
                    color: "#F1F5F9"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            // Expandable Interactive HUD Drawer.
            Rectangle {
                id: hudPanel
                visible: win.hudExpanded
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Math.max(14, theme.radius)
                color: "#EE0A0E16"
                border.width: 1
                border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.55)

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: backend.hookConnected ? "#22C55E" : "#94A3B8"
                        }

                        Text {
                            text: backend.effectiveAgentName.toUpperCase() + qsTr(" UPLINK")
                            color: win.activeAccent
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 1.1
                        }

                        Item { Layout.fillWidth: true }

                        Rectangle {
                            visible: backend.effectiveAgent === "agy"
                            implicitWidth: modelPillText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: backend.acpModel.indexOf("flash") >= 0 ? "#2A22D3EE" : "#1AFFFFFF"
                            border.width: 1
                            border.color: backend.acpModel.indexOf("flash") >= 0 ? "#6622D3EE" : "#22FFFFFF"

                            Text {
                                id: modelPillText
                                anchors.centerIn: parent
                                text: backend.acpModelShort
                                color: backend.acpModel.indexOf("flash") >= 0 ? "#67E8F9" : "#CBD5E1"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.cycleAcpModel()
                            }

                            ToolTip.visible: modelPillHover.hovered
                            ToolTip.text: qsTr("Model: %1 (click to cycle Flash Low / Neon Low / Neon High)").arg(backend.acpModel)
                            HoverHandler { id: modelPillHover }
                        }

                        Rectangle {
                            implicitWidth: autoApproveText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: backend.autoApprove ? "#2AF59E0B" : "#1AFFFFFF"
                            border.width: 1
                            border.color: backend.autoApprove ? "#66F59E0B" : "#22FFFFFF"

                            Text {
                                id: autoApproveText
                                anchors.centerIn: parent
                                text: backend.autoApprove ? qsTr("AUTO: ON") : qsTr("AUTO: OFF")
                                color: backend.autoApprove ? "#FCD34D" : "#94A3B8"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.toggleAutoApprove()
                            }

                            ToolTip.visible: autoApproveHover.hovered
                            ToolTip.text: backend.autoApprove
                                ? qsTr("Auto-approve: ON — executing actions without asking for approval")
                                : qsTr("Auto-approve: OFF — will ask for approval before running actions")
                            HoverHandler { id: autoApproveHover }
                        }

                        Rectangle {
                            implicitWidth: hooksText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: backend.voiceHooksEnabled ? "#2A22C55E" : "#1AFFFFFF"
                            border.width: 1
                            border.color: backend.voiceHooksEnabled ? "#5522C55E" : "#22FFFFFF"

                            Text {
                                id: hooksText
                                anchors.centerIn: parent
                                text: backend.voiceHooksEnabled ? qsTr("HOOK VOICE: ON") : qsTr("HOOK VOICE: OFF")
                                color: backend.voiceHooksEnabled ? "#86EFAC" : "#94A3B8"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.toggleVoiceHooks()
                            }
                        }

                        Rectangle {
                            implicitWidth: sizeToggleText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: win.hudLarge ? "#33FFB020" : "#1AFFFFFF"
                            border.width: 1
                            border.color: win.hudLarge
                                ? Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.65)
                                : "#22FFFFFF"

                            Text {
                                id: sizeToggleText
                                anchors.centerIn: parent
                                text: win.hudLarge ? qsTr("COMPACT") : qsTr("EXPAND")
                                color: win.hudLarge ? win.activeAccent : "#CBD5E1"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: win.hudLarge = !win.hudLarge
                            }
                        }

                        Rectangle {
                            implicitWidth: resetText.implicitWidth + 14
                            implicitHeight: 24
                            radius: 12
                            color: "#1AFFFFFF"
                            border.width: 1
                            border.color: "#22FFFFFF"

                            Text {
                                id: resetText
                                anchors.centerIn: parent
                                text: qsTr("NEW")
                                color: "#CBD5E1"
                                font.pixelSize: 10
                                font.bold: true
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.resetConversation()
                            }
                        }
                    }

                    ListView {
                        id: feedView
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 10
                        model: backend.messages
                        boundsBehavior: Flickable.StopAtBounds

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                            width: 8
                        }

                        onCountChanged: Qt.callLater(positionViewAtEnd)

                        delegate: Item {
                            width: feedView.width - 10
                            implicitHeight: bubble.implicitHeight

                            Rectangle {
                                id: bubble
                                width: parent.width
                                implicitHeight: msgCol.implicitHeight + 22
                                radius: 10
                                color: {
                                    if (modelData.role === "user") return "#26FFB020"
                                    if (modelData.role === "telemetry") return "#1A22D3EE"
                                    return "#1EFFFFFF"
                                }
                                border.width: 1
                                border.color: {
                                    if (modelData.role === "user") return "#50FFB020"
                                    if (modelData.role === "telemetry") return "#3A22D3EE"
                                    return "#2AFFFFFF"
                                }

                                ColumnLayout {
                                    id: msgCol
                                    anchors.fill: parent
                                    anchors.margins: 11
                                    spacing: 5

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: {
                                                if (modelData.role === "user") return qsTr("SIR")
                                                if (modelData.role === "telemetry") return (modelData.meta || "TELEMETRY")
                                                return modelData.meta && modelData.meta.length > 0
                                                    ? ("J.A.R.V.I.S. · " + modelData.meta)
                                                    : "J.A.R.V.I.S."
                                            }
                                            color: modelData.role === "telemetry" ? "#67E8F9" : win.activeAccent
                                            font.pixelSize: 11
                                            font.bold: true
                                            font.letterSpacing: 0.8
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: modelData.time || ""
                                            color: "#94A3B8"
                                            font.pixelSize: 11
                                        }
                                    }

                                    TextEdit {
                                        Layout.fillWidth: true
                                        text: modelData.text || ""
                                        textFormat: TextEdit.PlainText
                                        readOnly: true
                                        selectByMouse: true
                                        color: "#F8FAFC"
                                        selectionColor: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.38)
                                        selectedTextColor: "#FFFFFF"
                                        font.pixelSize: 14
                                        wrapMode: TextEdit.Wrap
                                    }
                                }
                            }
                        }
                    }

                    // Interactive Approval Prompt Card (shown when J.A.R.V.I.S. needs user permission).
                    Rectangle {
                        id: approvalCard
                        visible: backend.approvalPending
                        Layout.fillWidth: true
                        implicitHeight: approvalCol.implicitHeight + 24
                        radius: 12
                        color: "#2E1C0E"
                        border.width: 1.5
                        border.color: "#F59E0B"

                        ColumnLayout {
                            id: approvalCol
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Rectangle {
                                    width: 9
                                    height: 9
                                    radius: 4.5
                                    color: "#F59E0B"
                                }

                                Text {
                                    text: qsTr("APPROVAL REQUIRED")
                                    color: "#FBBF24"
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 1.1
                                }

                                Rectangle {
                                    visible: (backend.pendingApproval.kindLabel || "").length > 0
                                    implicitWidth: kindPillText.implicitWidth + 12
                                    implicitHeight: 20
                                    radius: 10
                                    color: "#33F59E0B"
                                    border.width: 1
                                    border.color: "#66F59E0B"

                                    Text {
                                        id: kindPillText
                                        anchors.centerIn: parent
                                        text: backend.pendingApproval.kindLabel || ""
                                        color: "#FDE68A"
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    visible: backend.micEnabled
                                    text: qsTr("Say \"approve\" or \"deny\"")
                                    color: "#94A3B8"
                                    font.pixelSize: 10
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: backend.pendingApproval.title || qsTr("Confirm action")
                                color: "#FEF3C7"
                                font.pixelSize: 14
                                font.bold: true
                                wrapMode: Text.WordWrap
                            }

                            Rectangle {
                                visible: (backend.pendingApproval.detail || "").length > 0
                                Layout.fillWidth: true
                                implicitHeight: Math.min(110, detailEdit.implicitHeight + 14)
                                radius: 8
                                color: "#AA0A0E16"
                                border.width: 1
                                border.color: "#33F59E0B"
                                clip: true

                                ScrollView {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                                    TextEdit {
                                        id: detailEdit
                                        width: parent.availableWidth
                                        text: backend.pendingApproval.detail || ""
                                        textFormat: TextEdit.PlainText
                                        readOnly: true
                                        selectByMouse: true
                                        color: "#E2E8F0"
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        wrapMode: TextEdit.Wrap
                                        padding: 4
                                    }
                                }
                            }

                            Flow {
                                Layout.fillWidth: true
                                spacing: 8

                                Repeater {
                                    model: backend.pendingApproval.options || []

                                    delegate: Rectangle {
                                        readonly property string optKind: modelData.kind || "allow_once"
                                        readonly property bool isReject: optKind.indexOf("reject") === 0
                                        readonly property bool isAlways: optKind === "allow_always"

                                        implicitWidth: optLabel.implicitWidth + 24
                                        implicitHeight: 34
                                        radius: 8
                                        color: {
                                            if (isReject) return optHover.hovered ? "#7F1D1D" : "#451A1A"
                                            if (isAlways) return optHover.hovered ? "#1D4ED8" : "#1E3A5F"
                                            return optHover.hovered ? "#15803D" : "#166534"
                                        }
                                        border.width: 1
                                        border.color: {
                                            if (isReject) return "#EF4444"
                                            if (isAlways) return "#60A5FA"
                                            return "#22C55E"
                                        }

                                        Text {
                                            id: optLabel
                                            anchors.centerIn: parent
                                            text: modelData.name || modelData.optionId
                                            color: "#F8FAFC"
                                            font.pixelSize: 12
                                            font.bold: true
                                        }

                                        HoverHandler { id: optHover }
                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: backend.respondToApproval(modelData.optionId)
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Repeater {
                            model: [
                                { label: qsTr("Greeting"), action: "greet" },
                                { label: qsTr("Workspace Status"), action: "prompt", prompt: "Give me a concise status summary of my ~/Work workspace." },
                                { label: qsTr("Stop Audio"), action: "stop" }
                            ]

                            delegate: Rectangle {
                                implicitWidth: chipLabel.implicitWidth + 16
                                implicitHeight: 26
                                radius: 13
                                color: chipHover.hovered ? "#33FFB020" : "#18FFFFFF"
                                border.width: 1
                                border.color: "#2AFFFFFF"

                                Text {
                                    id: chipLabel
                                    anchors.centerIn: parent
                                    text: modelData.label
                                    color: "#E2E8F0"
                                    font.pixelSize: 11
                                }

                                HoverHandler { id: chipHover }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (modelData.action === "greet") backend.greet()
                                        else if (modelData.action === "stop") backend.stopSpeaking()
                                        else backend.sendPrompt(modelData.prompt)
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignBottom
                        spacing: 8

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(128, Math.max(46, Math.max(promptInput.contentHeight, promptPlaceholder.visible ? promptPlaceholder.implicitHeight : 0) + 20))
                            radius: 10
                            color: "#1E141C2B"
                            border.width: 1
                            border.color: promptInput.activeFocus
                                ? Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.85)
                                : "#33FFFFFF"

                            Text {
                                id: promptPlaceholder
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 11
                                text: backend.busy
                                    ? qsTr("Processing directive...")
                                    : (backend.micEnabled
                                        ? qsTr("Mic active — speak or type (%1)... (Shift+Enter for newline)").arg(backend.effectiveAgentName)
                                        : qsTr("Ask J.A.R.V.I.S. (%1)... (Enter to send, Shift+Enter for newline)").arg(backend.effectiveAgentName))
                                color: "#94A3B8"
                                font: promptInput.font
                                wrapMode: Text.WordWrap
                                visible: promptInput.length === 0 && promptInput.preeditText.length === 0
                            }

                            ScrollView {
                                id: promptScroll
                                anchors.fill: parent
                                anchors.margins: 2
                                clip: true
                                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                                TextArea {
                                    id: promptInput
                                    width: promptScroll.availableWidth
                                    placeholderText: ""
                                    enabled: !backend.busy
                                    font.pixelSize: 14
                                    font.capitalization: Font.MixedCase
                                    inputMethodHints: Qt.ImhNoAutoUppercase
                                    color: "#F8FAFC"
                                    wrapMode: TextArea.Wrap
                                    topInset: 0
                                    bottomInset: 0
                                    leftPadding: 10
                                    rightPadding: 10
                                    topPadding: 9
                                    bottomPadding: 9
                                    background: Item {}

                                    Keys.onReturnPressed: function(event) {
                                        if (event.modifiers & Qt.ShiftModifier) {
                                            event.accepted = false
                                        } else {
                                            event.accepted = true
                                            if (text.trim().length > 0 && !backend.busy) {
                                                backend.sendPrompt(text)
                                                text = ""
                                            }
                                        }
                                    }
                                    Keys.onEnterPressed: function(event) {
                                        if (event.modifiers & Qt.ShiftModifier) {
                                            event.accepted = false
                                        } else {
                                            event.accepted = true
                                            if (text.trim().length > 0 && !backend.busy) {
                                                backend.sendPrompt(text)
                                                text = ""
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Button {
                            Layout.alignment: Qt.AlignBottom
                            implicitHeight: 46
                            text: backend.micEnabled ? qsTr("● Mic") : qsTr("Mic")
                            font.pixelSize: 13
                            highlighted: backend.micEnabled
                            onClicked: backend.toggleMic()
                        }

                        Button {
                            Layout.alignment: Qt.AlignBottom
                            implicitHeight: 46
                            text: backend.busy ? qsTr("Stop") : qsTr("Send")
                            font.pixelSize: 13
                            highlighted: !backend.busy
                            onClicked: {
                                if (backend.busy) {
                                    backend.cancelPrompt()
                                } else if (promptInput.text.trim().length > 0) {
                                    backend.sendPrompt(promptInput.text)
                                    promptInput.text = ""
                                }
                            }
                        }
                    }
                }
            }

            Item { Layout.fillHeight: !win.hudExpanded }
        }

        // Agent selector popover (Omarchy default + installed/supported agents).
        Popup {
            id: agentPopup
            x: Math.max(8, Math.min(parent.width - width - 8, (parent.width - width) / 2))
            y: controlBar.y + controlBar.height + 6
            width: Math.min(parent.width - 16, 260)
            padding: 6
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                radius: 10
                color: "#F40A0E16"
                border.width: 1
                border.color: Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.65)
            }

            contentItem: ColumnLayout {
                spacing: 3

                Text {
                    Layout.leftMargin: 6
                    Layout.topMargin: 2
                    Layout.bottomMargin: 2
                    text: qsTr("SELECT AI AGENT")
                    color: win.activeAccent
                    font.pixelSize: 9
                    font.bold: true
                    font.letterSpacing: 1.1
                }

                ListView {
                    Layout.fillWidth: true
                    implicitHeight: Math.min(230, contentHeight)
                    clip: true
                    spacing: 2
                    model: backend.availableAgents

                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 28
                        radius: 6
                        readonly property bool isSelected: backend.selectedAgent === modelData.id
                        color: isSelected
                            ? Qt.rgba(win.activeAccent.r, win.activeAccent.g, win.activeAccent.b, 0.24)
                            : (rowHover.hovered ? "#1EFFFFFF" : "transparent")
                        border.width: isSelected ? 1 : 0
                        border.color: win.activeAccent

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 7

                            Rectangle {
                                width: 6
                                height: 6
                                radius: 3
                                color: modelData.installed ? "#22C55E" : "#475569"
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.name
                                color: isSelected ? "#FFFFFF" : (modelData.installed ? "#E2E8F0" : "#94A3B8")
                                font.pixelSize: 11
                                font.bold: isSelected
                                elide: Text.ElideRight
                            }

                            Text {
                                visible: !modelData.installed
                                text: qsTr("uninstalled")
                                color: "#64748B"
                                font.pixelSize: 9
                            }
                        }

                        HoverHandler { id: rowHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                backend.selectAgent(modelData.id)
                                agentPopup.close()
                            }
                        }
                    }
                }
            }
        }

        // Keyboard shortcuts help overlay (`?`).
        Rectangle {
            visible: win.helpVisible
            anchors.centerIn: parent
            width: Math.min(parent.width - 24, 300)
            implicitHeight: helpCol.implicitHeight + 24
            radius: 12
            color: "#F20A0E16"
            border.width: 1
            border.color: win.activeAccent

            ColumnLayout {
                id: helpCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Text {
                    text: qsTr("J.A.R.V.I.S. CONTROLS")
                    color: win.activeAccent
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 1.0
                }
                Text { text: qsTr("Drag orb — Move anywhere on desktop"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Scroll on orb / Ctrl+± — Resize J.A.R.V.I.S."); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Hover orb — Reveal HUD & agent switcher"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Click orb / Ctrl+H — Expand chat HUD"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Ctrl+P — Toggle Always on Top"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Ctrl+V — Toggle microphone voice input"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Ctrl+M — Mute / unmute British voice"); color: "#E2E8F0"; font.pixelSize: 11 }
                Text { text: qsTr("Ctrl+Q — Quit"); color: "#E2E8F0"; font.pixelSize: 11 }
            }
        }
    }
}
