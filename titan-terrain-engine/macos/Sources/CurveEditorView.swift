// Draggable height-remap curve editor — the SwiftUI twin of the web lab's
// src/components/CurveEditor.tsx.
//
// The engine has always accepted an arbitrary number of monotone-cubic control
// points (titan_apply_curve); the UI offered five fixed sliders, so most of
// that capability was unreachable.
//
// The rendered curve comes from the engine's own sampler (titan_sample_curve)
// rather than a spline reimplemented here. This codebase has already paid once
// for duplicating a formula across C++, TypeScript and Swift, and a curve
// editor whose preview disagrees with the result is worse than no editor.

import SwiftUI

struct CurveEditorView: View {
    let points: [CurvePoint]
    let onChange: ([CurvePoint]) -> Void

    /// Pointer distance (in curve units, 0..1) that counts as grabbing a point.
    private let grabRadius = 0.06
    private let samples = 96

    @State private var dragging: Int? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            GeometryReader { geo in
                let w = geo.size.width
                let h = geo.size.height
                ZStack {
                    grid(w: w, h: h)
                    identityReference(w: w, h: h)
                    curvePath(w: w, h: h)
                    handles(w: w, h: h)
                }
                .contentShape(Rectangle())
                .gesture(dragGesture(w: w, h: h))
            }
            .frame(height: 150)
            .background(Color.black.opacity(0.35))
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.25)))
            .clipShape(RoundedRectangle(cornerRadius: 4))

            HStack {
                Text("\(points.count) point\(points.count == 1 ? "" : "s")"
                     + (points.count >= maxCurvePoints ? " (max)" : ""))
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Spacer()
                ForEach(CurveEditorView.presets, id: \.name) { preset in
                    Button(preset.name) { onChange(preset.points) }
                        .buttonStyle(.plain)
                        .font(.system(size: 9, weight: .medium))
                        .foregroundStyle(.secondary)
                        .help(preset.hint)
                }
            }
        }
    }

    // MARK: - Drawing

    private func grid(w: CGFloat, h: CGFloat) -> some View {
        Path { p in
            for f in [0.25, 0.5, 0.75] {
                p.move(to: CGPoint(x: w * f, y: 0)); p.addLine(to: CGPoint(x: w * f, y: h))
                p.move(to: CGPoint(x: 0, y: h * f)); p.addLine(to: CGPoint(x: w, y: h * f))
            }
        }
        .stroke(Color.secondary.opacity(0.18), lineWidth: 0.5)
    }

    /// Identity line, so departures from "no change" are readable at a glance.
    private func identityReference(w: CGFloat, h: CGFloat) -> some View {
        Path { p in
            p.move(to: CGPoint(x: 0, y: h))
            p.addLine(to: CGPoint(x: w, y: 0))
        }
        .stroke(Color.secondary.opacity(0.35),
                style: StrokeStyle(lineWidth: 0.8, dash: [3, 3]))
    }

    private func curvePath(w: CGFloat, h: CGFloat) -> some View {
        let ys = sampleCurve(points, samples: samples)
        return Path { p in
            guard ys.count == samples else { return }
            for i in 0..<samples {
                let x = CGFloat(i) / CGFloat(samples - 1) * w
                let y = (1 - CGFloat(ys[i])) * h
                if i == 0 { p.move(to: CGPoint(x: x, y: y)) } else { p.addLine(to: CGPoint(x: x, y: y)) }
            }
        }
        .stroke(Color.green, lineWidth: 1.6)
    }

    private func handles(w: CGFloat, h: CGFloat) -> some View {
        ForEach(Array(points.enumerated()), id: \.offset) { i, pt in
            let pinned = i == 0 || i == points.count - 1
            Circle()
                .fill(pinned ? Color.secondary : Color.green)
                .frame(width: dragging == i ? 11 : 8, height: dragging == i ? 11 : 8)
                .position(x: pt.x * w, y: (1 - pt.y) * h)
                .onTapGesture(count: 2) { remove(i) }
        }
    }

    // MARK: - Interaction

    private func dragGesture(w: CGFloat, h: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                let pos = CurvePoint(
                    x: min(1, max(0, Double(value.location.x / max(w, 1)))),
                    y: min(1, max(0, 1 - Double(value.location.y / max(h, 1))))
                )
                if dragging == nil {
                    // Hit-test where the press *started*, not where the cursor
                    // already is. The first onChanged can arrive well away from
                    // the press when events coalesce during a fast drag, and
                    // testing that position grabs nothing and drops a spurious
                    // point instead of moving the one under the finger.
                    let start = CurvePoint(
                        x: min(1, max(0, Double(value.startLocation.x / max(w, 1)))),
                        y: min(1, max(0, 1 - Double(value.startLocation.y / max(h, 1))))
                    )
                    if let hit = nearest(to: start) {
                        dragging = hit
                    } else if points.count < maxCurvePoints {
                        // Pressing empty space adds a point there and drags it.
                        guard let next = sanitizeCurve(points + [start]) else { return }
                        onChange(next)
                        dragging = next.firstIndex { abs($0.x - start.x) < 1e-9 }
                        return
                    } else {
                        return
                    }
                }
                guard let i = dragging, i < points.count else { return }
                var next = points
                // Endpoints stay pinned to x = 0 and x = 1: the remap has to
                // cover the whole height range, and the engine's spline clamps
                // outside it anyway.
                let isEnd = i == 0 || i == points.count - 1
                next[i] = CurvePoint(x: isEnd ? next[i].x : pos.x, y: pos.y)
                if let cleaned = sanitizeCurve(next) { onChange(cleaned) }
            }
            .onEnded { _ in dragging = nil }
    }

    private func nearest(to pos: CurvePoint) -> Int? {
        var best: Int? = nil
        var bestDist = grabRadius
        for (i, p) in points.enumerated() {
            let d = ((p.x - pos.x) * (p.x - pos.x) + (p.y - pos.y) * (p.y - pos.y)).squareRoot()
            if d < bestDist { bestDist = d; best = i }
        }
        return best
    }

    private func remove(_ i: Int) {
        // Two points are the minimum the engine will evaluate, and the
        // endpoints anchor the range.
        guard points.count > 2, i != 0, i != points.count - 1 else { return }
        var next = points
        next.remove(at: i)
        onChange(next)
    }

    // MARK: - Presets

    struct Preset { let name: String; let hint: String; let points: [CurvePoint] }

    static let presets: [Preset] = [
        Preset(name: "Reset", hint: "Identity — no change", points: [
            CurvePoint(x: 0, y: 0), CurvePoint(x: 0.5, y: 0.5), CurvePoint(x: 1, y: 1),
        ]),
        Preset(name: "S", hint: "Contrast: flattens lowlands, steepens midslopes, rounds peaks", points: [
            CurvePoint(x: 0, y: 0), CurvePoint(x: 0.3, y: 0.16), CurvePoint(x: 0.5, y: 0.5),
            CurvePoint(x: 0.7, y: 0.84), CurvePoint(x: 1, y: 1),
        ]),
        Preset(name: "Mesa", hint: "Compresses the top into a plateau", points: [
            CurvePoint(x: 0, y: 0), CurvePoint(x: 0.45, y: 0.5), CurvePoint(x: 0.7, y: 0.86),
            CurvePoint(x: 0.85, y: 0.95), CurvePoint(x: 1, y: 1),
        ]),
        Preset(name: "Basin", hint: "Deepens valleys, leaves peaks alone", points: [
            CurvePoint(x: 0, y: 0), CurvePoint(x: 0.35, y: 0.12), CurvePoint(x: 0.7, y: 0.55),
            CurvePoint(x: 1, y: 1),
        ]),
    ]
}
