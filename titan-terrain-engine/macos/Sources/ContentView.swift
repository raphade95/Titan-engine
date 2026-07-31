// TitanLab main window: tabbed parameter sidebar (Base / Stack / Render /
// Export — mirroring the web lab) + Metal viewport with 2D top-down overlay,
// terrain inspector, and manual carver.

import MetalKit
import SwiftUI
import UniformTypeIdentifiers

struct MetalViewRepresentable: NSViewRepresentable {
    @ObservedObject var model: EngineModel

    final class Coordinator {
        var renderer: TerrainRenderer?
        // The cone currently being dragged, so pointer motion repositions it
        // instead of dropping a new one every frame.
        var draggingVolcano: UUID?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> TerrainMTKView {
        let view = TerrainMTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        if let renderer = TerrainRenderer(view: view) {
            context.coordinator.renderer = renderer
            view.delegate = renderer
            model.onMesh = { mesh in renderer.setMesh(mesh) }
            model.onFlora = { instances, biome in renderer.setFlora(instances, biome: biome) }
        }
        view.heightAt = { [weak model] gx, gy in model?.probeHeight(gridX: gx, gridY: gy) ?? 0 }
        view.onProbe = { [weak model] gx, gy in
            guard let model else { return }
            if model.inspectorOn { model.updateProbe(gridX: gx, gridY: gy) }
        }
        view.onProbeMiss = { [weak model] in model?.clearProbe() }
        view.onCarve = { [weak model] gx, gy in model?.carve(gridX: gx, gridY: gy) }
        view.onPlaceVolcano = { [weak model, weak coordinator = context.coordinator] gx, gy in
            guard let model, let coordinator else { return }
            let n = max(1.0, model.size)
            coordinator.draggingVolcano = model.placeVolcano(atX: Double(gx) / n,
                                                             y: Double(gy) / n)
        }
        view.onDragVolcano = { [weak model, weak coordinator = context.coordinator] gx, gy in
            guard let model, let coordinator, let id = coordinator.draggingVolcano else { return }
            let n = max(1.0, model.size)
            model.moveVolcano(id, toX: Double(gx) / n, y: Double(gy) / n)
        }
        return view
    }

    func updateNSView(_ nsView: TerrainMTKView, context: Context) {
        context.coordinator.renderer?.settings = RenderSettings(
            sunElevation: Float(model.sunElevation),
            sunAzimuth: Float(model.sunAzimuth),
            sunIntensity: Float(model.sunIntensity),
            fogDensity: Float(model.fogDensity),
            seaLevel: Float(model.seaLevel),
            biome: model.biome,
            wireframe: model.wireframe,
            showGrid: model.showGrid
        )
        nsView.inspectEnabled = model.inspectorOn
        nsView.carveEnabled = model.carverOn
        nsView.volcanoPlaceEnabled = model.volcanoPlacementOn
    }
}

struct ContentView: View {
    @StateObject private var model = EngineModel()
    @State private var tab = 0
    // Which layers have their advanced physics panel open, keyed by layer id.
    @State private var expandedAdvanced: Set<UUID> = []

    var body: some View {
        HSplitView {
            sidebar
                .frame(minWidth: 310, idealWidth: 340, maxWidth: 400)
            viewport
                .frame(minWidth: 500, maxWidth: .infinity, maxHeight: .infinity)
        }
        .onAppear { model.rebuild() }
    }

    // MARK: - Viewport (3D + overlays)

    private var viewport: some View {
        ZStack(alignment: .topTrailing) {
            MetalViewRepresentable(model: model)

            if model.topDownFullscreen {
                topDownFullscreenView
            } else {
                if model.showTopDown, let image = model.topDownImage {
                    topDownMiniMap(image)
                }
            }

            if let probe = model.probe, model.inspectorOn, !model.topDownFullscreen {
                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        probeCard(probe)
                    }
                }
                .padding(12)
                .allowsHitTesting(false)
            }
        }
    }

    private func topDownMiniMap(_ image: CGImage) -> some View {
        VStack(alignment: .trailing, spacing: 4) {
            HStack(spacing: 6) {
                Text("N ↑ TOP-DOWN")
                    .font(.system(size: 8, weight: .bold))
                    .kerning(1.2)
                    .foregroundStyle(.secondary)
                Button {
                    model.topDownFullscreen = true
                    model.refreshTopDown()
                } label: {
                    Image(systemName: "arrow.up.left.and.arrow.down.right")
                        .font(.system(size: 9))
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .help("Swap the 2D map into the full viewport")
            }
            Image(decorative: image, scale: 1)
                .resizable()
                .interpolation(.none)
                .frame(width: 220, height: 220)
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4)
                    .stroke(Color.black.opacity(0.4), lineWidth: 1))
                .onTapGesture {
                    model.topDownFullscreen = true
                    model.refreshTopDown()
                }
        }
        .padding(8)
        .background(.black.opacity(0.45), in: RoundedRectangle(cornerRadius: 8))
        .padding(10)
    }

    private var topDownFullscreenView: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Label("2D Top-Down — N is up (the far edge in the default 3D view)",
                      systemImage: "map")
                    .font(.system(size: 11, weight: .bold))
                Spacer()
                Text("Zoom")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                Slider(value: $model.topDownZoom, in: 1...8)
                    .frame(width: 160)
                Button {
                    model.topDownFullscreen = false
                } label: {
                    Label("Back to 3D", systemImage: "cube")
                }
            }
            .padding(10)
            .background(.black.opacity(0.5))

            GeometryReader { geo in
                let side = min(geo.size.width, geo.size.height) * model.topDownZoom
                ScrollView([.horizontal, .vertical]) {
                    if let image = model.topDownImage {
                        Image(decorative: image, scale: 1)
                            .resizable()
                            .interpolation(.none)
                            .frame(width: side, height: side)
                            .frame(minWidth: geo.size.width, minHeight: geo.size.height)
                    } else {
                        Text("Generating map…")
                            .frame(width: geo.size.width, height: geo.size.height)
                    }
                }
            }
            .background(Color.black.opacity(0.85))
        }
    }

    private func probeCard(_ probe: ProbeData) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("TERRAIN PROBE")
                .font(.system(size: 8, weight: .bold)).kerning(1.2)
                .foregroundStyle(.green)
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 2) {
                GridRow {
                    Text("Cell").foregroundStyle(.secondary)
                    Text("\(probe.x), \(probe.y)")
                }
                GridRow {
                    Text("Height").foregroundStyle(.secondary)
                    Text(String(format: "%.2f m", probe.height))
                }
                GridRow {
                    Text("Sediment").foregroundStyle(.secondary)
                    Text(String(format: "%.2f cm", probe.sediment * 10))
                }
                GridRow {
                    Text("Flow").foregroundStyle(.secondary)
                    Text(String(format: "%.1f %%", probe.flow * 100))
                }
                GridRow {
                    Text("Slope").foregroundStyle(.secondary)
                    Text(String(format: "%.1f°", atan(probe.slope) * 180 / .pi))
                }
            }
            .font(.system(size: 10, design: .monospaced))
        }
        .padding(10)
        .background(.black.opacity(0.65), in: RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(.green.opacity(0.35), lineWidth: 1))
    }

    // MARK: - Sidebar

    private var sidebar: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 10) {
                header
                Picker("", selection: $tab) {
                    Text("Base").tag(0)
                    Text("Stack").tag(1)
                    Text("Render").tag(2)
                    Text("Export").tag(3)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
            }
            .padding([.horizontal, .top], 14)
            .padding(.bottom, 8)

            Divider()

            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    switch tab {
                    case 1: stackTab
                    case 2: renderTab
                    case 3: exportTab
                    default: baseTab
                    }
                }
                .padding(14)
            }

            Divider()
            statusFooter
                .padding(10)
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("TITAN LAB").font(.system(size: 18, weight: .black))
            Text("Procedural Terrain Engine")
                .font(.system(size: 9, weight: .medium)).foregroundStyle(.secondary)
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
    }

    // MARK: - Base tab

    private var baseTab: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Presets")
                ForEach(EngineModel.presets()) { preset in
                    Button {
                        model.applyPreset(preset)
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

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Biological Biome")
                Picker("", selection: $model.biome) {
                    Text("Forest").tag(1)
                    Text("Arctic").tag(0)
                    Text("Vulcan").tag(2)
                    Text("Desert").tag(3)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
            }

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Noise Structure")
                Picker("Noise", selection: $model.noiseType) {
                    Text("Flat").tag(0)
                    Text("Simplex").tag(1)
                    Text("Ridged").tag(2)
                    Text("Billow").tag(3)
                    Text("Cells (Voronoi)").tag(4)
                    Text("Walls (Voronoi Ridge)").tag(5)
                    Text("Worley Manhattan").tag(6)
                    Text("Worley Chebyshev").tag(7)
                    Text("Hybrid Multifractal").tag(8)
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .onChange(of: model.noiseType) { _, _ in model.rebuild() }
            }

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Heightmap Import")
                if let name = model.importedName {
                    HStack(spacing: 6) {
                        Image(systemName: "photo").foregroundStyle(.green)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(name).font(.system(size: 10, weight: .bold)).lineLimit(1)
                            Text("Base terrain · scaled by Height")
                                .font(.system(size: 8)).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button { model.clearImport() } label: {
                            Image(systemName: "xmark.circle.fill")
                        }
                        .buttonStyle(.plain)
                        .foregroundStyle(.secondary)
                    }
                    .padding(6)
                    .background(.green.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
                } else {
                    Button {
                        let panel = NSOpenPanel()
                        var types: [UTType] = [.png, .jpeg, .tiff]
                        for ext in ["r16", "r32", "raw"] {
                            if let t = UTType(filenameExtension: ext) { types.append(t) }
                        }
                        panel.allowedContentTypes = types
                        panel.message = "Choose a heightmap (.png, .r16, .r32)"
                        if panel.runModal() == .OK, let url = panel.url {
                            model.importHeightmap(url: url)
                        }
                    } label: {
                        Label("Import .png / .r16 / .r32", systemImage: "square.and.arrow.down")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 10) {
                sectionTitle("Base Terrain")
                HStack {
                    TextField("Seed", text: $model.seed)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(size: 11, design: .monospaced))
                        .onSubmit { model.rebuild() }
                    Toggle(isOn: $model.seedLocked) {
                        Image(systemName: model.seedLocked ? "lock.fill" : "lock.open")
                    }
                    .toggleStyle(.button)
                    .help(model.seedLocked
                          ? "Seed locked — regenerate reproduces this exact terrain"
                          : "Seed unlocked — regenerate picks a fresh seed")
                    Button {
                        model.seed = randomSeed()
                        model.rebuild()
                    } label: {
                        Image(systemName: "dice")
                    }
                    .help("Randomize the seed")
                }

                paramSlider("World Size", $model.worldSize, 64...8192, "%.0f u", step: 64)
                Text("How far the terrain spans, in the same units as Height. This is the terrain's actual size — Resolution only controls how finely it is sampled.")
                    .font(.system(size: 8.5)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                paramSlider("Resolution", $model.size, 64...2048, "%.0f", step: 64,
                            detail: "\(Int(model.size))×\(Int(model.size))")
                Text(String(format: "Samples per edge — detail only. Cell size %.3f u.",
                            model.worldSize / max(1, model.size)))
                    .font(.system(size: 8.5)).foregroundStyle(.secondary)
                paramSlider("Scale", $model.scale, 0.1...10, "%.1f")
                paramSlider("Height", $model.heightMultiplier, 10...500, "%.0f")
                paramSlider("Octaves", $model.octaves, 1...12, "%.0f", step: 1)
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
    }

    // MARK: - Stack tab

    private var stackTab: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("The stack runs top to bottom on every rebuild. Same seed + same stack = identical terrain, every time.")
                .font(.system(size: 9)).foregroundStyle(.secondary)

            if model.stack.isEmpty {
                Text("No layers yet — add one below or pick a preset.")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 18)
                    .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 8))
            }

            ForEach(Array(model.stack.enumerated()), id: \.element.id) { index, layer in
                layerCard(layer, index: index)
            }

            Menu {
                ForEach(layerOrder, id: \.self) { kind in
                    Button(layerDefs[kind]!.label) { model.addLayer(kind) }
                }
            } label: {
                Label("Add Layer", systemImage: "plus")
                    .frame(maxWidth: .infinity)
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Volcanoes")
                Text("Turn this on and press anywhere on the terrain to drop a volcano — drag before releasing to position it. Each one adds a Volcano layer you can tune above; a single Lava Flow layer erupts them all.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Toggle(isOn: $model.volcanoPlacementOn) {
                    Label(model.volcanoPlacementOn
                          ? "Placing — click the terrain"
                          : "Place a Volcano", systemImage: "flame")
                }
                .toggleStyle(.button)
                .tint(.orange)
                .onChange(of: model.volcanoPlacementOn) { _, on in
                    if on {
                        model.inspectorOn = false
                        model.carverOn = false
                        model.clearProbe()
                    }
                }
                HStack {
                    Button {
                        model.placeVolcano(atX: 0.5, y: 0.5)
                    } label: {
                        Label("At Center", systemImage: "plus")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(model.isGenerating)
                    Button {
                        model.clearVolcanoes()
                    } label: {
                        Label("Clear All", systemImage: "trash")
                            .frame(maxWidth: .infinity)
                    }
                    .disabled(model.volcanoCount == 0)
                }
                if model.volcanoCount > 0 {
                    Text("\(model.volcanoCount) volcano\(model.volcanoCount == 1 ? "" : "es") placed")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(.orange)
                }
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Flora Scattering")
                Text("Procedurally scatter vegetation based on moisture (flow), sediment depth, and slope analysis.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                paramSlider("Density", $model.floraDensity, 100...10000, "%.0f",
                            step: 100, rebuildOnEnd: false)
                HStack {
                    Button {
                        model.scatterFlora()
                    } label: {
                        Label("Scatter Foliage", systemImage: "leaf")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                    .disabled(model.isGenerating)
                    if model.floraCount > 0 {
                        Button("Clear") { model.clearFlora() }
                    }
                }
                if model.floraCount > 0 {
                    Text("\(model.floraCount) instances placed")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    private func layerCard(_ layer: TitanLayer, index: Int) -> some View {
        let def = layerDefs[layer.kind]!
        return VStack(alignment: .leading, spacing: 8) {
            layerHeader(layer, def: def, index: index)
            Text(def.blurb).font(.system(size: 8.5)).foregroundStyle(.secondary)
            ForEach(def.params.filter { !$0.advanced }, id: \.key) { pd in
                layerParamRow(layer, pd)
            }
            advancedBlock(layer, def: def)
            layerMaskBlock(layer)
        }
        .padding(9)
        .background(.quaternary.opacity(layer.enabled ? 0.45 : 0.2),
                    in: RoundedRectangle(cornerRadius: 8))
        .opacity(layer.enabled ? 1 : 0.6)
    }

    // Advanced physics, collapsed by default. These map onto the engine's _ex
    // erosion entry points, which shipped in v0.4 and were reachable from no UI
    // until now — the tuning surface power users compare against Gaea.
    @ViewBuilder
    private func advancedBlock(_ layer: TitanLayer, def: LayerDef) -> some View {
        let advanced = def.params.filter { $0.advanced }
        if !advanced.isEmpty {
            DisclosureGroup(isExpanded: Binding(
                get: { expandedAdvanced.contains(layer.id) },
                set: { open in
                    if open { expandedAdvanced.insert(layer.id) }
                    else { expandedAdvanced.remove(layer.id) }
                }
            )) {
                VStack(alignment: .leading, spacing: 8) {
                    ForEach(advanced, id: \.key) { pd in
                        layerParamRow(layer, pd)
                    }
                }
                .padding(.top, 4)
            } label: {
                Text("ADVANCED (\(advanced.count))")
                    .font(.system(size: 8.5, weight: .semibold)).kerning(0.8)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func layerHeader(_ layer: TitanLayer, def: LayerDef, index: Int) -> some View {
        HStack(spacing: 6) {
            Toggle("", isOn: Binding(
                get: { layer.enabled },
                set: { model.toggleLayer(layer.id, enabled: $0) }
            ))
            .toggleStyle(.switch)
            .controlSize(.mini)
            .labelsHidden()
            Text(def.label.uppercased())
                .font(.system(size: 10, weight: .bold)).kerning(0.8)
            Spacer()
            Button { model.moveLayer(layer.id, by: -1) } label: {
                Image(systemName: "arrow.up")
            }
            .buttonStyle(.plain).font(.system(size: 9))
            .disabled(index == 0)
            Button { model.moveLayer(layer.id, by: 1) } label: {
                Image(systemName: "arrow.down")
            }
            .buttonStyle(.plain).font(.system(size: 9))
            .disabled(index == model.stack.count - 1)
            Button { model.removeLayer(layer.id) } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.plain).font(.system(size: 9))
            .foregroundStyle(.red.opacity(0.7))
        }
    }

    @ViewBuilder
    private func layerParamRow(_ layer: TitanLayer, _ pd: ParamDef) -> some View {
        if let choices = pd.choices {
            VStack(alignment: .leading, spacing: 3) {
                Text(pd.label).font(.system(size: 9)).foregroundStyle(.secondary)
                layerChoicePicker(layer, pd, choices: choices)
            }
        } else {
            paramSlider(pd.label, layerParamBinding(layer, pd), pd.range,
                        pd.step < 1 ? "%.2f" : "%.0f", step: pd.step)
        }
    }

    private func layerChoicePicker(_ layer: TitanLayer, _ pd: ParamDef,
                                   choices: [String]) -> some View {
        let selection = Binding<Int>(
            get: { Int(layer.params[pd.key] ?? pd.def) },
            set: { newValue in
                model.setLayerParam(layer.id, pd.key, Double(newValue))
                model.rebuild()
            }
        )
        return Group {
            if choices.count > 4 {
                Picker("", selection: selection) {
                    ForEach(Array(choices.enumerated()), id: \.offset) { ci, label in
                        Text(label).tag(ci)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            } else {
                Picker("", selection: selection) {
                    ForEach(Array(choices.enumerated()), id: \.offset) { ci, label in
                        Text(label).tag(ci)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
            }
        }
    }

    private func layerParamBinding(_ layer: TitanLayer, _ pd: ParamDef) -> Binding<Double> {
        Binding(
            get: { model.stack.first(where: { $0.id == layer.id })?.params[pd.key] ?? pd.def },
            set: { model.setLayerParam(layer.id, pd.key, $0) }
        )
    }

    private func layerMaskBlock(_ layer: TitanLayer) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text("MASK").font(.system(size: 8, weight: .bold)).kerning(0.8)
                    .foregroundStyle(.secondary)
                Spacer()
                if layer.mask.mode > 0 {
                    Toggle("Invert", isOn: Binding(
                        get: { layer.mask.invert },
                        set: { v in
                            model.setLayerMask(layer.id) { $0.invert = v }
                            model.rebuild()
                        }
                    ))
                    .toggleStyle(.checkbox)
                    .controlSize(.mini)
                    .font(.system(size: 9))
                }
            }
            Picker("", selection: Binding(
                get: { layer.mask.mode },
                set: { v in
                    model.setLayerMask(layer.id) { $0.mode = v }
                    model.rebuild()
                }
            )) {
                ForEach(Array(maskModeLabels.enumerated()), id: \.offset) { mi, label in
                    Text(label).tag(mi)
                }
            }
            .pickerStyle(.segmented)
            .controlSize(.small)
            .labelsHidden()
            if layer.mask.mode > 0 {
                paramSlider("Band Min", Binding(
                    get: { model.stack.first(where: { $0.id == layer.id })?.mask.lo ?? 0 },
                    set: { v in model.setLayerMask(layer.id) { $0.lo = v } }
                ), 0...1, "%.2f")
                paramSlider("Band Max", Binding(
                    get: { model.stack.first(where: { $0.id == layer.id })?.mask.hi ?? 1 },
                    set: { v in model.setLayerMask(layer.id) { $0.hi = v } }
                ), 0...1, "%.2f")
            }
        }
        .padding(.top, 2)
    }

    // MARK: - Render tab

    private var renderTab: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 10) {
                sectionTitle("Hydrology")
                paramSlider("Sea Level", $model.seaLevel, -40...100, "%.1f m",
                            rebuildOnEnd: false, onEnd: { model.refreshTopDown() })
            }

            Divider()

            VStack(alignment: .leading, spacing: 10) {
                sectionTitle("Atmosphere")
                paramSlider("Sun Elevation", $model.sunElevation, -10...90, "%.0f°",
                            rebuildOnEnd: false)
                paramSlider("Sun Azimuth", $model.sunAzimuth, 0...360, "%.0f°",
                            rebuildOnEnd: false)
                paramSlider("Sun Intensity", $model.sunIntensity, 0...5, "%.1fx",
                            rebuildOnEnd: false)
                paramSlider("Fog Density", $model.fogDensity, 0...0.01, "%.4f",
                            rebuildOnEnd: false)
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("View")
                Toggle("2D Top-Down Map", isOn: $model.showTopDown)
                    .onChange(of: model.showTopDown) { _, _ in model.refreshTopDown() }
                Toggle("Wireframe", isOn: $model.wireframe)
                Toggle("Grid Helper", isOn: $model.showGrid)
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Terrain Inspector")
                Text("Hover the terrain to probe height, sediment, flow, and slope.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Toggle("Enable Inspector", isOn: $model.inspectorOn)
                    .onChange(of: model.inspectorOn) { _, on in
                        if on {
                            model.carverOn = false
                            model.volcanoPlacementOn = false
                        } else {
                            model.clearProbe()
                        }
                    }
            }

            Divider()

            VStack(alignment: .leading, spacing: 8) {
                sectionTitle("Manual Carver")
                Text("Click and drag on the terrain to carve bedrock.")
                    .font(.system(size: 9)).foregroundStyle(.secondary)
                Toggle("Enable Carver", isOn: $model.carverOn)
                    .onChange(of: model.carverOn) { _, on in
                        if on {
                            model.inspectorOn = false
                            model.volcanoPlacementOn = false
                            model.clearProbe()
                        }
                    }
                if model.carverOn {
                    paramSlider("Brush Size", $model.carveRadius, 1...20, "%.0f",
                                step: 1, rebuildOnEnd: false)
                    paramSlider("Depth", $model.carveDepth, 0.1...10, "%.1f",
                                rebuildOnEnd: false)
                }
            }
        }
    }

    // MARK: - Export tab

    private struct ExportSpec: Identifiable {
        let id: String
        let label: String
        let ext: String
        let sub: String
    }

    private static let exportSpecs: [ExportSpec] = [
        ExportSpec(id: "r16", label: ".r16", ext: "r16", sub: "16-bit RAW · Unreal"),
        ExportSpec(id: "png16", label: ".png 16", ext: "png", sub: "16-bit PNG · Unity"),
        ExportSpec(id: "exr", label: ".exr", ext: "exr", sub: "Float32 · Blender"),
        ExportSpec(id: "r32", label: ".r32", ext: "r32", sub: "Float32 RAW"),
        ExportSpec(id: "obj", label: ".obj", ext: "obj", sub: "Mesh · Blender"),
        ExportSpec(id: "normal", label: "Normal", ext: "png", sub: "Normal map · RGB8"),
        ExportSpec(id: "ao", label: "AO", ext: "png", sub: "Ambient occlusion"),
        ExportSpec(id: "splat", label: "Splat", ext: "png", sub: "RGBA masks · matches viewport"),
    ]

    private var heightRangeSection: some View {
        // The exporters trim an outlier tail when droplet erosion has left
        // sediment towers, so the file's span can be narrower than the
        // terrain's. Report the one the file actually encodes — a Z scale
        // derived from the other is wrong.
        let lo = model.exportRange.min
        let hi = model.exportRange.max
        let span = hi - lo
        let trimmed = model.exportRange.max < model.heightRange.max - 1e-4
        return VStack(alignment: .leading, spacing: 6) {
            Text("EXPORT HEIGHT RANGE")
                .font(.system(size: 9, weight: .semibold))
                .tracking(1.4)
                .foregroundStyle(.secondary)
            Text(String(format: "%.2f \u{2192} %.2f    span %.2f", lo, hi, span))
                .font(.system(size: 11, design: .monospaced))
            Text(String(format: ".r16 and .png16 normalize to exactly this span, not to the Height slider (%.0f) \u{2014} erosion and deposition move both ends. Unreal Z scale = %.3f at 1 unit = 1 m.", model.heightMultiplier, span * 100.0 / 512.0))
                .font(.system(size: 9))
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if trimmed {
                Text(String(format: "Terrain reaches %.2f, but a few single-cell sediment towers sit far above everything else. Normalizing to them would waste most of the 16-bit depth, so the export clips them to %.2f. A Thermal Weathering layer settles them out properly.", model.heightRange.max, hi))
                    .font(.system(size: 9))
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.secondary.opacity(0.08)))
    }

    private var exportTab: some View {
        VStack(alignment: .leading, spacing: 16) {
            heightRangeSection
            exportFormatsSection
            Divider()
            projectSection
            Divider()
            importGuideSection
        }
    }

    private var exportFormatsSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            sectionTitle("Heightmap & Mesh Export")
            Text("High-bitrate heightmaps and maps for Unreal, Unity, and Blender.")
                .font(.system(size: 9)).foregroundStyle(.secondary)
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 6) {
                ForEach(Self.exportSpecs) { spec in
                    exportButton(spec.label, kind: spec.id, ext: spec.ext, sub: spec.sub)
                }
            }
        }
    }

    private var projectSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            sectionTitle("Project")
            Text("A .titan file stores the seed, base parameters, full layer stack, and any imported heightmap — a perfect reproduction recipe, portable to the web lab.")
                .font(.system(size: 9)).foregroundStyle(.secondary)
            HStack {
                Button {
                    saveProject()
                } label: {
                    Label("Save .titan", systemImage: "square.and.arrow.down")
                        .frame(maxWidth: .infinity)
                }
                Button {
                    loadProject()
                } label: {
                    Label("Load .titan", systemImage: "folder")
                        .frame(maxWidth: .infinity)
                }
            }
        }
    }

    private var importGuideSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            sectionTitle("Import Guide")
            Text("Unreal: Landscape mode → Import from File → .r16.\nUnity: Terrain → Import Raw, or the 16-bit PNG.\nBlender: .exr as displacement, or the .obj mesh.")
                .font(.system(size: 8.5)).foregroundStyle(.secondary)
        }
    }

    private func saveProject() {
        guard let data = model.serializeProject() else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "terrain_\(model.seed).titan"
        if let titanType = UTType(filenameExtension: "titan") {
            panel.allowedContentTypes = [titanType, .json]
        }
        if panel.runModal() == .OK, let url = panel.url {
            try? data.write(to: url)
        }
    }

    private func loadProject() {
        let panel = NSOpenPanel()
        var types: [UTType] = [.json]
        if let titanType = UTType(filenameExtension: "titan") { types.append(titanType) }
        panel.allowedContentTypes = types
        panel.message = "Choose a .titan project file"
        if panel.runModal() == .OK, let url = panel.url,
           let data = try? Data(contentsOf: url) {
            if !model.loadProject(from: data) {
                model.statusText = "Could not read project file"
            }
        }
    }

    // MARK: - Small helpers

    private func sectionTitle(_ text: String) -> some View {
        Text(text.uppercased())
            .font(.system(size: 9, weight: .bold))
            .foregroundStyle(.secondary)
            .kerning(1.2)
    }

    private func paramSlider(_ label: String, _ value: Binding<Double>,
                             _ range: ClosedRange<Double>, _ fmt: String,
                             step: Double? = nil, rebuildOnEnd: Bool = true,
                             detail: String? = nil,
                             onEnd: (() -> Void)? = nil) -> some View {
        VStack(spacing: 2) {
            HStack {
                Text(label).font(.system(size: 10))
                Spacer()
                Text(detail ?? String(format: fmt, value.wrappedValue))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
            Group {
                if let step {
                    Slider(value: value, in: range, step: step) { editing in
                        if !editing {
                            if rebuildOnEnd { model.rebuild() }
                            onEnd?()
                        }
                    }
                } else {
                    Slider(value: value, in: range) { editing in
                        if !editing {
                            if rebuildOnEnd { model.rebuild() }
                            onEnd?()
                        }
                    }
                }
            }
        }
    }

    private func exportButton(_ label: String, kind: String, ext: String, sub: String) -> some View {
        Button {
            guard let data = model.exportData(kind: kind) else { return }
            let panel = NSSavePanel()
            let suffix = (kind == "normal" || kind == "ao") ? "_\(kind)" : ""
            panel.nameFieldStringValue =
                "titan_\(model.seed)_\(Int(model.size))x\(Int(model.size))\(suffix).\(ext)"
            if panel.runModal() == .OK, let url = panel.url {
                try? data.write(to: url)
            }
        } label: {
            VStack(spacing: 1) {
                Text(label).font(.system(size: 10, weight: .bold))
                Text(sub).font(.system(size: 7.5)).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 4)
        }
        .buttonStyle(.bordered)
    }
}
