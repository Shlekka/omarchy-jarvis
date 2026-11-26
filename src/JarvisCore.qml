import QtQuick

Item {
    id: root

    property string stateName: "idle"
    property double audioLevel: 0.0
    property string paletteMode: "gold"
    property string themeAccent: "#FFB020"
    property bool hovered: false

    property double phase: 0.0
    property double smoothAudio: 0.0

    // Precompute a 34-node Fibonacci sphere for the 3D JARVIS neural matrix.
    readonly property var sphereNodes: {
        var nodes = []
        var count = 34
        var golden = Math.PI * (3.0 - Math.sqrt(5.0))
        for (var i = 0; i < count; i++) {
            var y = 1.0 - (i / (count - 1)) * 2.0
            var r = Math.sqrt(Math.max(0.0, 1.0 - y * y))
            var theta = golden * i
            nodes.push({
                x: Math.cos(theta) * r,
                y: y,
                z: Math.sin(theta) * r,
                seed: (i * 1.6180339) % 6.28318
            })
        }
        return nodes
    }

    function primaryColor() {
        if (stateName === "error") return "#EF4444"
        if (stateName === "approval") return "#F59E0B"
        if (stateName === "listening") return "#38BDF8"
        if (stateName === "working") return "#22D3EE"
        if (paletteMode === "cyan") return "#38BDF8"
        if (paletteMode === "omarchy") return themeAccent
        return "#FFB020"
    }

    function secondaryColor() {
        if (stateName === "error") return "#991B1B"
        if (stateName === "approval") return "#B45309"
        if (stateName === "listening") return "#0284C7"
        if (stateName === "working") return "#F59E0B"
        if (paletteMode === "cyan") return "#0284C7"
        if (paletteMode === "omarchy") return themeAccent
        return "#E07A00"
    }

    function coreColor() {
        if (stateName === "error") return "#FEE2E2"
        if (stateName === "approval") return "#FEF3C7"
        if (stateName === "listening") return "#E0F2FE"
        if (paletteMode === "cyan") return "#E0F7FF"
        return "#FFF5D6"
    }

    function hexToRgba(hex, alpha) {
        var c = Qt.color(hex)
        return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255) + "," + alpha.toFixed(3) + ")"
    }

    Timer {
        interval: 16
        running: root.visible
        repeat: true
        onTriggered: {
            var speed = 0.014
            if (root.stateName === "thinking") speed = 0.036
            else if (root.stateName === "approval") speed = 0.030
            else if (root.stateName === "working") speed = 0.028
            else if (root.stateName === "speaking") speed = 0.020 + root.smoothAudio * 0.024
            else if (root.stateName === "error") speed = 0.032
            else if (root.hovered) speed = 0.020

            root.phase += speed
            var targetAudio = (root.stateName === "speaking" || root.stateName === "listening")
                ? root.audioLevel
                : (root.stateName === "approval"
                    ? 0.24 + 0.16 * Math.sin(root.phase * 5.0)
                    : (root.stateName === "thinking" ? 0.18 + 0.10 * Math.sin(root.phase * 4.5) : 0.0))
            root.smoothAudio = root.smoothAudio * 0.72 + targetAudio * 0.28
            canvas.requestPaint()
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        renderStrategy: Canvas.Threaded

        onPaint: {
            var ctx = getContext("2d")
            var w = width
            var h = height
            if (w <= 0 || h <= 0) return

            ctx.clearRect(0, 0, w, h)

            var cx = w * 0.5
            var cy = h * 0.5
            var R = Math.min(w, h) * 0.45
            var t = root.phase
            var amp = root.smoothAudio

            var colPrimary = root.primaryColor()
            var colSecondary = root.secondaryColor()
            var colCore = root.coreColor()

            // 1. Ambient holographic backdrop halo (subtle dark contrast + luminous glow).
            var bgGrad = ctx.createRadialGradient(cx, cy, R * 0.08, cx, cy, R * 1.05)
            bgGrad.addColorStop(0.0, root.hexToRgba(colPrimary, 0.28 + amp * 0.22))
            bgGrad.addColorStop(0.42, "rgba(10, 14, 22, 0.58)")
            bgGrad.addColorStop(0.76, root.hexToRgba(colSecondary, 0.14 + amp * 0.10))
            bgGrad.addColorStop(1.0, "rgba(0, 0, 0, 0.0)")
            ctx.fillStyle = bgGrad
            ctx.beginPath()
            ctx.arc(cx, cy, R * 1.05, 0, Math.PI * 2)
            ctx.fill()

            // 2. Outer calibration dial (72 ticks + 4 quadrant brackets).
            ctx.save()
            ctx.translate(cx, cy)
            ctx.rotate(t * 0.15)
            var tickR = R * 0.94
            for (var i = 0; i < 72; i++) {
                var a = (i / 72.0) * Math.PI * 2
                var major = (i % 6 === 0)
                var len = major ? R * 0.065 : R * 0.03
                ctx.strokeStyle = root.hexToRgba(colPrimary, major ? 0.65 : 0.25)
                ctx.lineWidth = major ? 1.5 : 0.9
                ctx.beginPath()
                ctx.moveTo(Math.cos(a) * (tickR - len), Math.sin(a) * (tickR - len))
                ctx.lineTo(Math.cos(a) * tickR, Math.sin(a) * tickR)
                ctx.stroke()
            }
            ctx.restore()

            // 3. Outer counter-rotating segmented arcs.
            ctx.save()
            ctx.translate(cx, cy)
            ctx.rotate(-t * 0.42)
            var outerR = R * 0.84
            var segs = [
                [0.0, 1.05, 2.2, 0.85],
                [1.45, 2.15, 1.3, 0.45],
                [2.55, 3.85, 2.2, 0.85],
                [4.25, 4.95, 1.3, 0.45],
                [5.35, 5.95, 1.8, 0.65]
            ]
            for (var s = 0; s < segs.length; s++) {
                ctx.strokeStyle = root.hexToRgba(colPrimary, segs[s][3])
                ctx.lineWidth = segs[s][2]
                ctx.beginPath()
                ctx.arc(0, 0, outerR, segs[s][0], segs[s][1])
                ctx.stroke()
            }
            ctx.restore()

            // 4. Mid rotating tactical ring + telemetry sweep when thinking or working.
            ctx.save()
            ctx.translate(cx, cy)
            ctx.rotate(t * 0.68)
            var midR = R * 0.72
            ctx.strokeStyle = root.hexToRgba(colSecondary, 0.55)
            ctx.lineWidth = 1.4
            for (var m = 0; m < 6; m++) {
                var ma = m * (Math.PI / 3.0)
                ctx.beginPath()
                ctx.arc(0, 0, midR, ma + 0.12, ma + 0.82)
                ctx.stroke()
            }

            if (root.stateName === "thinking" || root.stateName === "working") {
                ctx.strokeStyle = root.hexToRgba(colCore, 0.88)
                ctx.lineWidth = 2.6
                ctx.beginPath()
                ctx.arc(0, 0, midR + 3, 0, 1.15)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(0, 0, midR + 3, Math.PI, Math.PI + 0.85)
                ctx.stroke()
            }
            ctx.restore()

            // 5. Audio-reactive radial equalizer filaments around the neural sphere.
            var eqCount = 44
            var eqInner = R * 0.52
            for (var e = 0; e < eqCount; e++) {
                var ea = (e / eqCount) * Math.PI * 2 + t * 0.22
                var harmonic = 0.5 + 0.5 * Math.sin(e * 0.75 - t * 6.5) * Math.cos(e * 0.35 + t * 3.1)
                var barLen = R * (0.04 + amp * (0.14 + 0.16 * harmonic))
                if (root.stateName === "idle") {
                    barLen = R * (0.028 + 0.015 * Math.sin(e * 0.5 + t * 2.0))
                }
                ctx.strokeStyle = root.hexToRgba(colPrimary, 0.32 + amp * 0.58)
                ctx.lineWidth = 1.6
                ctx.beginPath()
                ctx.moveTo(cx + Math.cos(ea) * eqInner, cy + Math.sin(ea) * eqInner)
                ctx.lineTo(cx + Math.cos(ea) * (eqInner + barLen), cy + Math.sin(ea) * (eqInner + barLen))
                ctx.stroke()
            }

            // 6. 3D-projected geodesic JARVIS neural sphere.
            var sphereR = R * (0.44 + amp * 0.065 + 0.015 * Math.sin(t * 2.2))
            var rotY = t * 0.85
            var rotX = Math.sin(t * 0.45) * 0.55
            var cosY = Math.cos(rotY), sinY = Math.sin(rotY)
            var cosX = Math.cos(rotX), sinX = Math.sin(rotX)

            var projected = []
            var nodes = root.sphereNodes
            for (var n = 0; n < nodes.length; n++) {
                var nd = nodes[n]
                // Pulse nodes gently along their normals when active.
                var radialPulse = 1.0 + amp * 0.12 * Math.sin(t * 7.0 + nd.seed)
                var x0 = nd.x * radialPulse
                var y0 = nd.y * radialPulse
                var z0 = nd.z * radialPulse

                var x1 = x0 * cosY - z0 * sinY
                var z1 = x0 * sinY + z0 * cosY
                var y2 = y0 * cosX - z1 * sinX
                var z2 = y0 * sinX + z1 * cosX

                var persp = 2.4 / (2.4 - z2 * 0.65)
                projected.push({
                    sx: cx + x1 * sphereR * persp,
                    sy: cy + y2 * sphereR * persp,
                    z: z2,
                    alpha: Math.max(0.14, Math.min(1.0, (z2 + 1.25) / 2.25)),
                    size: (1.5 + persp * 1.15) * (1.0 + amp * 0.35)
                })
            }

            // Draw neural filaments between nearby 3D points.
            var maxDist = sphereR * 0.78
            var maxDistSq = maxDist * maxDist
            ctx.lineWidth = 0.85
            for (var aIdx = 0; aIdx < projected.length; aIdx++) {
                var pa = projected[aIdx]
                for (var bIdx = aIdx + 1; bIdx < projected.length; bIdx++) {
                    var pb = projected[bIdx]
                    var dx = pa.sx - pb.sx
                    var dy = pa.sy - pb.sy
                    var d2 = dx * dx + dy * dy
                    if (d2 < maxDistSq) {
                        var prox = 1.0 - Math.sqrt(d2) / maxDist
                        var lineAlpha = prox * Math.min(pa.alpha, pb.alpha) * (0.48 + amp * 0.42)
                        ctx.strokeStyle = root.hexToRgba(colPrimary, lineAlpha)
                        ctx.beginPath()
                        ctx.moveTo(pa.sx, pa.sy)
                        ctx.lineTo(pb.sx, pb.sy)
                        ctx.stroke()
                    }
                }
            }

            // Draw glowing neural nodes.
            for (var pIdx = 0; pIdx < projected.length; pIdx++) {
                var p = projected[pIdx]
                ctx.fillStyle = root.hexToRgba(p.z > 0.25 ? colCore : colPrimary, p.alpha * 0.95)
                ctx.beginPath()
                ctx.arc(p.sx, p.sy, p.size, 0, Math.PI * 2)
                ctx.fill()
            }

            // 7. Radiant inner plasma core.
            var coreR = R * (0.20 + amp * 0.08 + 0.018 * Math.sin(t * 3.2))
            var coreGrad = ctx.createRadialGradient(cx, cy, 0, cx, cy, coreR * 1.45)
            coreGrad.addColorStop(0.0, root.hexToRgba(colCore, 0.95))
            coreGrad.addColorStop(0.42, root.hexToRgba(colPrimary, 0.72 + amp * 0.22))
            coreGrad.addColorStop(1.0, root.hexToRgba(colSecondary, 0.0))
            ctx.fillStyle = coreGrad
            ctx.beginPath()
            ctx.arc(cx, cy, coreR * 1.45, 0, Math.PI * 2)
            ctx.fill()

            // Crisp inner aperture ring.
            ctx.strokeStyle = root.hexToRgba(colCore, 0.78)
            ctx.lineWidth = 1.3
            ctx.beginPath()
            ctx.arc(cx, cy, coreR * 0.65, 0, Math.PI * 2)
            ctx.stroke()
        }
    }
}
