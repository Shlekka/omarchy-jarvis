import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui

BarWidget {
  id: root
  moduleName: "io.github.shlekka.jarvis"

  Commons.Translations {
    id: strings
    directory: Qt.resolvedUrl("i18n")
  }

  readonly property var theme: Commons.ShellColor || Commons.Color
  readonly property var service: bar && bar.shell && typeof bar.shell.serviceFor === "function"
    ? bar.shell.serviceFor(moduleName) : null

  readonly property string sourceDir: {
    var url = String(Qt.resolvedUrl("."))
    if (url.indexOf("file://") !== 0) return ""
    try { url = decodeURIComponent(url.substring(7)) } catch (e) { return "" }
    return url.replace(/\/$/, "")
  }
  readonly property string ctlPath: sourceDir !== "" ? (sourceDir + "/bin/jarvis-ctl") : ""

  property bool fallbackRunning: false
  property bool fallbackInstalled: true
  readonly property bool running: service ? service.running : fallbackRunning
  readonly property bool installed: service ? service.installed : fallbackInstalled
  readonly property bool opened: running

  property bool autoStartDone: false
  readonly property bool autoStart: setting("autoStart", false) === true

  function ipc(args) { Quickshell.execDetached(["omarchy-shell", "jarvis"].concat(args)) }
  function toggle() { if (service) service.toggle(); else ipc(["toggle"]) }
  function start() { if (service) service.start(); else ipc(["start"]) }
  function stop() { if (service) service.stop(); else ipc(["stop"]) }
  function setup() { if (service) service.setup(); else ipc(["setup"]) }
  function open() { start() }
  function close() { stop() }

  // Fallback status poller when hosted inside a custom bar whose facade omits serviceFor().
  Process {
    id: fallbackStatusProc
    command: ["bash", root.ctlPath, "status"]
    running: false
    stdout: SplitParser {
      splitMarker: "\n"
      onRead: function(line) {
        try {
          var data = JSON.parse(line)
          if (data && typeof data === "object") {
            root.fallbackRunning = data.running === true
            root.fallbackInstalled = data.installed === true
          }
        } catch (e) {
          // Ignore partial output
        }
      }
    }
  }

  Timer {
    id: fallbackPoll
    interval: 3000
    repeat: true
    running: root.service === null && root.ctlPath !== ""
    triggeredOnStart: true
    onTriggered: if (!fallbackStatusProc.running) fallbackStatusProc.running = true
  }

  property var watching: null
  onServiceChanged: {
    if (watching) watching.watchStatus(false)
    watching = service
    if (watching) watching.watchStatus(true)
  }

  Component.onCompleted: {
    if (service) {
      watching = service
      watching.watchStatus(true)
    }
    if (autoStart && !autoStartDone) {
      autoStartDone = true
      autoStartTimer.start()
    }
  }

  Component.onDestruction: {
    if (watching) watching.watchStatus(false)
  }

  Timer {
    id: autoStartTimer
    interval: 1200
    repeat: false
    onTriggered: {
      if (root.installed && !root.running) root.start()
    }
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    active: root.running
    tooltipText: !root.installed
      ? strings.t("tooltip.uninstalled")
      : (root.running ? strings.t("tooltip.running") : strings.t("tooltip.standby"))

    iconComponent: Component {
      Item {
        id: coreIcon
        property color tint: root.running ? root.theme.bar.active : root.theme.bar.foreground

        // Outer holographic calibration ring
        Rectangle {
          anchors.centerIn: parent
          width: Math.min(parent.width, parent.height) * 0.92
          height: width
          radius: width / 2
          color: "transparent"
          border.width: Math.max(1.2, width * 0.09)
          border.color: coreIcon.tint
          opacity: root.running ? 1.0 : 0.78
          Behavior on opacity { NumberAnimation { duration: Style.duration(180); easing.type: Easing.OutCubic } }
        }

        // Inner neural ring
        Rectangle {
          anchors.centerIn: parent
          width: Math.min(parent.width, parent.height) * 0.56
          height: width
          radius: width / 2
          color: "transparent"
          border.width: Math.max(1.0, width * 0.13)
          border.color: coreIcon.tint
          opacity: root.running ? 0.95 : 0.6
          Behavior on opacity { NumberAnimation { duration: Style.duration(180); easing.type: Easing.OutCubic } }
        }

        // Central core dot
        Rectangle {
          anchors.centerIn: parent
          width: Math.min(parent.width, parent.height) * 0.26
          height: width
          radius: width / 2
          color: coreIcon.tint
          opacity: root.running ? 1.0 : 0.75
        }
      }
    }

    onPressed: function(buttonCode) {
      if (buttonCode === Qt.RightButton) root.setup()
      else root.toggle()
    }
  }

  // Subtle status indicator dot when J.A.R.V.I.S. is active
  Rectangle {
    visible: root.running
    anchors.top: button.top
    anchors.right: button.right
    anchors.topMargin: Style.space(4)
    anchors.rightMargin: Style.space(4)
    width: Style.space(5)
    height: Style.space(5)
    radius: width / 2
    color: root.theme.bar.active
  }
}
