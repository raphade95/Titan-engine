// TitanLab main window: parameter sidebar + Metal viewport.

import MetalKit
import SwiftUI
import UniformTypeIdentifiers

struct MetalViewRepresentable: NSViewRepresentable {
    @ObservedObject var model: EngineModel

    final class Coordinator {
        var renderer: TerrainRenderer?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> TerrainMTKView {
        let view = TerrainMTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        if let renderer = TerrainRenderer(view: view) {
            context.coordinator.renderer = renderer
            view.delegate = renderer
            model.onMesh = { mesh in
                renderer.setMesh(mesh)
            }
        }
        return view
    }

    func updateNSView(_ nsView: TerrainMTKView, context: Context) {}
}

struct ContentView: View {
    @StateObject private var model = EngineModel()

    var body: some View {
        HSplitView {
            sidebar
                .frame(minWidth: 300, idealWidth: 320, maxWidth: 380)
            MetalViewRepresentable(model: model)
                .frame(minWidth: 500, maxWidth: .infinity, maxHeight: .infinity)
        }
        .onAppear { model.rebuild() }
    }

    private var sidebar: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                header
                presetSection
                Divider()
                baseSection
                Divider()
                erosionSection
                Divider()
                exportSection
                statusFooter
            }
            .padding(16)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("TITAN LAB").font(.system(size: 18, weight: .black, design: .default))
            Text("Procedural Terrain Engine")
                .font(.system(size: 9, weight: .medium)).foregroundStyle(.secondary)
        }
    }

    private var presetSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            sectionTitle("Presets")
            ForEach(EngineModel.presets()) { preset in
                Button {
                    preset.apply(model)
                    model.regenerateWithFreshSeed()
                } label: {
                    VStack(alignment: .leading, spacing: 1) {
                        Text(preset.name).font(.system(size: 11, weight: .bold))
                        Text(preset.tagline).font(.system(size: 9)).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 6).padding(.horizontal, 8)
                }
                .buttonStyle(.bordered)
            }
        }
    }

    private var baseSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("Base Terrain")

            Picker("Noise", selection: $model.noiseType) {
                Text("Flat").tag(0)
                Text("Simplex").tag(1)
                Text("Ridged").tag(2)
                Text("Billow").tag(3)
            }
            .pickerStyle(.segmented)

            HStack {
                TextField("Seed", text: $model.seed)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(size: 11, design: .monospaced))
                Toggle(isOn: $model.seedLocked) {
                    Image(systemName: model.seedLocked ? "lock.fill" : "lock.open")
                }
                .toggleStyle(.button)
                Button {
                    model.seed = randomSeed()
                    model.rebuild()
                } label: {
                    Image(systemName: "dice")
                }
            }

            Picker("Resolution", selection: $model.size) {
                Text("128").tag(128.0)
                Text("256").tag(256.0)
                Text("512").tag(512.0)
            }
            .pickerStyle(.segmented)

            paramSlider("Scale", $model.scale, 0.1...10, "%.1f")
            paramSlider("Height", $model.heightMultiplier, 10...200, "%.0f")
            paramSlider("Octaves", $model.octaves, 1...12, "%.0f")
            paramSlider("Exponent", $model.exponent, 0.5...3, "%.2f")
            paramSlider("Domain Warp", $model.warpStrength, 0...2, "%.2f")

            Button {
                model.regenerateWithFreshSeed()
            } label: {
                Label("Regenerate", systemImage: "arrow.clockwise")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.isGenerating)
        }
    }

    private var erosionSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            sectionTitle("Simulation Stack")

            Toggle("River Networks", isOn: $model.riversEnabled)
            if model.riversEnabled {
                paramSlider("Passes", $model.riverPasses, 1...10, "%.0f")
                paramSlider("Strength", $model.riverStrength, 0.1...3, "%.1f")
            }

            Toggle("Hydraulic Erosion", isOn: $model.dropletsEnabled)
            if model.dropletsEnabled {
                paramSlider("Droplets ×16k", $model.dropletRounds, 1...12, "%.0f")
                Picker("Rainfall", selection: $model.dropletSpawnMode) {
                    Text("Uniform").tag(0)
                    Text("Highlands").tag(1)
                }
                .pickerStyle(.segmented)
            }

            Toggle("Thermal Weathering", isOn: $model.thermalEnabled)
            if model.thermalEnabled {
                paramSlider("Passes", $model.thermalPasses, 1...50, "%.0f")
                paramSlider("Repose Angle", $model.thermalAngle, 20...45, "%.0f")
            }

            Button {
                model.rebuild()
            } label: {
                Label("Run Stack", systemImage: "play.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .disabled(model.isGenerating)
        }
    }

    private var exportSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            sectionTitle("Export")
            HStack {
                exportButton(".r16", kind: "r16", ext: "r16")
                exportButton(".png 16", kind: "png16", ext: "png")
                exportButton(".exr", kind: "exr", ext: "exr")
            }
            HStack {
                exportButton(".r32", kind: "r32", ext: "r32")
                exportButton(".obj", kind: "obj", ext: "obj")
            }
        }
    }

    private var statusFooter: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(model.isGenerating ? Color.yellow : Color.green)
                .frame(width: 7, height: 7)
            Text(model.statusText)
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
        .padding(.top, 6)
    }

    // MARK: - Small helpers

    private func sectionTitle(_ text: String) -> some View {
        Text(text.uppercased())
            .font(.system(size: 9, weight: .bold))
            .foregroundStyle(.secondary)
            .kerning(1.2)
    }

    private func paramSlider(_ label: String, _ value: Binding<Double>,
                             _ range: ClosedRange<Double>, _ fmt: String) -> some View {
        VStack(spacing: 2) {
            HStack {
                Text(label).font(.system(size: 10))
                Spacer()
                Text(String(format: fmt, value.wrappedValue))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
            Slider(value: value, in: range) { editing in
                // Deferred update: rebuild only when the drag ends.
                if !editing { model.rebuild() }
            }
        }
    }

    private func exportButton(_ label: String, kind: String, ext: String) -> some View {
        Button(label) {
            guard let data = model.exportData(kind: kind) else { return }
            let panel = NSSavePanel()
            panel.nameFieldStringValue = "titan_\(model.seed)_\(Int(model.size))x\(Int(model.size)).\(ext)"
            if panel.runModal() == .OK, let url = panel.url {
                try? data.write(to: url)
            }
        }
        .font(.system(size: 10))
    }
}
