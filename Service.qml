import QtQuick
import Quickshell
import Quickshell.Io

// Always-on Omarchy shell service for J.A.R.V.I.S.
// Tracks runtime state, exposes `omarchy-shell jarvis <cmd>` IPC actions,
// and manages the desktop launcher entry while the plugin is enabled.
Scope {
  id: root

  property string omarchyPath: ""
  property var shell: null
  property var manifest: null

  readonly property string sourceDir: {
    var url = String(Qt.resolvedUrl("."))
    if (url.indexOf("file://") !== 0) return ""
    try { url = decodeURIComponent(url.substring(7)) } catch (e) { return "" }
    return url.replace(/\/$/, "")
  }

  readonly property string ctlPath: sourceDir !== "" ? (sourceDir + "/bin/jarvis-ctl") : ""

  property bool running: false
  property bool installed: false
  property bool opened: running
  property int statusWatchers: 0

  function watchStatus(on) {
    statusWatchers = Math.max(0, statusWatchers + (on ? 1 : -1))
    if (statusWatchers > 0) refreshStatus()
  }

  function refreshStatus() {
    if (ctlPath === "" || statusProc.running) return
    statusProc.running = true
  }

  function runCtl(action) {
    if (ctlPath === "") return
    Quickshell.execDetached(["bash", ctlPath, action])
    refreshSoon.restart()
  }

  function start() { runCtl("start") }
  function stop() { runCtl("stop") }
  function toggle() { runCtl("toggle") }
  function setup() { runCtl("setup") }
  function show() { start() }
  function hide() { stop() }
  function open() { start() }
  function close() { stop() }

  Process {
    id: statusProc
    command: ["bash", root.ctlPath, "status"]
    running: false
    stdout: SplitParser {
      splitMarker: "\n"
      onRead: function(line) {
        try {
          var data = JSON.parse(line)
          if (data && typeof data === "object") {
            root.running = data.running === true
            root.installed = data.installed === true
          }
        } catch (e) {
          // Ignore partial output
        }
      }
    }
  }

  Timer {
    id: statusPoll
    interval: 2500
    repeat: true
    running: root.statusWatchers > 0 && root.ctlPath !== ""
    triggeredOnStart: true
    onTriggered: root.refreshStatus()
  }

  Timer {
    id: refreshSoon
    interval: 450
    repeat: false
    onTriggered: root.refreshStatus()
  }

  IpcHandler {
    target: "jarvis"
    function toggle(): void { root.toggle() }
    function start(): void { root.start() }
    function stop(): void { root.stop() }
    function setup(): void { root.setup() }
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.show() }
    function hide(): void { root.hide() }
  }

  // ---------------- launcher entry -----------------------------------------

  readonly property string desktopDest: Quickshell.env("HOME") + "/.local/share/applications/jarvis.desktop"
  readonly property string desktopMarker: "^X-Jarvis-Plugin-Managed=true$"

  readonly property string installDesktopScript:
      '[ -L "$1" ] && exit 0\n'
    + 'if [ -e "$1" ] && ! grep -q "$2" "$1"; then exit 0; fi\n'
    + 'mkdir -p "${1%/*}" || exit 0\n'
    + 'tmp=$(mktemp "$1.jarvis.XXXXXX") || exit 0\n'
    + 'cat >"$tmp" <<EOF\n'
    + '[Desktop Entry]\n'
    + 'Type=Application\n'
    + 'Name=J.A.R.V.I.S.\n'
    + 'Comment=Holographic desktop AI companion connected to Omarchy AI agents\n'
    + 'Exec=bash "$3" start\n'
    + 'Icon=$4\n'
    + 'Terminal=false\n'
    + 'Categories=Utility;Development;\n'
    + 'StartupWMClass=jarvis\n'
    + 'X-Jarvis-Plugin-Managed=true\n'
    + 'EOF\n'
    + 'chmod 644 "$tmp"\n'
    + 'if cmp -s "$tmp" "$1"; then rm -f "$tmp"; else mv -f "$tmp" "$1"; fi\n'

  readonly property string removeDesktopScript:
      '[ -L "$1" ] && exit 0\n'
    + 'grep -q "$2" "$1" 2>/dev/null && rm -f "$1"\n'

  property bool desktopInstalled: false

  Component.onCompleted: {
    if (sourceDir === "") return
    desktopInstalled = true
    Quickshell.execDetached([
      "sh", "-c", installDesktopScript, "sh",
      desktopDest, desktopMarker, ctlPath, sourceDir + "/pkgbuild/jarvis.svg"
    ])
    refreshStatus()
  }

  Component.onDestruction: {
    if (!desktopInstalled) return
    Quickshell.execDetached(["sh", "-c", removeDesktopScript, "sh", desktopDest, desktopMarker])
  }
}
