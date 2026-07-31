// Node graph: the document model and its evaluator.
//
// The layer stack is a straight line — every operation applies to the result
// of the one above it. A graph lifts that restriction: terrain can fork, be
// worked on two different ways, and rejoin. Everything else is deliberately
// the same. A graph node holds a `TitanLayer`, and evaluating one calls
// `EngineModel.execute` — the very function the stack uses. There is no second
// implementation of blur, or erosion, or anything else, and a node cannot
// drift away from the layer that shares its name.
//
// What made this possible engine-side is titan_read_height / titan_set_height
// (C API v0.11): a branch's result can be parked in a buffer and the engine
// rewound to an earlier surface. Without them a host would have to reimplement
// the operations to evaluate anything that is not a straight line.
//
// Conventions follow the node editors people already know — see
// NodeGraphView.swift for the interaction side of that.

import Foundation
import SwiftUI

// ---------------------------------------------------------------------------
// Node kinds
// ---------------------------------------------------------------------------

enum NodeKind: String, CaseIterable, Identifiable {
    // Sources
    case terrain, flat
    // Operations that are layers by another name
    case noise, gradient, volcano, lava, snow
    case hydraulic, thermal, fluvial, terrace, plateau
    case clamp, curve, blur, sharpen, transform
    // Graph-only
    case combine, mask, output

    var id: String { rawValue }

    /// The stack layer this node runs, when there is one. Nodes without a
    /// layer (sources, combine, mask, output) are handled by the evaluator.
    var layerKind: LayerKind? {
        switch self {
        case .noise: return .noise
        case .gradient: return .gradient
        case .volcano: return .volcano
        case .lava: return .lava
        case .snow: return .snow
        case .hydraulic: return .hydraulic
        case .thermal: return .thermal
        case .fluvial: return .fluvial
        case .terrace: return .terrace
        case .plateau: return .plateau
        case .clamp: return .clamp
        case .curve: return .curve
        case .blur: return .blur
        case .sharpen: return .sharpen
        case .transform: return .transform
        case .terrain, .flat, .combine, .mask, .output: return nil
        }
    }

    var label: String {
        switch self {
        case .terrain: return "Terrain"
        case .flat: return "Flat"
        case .combine: return "Combine"
        case .mask: return "Mask"
        case .output: return "Output"
        default: return layerKind.flatMap { layerDefs[$0]?.label } ?? rawValue.capitalized
        }
    }

    var blurb: String {
        switch self {
        case .terrain: return "Fractal base terrain — the same generator the Base tab drives."
        case .flat: return "An empty field. Start from nothing and build it up."
        case .combine: return "Merges two branches with a blend mode."
        case .mask: return "Bands height, slope or curvature into a 0–1 field for a Mask input."
        case .output: return "What the viewport shows and what exports write."
        default: return layerKind.flatMap { layerDefs[$0]?.blurb } ?? ""
        }
    }

    var category: String {
        switch self {
        case .terrain, .flat: return "Source"
        case .hydraulic, .thermal, .fluvial: return "Erosion"
        case .volcano, .lava, .snow: return "Nature"
        case .noise, .gradient, .terrace, .plateau: return "Shape"
        case .clamp, .curve, .blur, .sharpen, .transform: return "Filter"
        case .combine, .mask, .output: return "Graph"
        }
    }

    var symbol: String {
        switch self {
        case .terrain: return "mountain.2.fill"
        case .flat: return "square.dashed"
        case .output: return "display"
        case .combine: return "arrow.triangle.merge"
        case .mask: return "circle.lefthalf.filled"
        case .hydraulic: return "drop.fill"
        case .fluvial: return "water.waves"
        case .thermal: return "thermometer.medium"
        case .volcano: return "flame.fill"
        case .lava: return "flame"
        case .snow: return "snowflake"
        case .noise: return "waveform.path"
        case .gradient: return "square.righthalf.filled"
        case .terrace: return "stairs"
        case .plateau: return "rectangle.compress.vertical"
        case .clamp: return "arrow.up.and.down.square"
        case .curve: return "point.topleft.down.curvedto.point.bottomright.up"
        case .blur: return "aqi.medium"
        case .sharpen: return "triangle.fill"
        case .transform: return "arrow.up.arrow.down"
        }
    }

    /// Sources have no field input; everything else transforms one.
    var fieldInputs: Int {
        switch self {
        case .terrain, .flat: return 0
        case .combine: return 2
        default: return 1
        }
    }

    /// Only operations that honour the engine's mask expose a Mask port. It is
    /// the same mask the stack's per-layer mask sets, driven by a field
    /// instead of a preset band.
    var takesMask: Bool {
        switch self {
        case .terrain, .flat, .output, .mask: return false
        default: return true
        }
    }

    var hasOutput: Bool { self != .output }

    var inputLabels: [String] {
        switch self {
        case .combine: return ["A", "B"]
        case .output: return ["In"]
        default: return fieldInputs == 1 ? ["In"] : []
        }
    }

    /// Parameters, reusing the stack's definitions wherever the node is a
    /// layer so the two editors cannot describe the same control differently.
    var params: [ParamDef] {
        switch self {
        case .terrain:
            return [
                ParamDef(key: "noiseType", label: "Structure", range: 0...7, step: 1, def: 0,
                         choices: layerNoiseLabels),
                ParamDef(key: "scale", label: "Scale", range: 0.5...12, step: 0.5, def: 3),
                ParamDef(key: "height", label: "Height", range: 1...200, step: 1, def: 60),
                ParamDef(key: "octaves", label: "Octaves", range: 1...12, step: 1, def: 6),
                ParamDef(key: "persistence", label: "Persistence", range: 0.1...0.9, step: 0.05, def: 0.5),
                ParamDef(key: "lacunarity", label: "Lacunarity", range: 1.2...3.5, step: 0.1, def: 2.0),
                ParamDef(key: "exponent", label: "Exponent", range: 0.5...3, step: 0.1, def: 1.0),
                ParamDef(key: "warp", label: "Warp", range: 0...1, step: 0.05, def: 0),
            ]
        case .combine:
            return [
                ParamDef(key: "blend", label: "Blend", range: 0...5, step: 1, def: 0, choices: blendLabels),
                ParamDef(key: "strength", label: "A Strength", range: 0...2, step: 0.05, def: 1.0),
                ParamDef(key: "alpha", label: "Mix Alpha", range: 0...1, step: 0.05, def: 0.5),
            ]
        case .mask:
            return [
                ParamDef(key: "feature", label: "From", range: 0...2, step: 1, def: 0,
                         choices: ["Height", "Slope", "Curvature"]),
                ParamDef(key: "lo", label: "Low", range: 0...1, step: 0.01, def: 0.35),
                ParamDef(key: "hi", label: "High", range: 0...1, step: 0.01, def: 1.0),
                ParamDef(key: "feather", label: "Feather", range: 0...0.5, step: 0.01, def: 0.05),
                ParamDef(key: "invert", label: "Invert", range: 0...1, step: 1, def: 0,
                         choices: ["No", "Yes"]),
            ]
        case .flat, .output:
            return []
        default:
            return layerKind.flatMap { layerDefs[$0]?.params } ?? []
        }
    }
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

struct GraphNode: Identifiable {
    let id: UUID
    var kind: NodeKind
    var position: CGPoint
    var params: [String: Double]
    var curve: [CurvePoint]? = nil
    var enabled = true
    /// Renamed by the user. Empty means "use the kind's label".
    var name: String = ""

    init(kind: NodeKind, at position: CGPoint, id: UUID = UUID()) {
        self.id = id
        self.kind = kind
        self.position = position
        var p: [String: Double] = [:]
        for def in kind.params { p[def.key] = def.def }
        self.params = p
        if kind == .curve { self.curve = defaultCurve }
        // A volcano needs somewhere to be; the stack version is placed by
        // clicking the terrain, which a graph node has no equivalent of.
        if kind == .volcano {
            self.params["x"] = 0.5
            self.params["y"] = 0.5
        }
    }

    var title: String { name.isEmpty ? kind.label : name }
}

/// A connection into `to`'s port. Port -1 is the mask input; 0.. are fields.
struct GraphEdge: Identifiable, Hashable {
    let id: UUID
    var from: UUID
    var to: UUID
    var port: Int

    init(from: UUID, to: UUID, port: Int, id: UUID = UUID()) {
        self.id = id
        self.from = from
        self.to = to
        self.port = port
    }
}

let maskPort = -1

final class TerrainGraph: ObservableObject {
    @Published var nodes: [GraphNode] = []
    @Published var edges: [GraphEdge] = []
    /// The node whose result the viewport shows. Nil follows the Output node.
    /// Every node editor worth using lets you look at an intermediate result
    /// without rewiring the graph to get there.
    @Published var previewNode: UUID? = nil

    var outputID: UUID? { nodes.first { $0.kind == .output }?.id }

    func node(_ id: UUID) -> GraphNode? { nodes.first { $0.id == id } }

    func index(_ id: UUID) -> Int? { nodes.firstIndex { $0.id == id } }

    func source(of node: UUID, port: Int) -> UUID? {
        edges.first { $0.to == node && $0.port == port }?.from
    }

    func consumers(of node: UUID) -> [UUID] {
        edges.filter { $0.from == node }.map(\.to)
    }

    // MARK: Editing

    @discardableResult
    func add(_ kind: NodeKind, at point: CGPoint) -> UUID {
        let node = GraphNode(kind: kind, at: point)
        nodes.append(node)
        return node.id
    }

    func remove(_ id: UUID) {
        guard node(id)?.kind != .output else { return }  // the graph needs one
        nodes.removeAll { $0.id == id }
        edges.removeAll { $0.from == id || $0.to == id }
        if previewNode == id { previewNode = nil }
    }

    /// Connects, replacing whatever occupied that input — an input takes one
    /// wire, which is what every node editor does and what keeps evaluation
    /// unambiguous. Refuses a connection that would close a loop.
    @discardableResult
    func connect(from: UUID, to: UUID, port: Int) -> Bool {
        guard from != to, node(from)?.kind.hasOutput == true else { return false }
        guard !reaches(from: to, target: from) else { return false }
        edges.removeAll { $0.to == to && $0.port == port }
        edges.append(GraphEdge(from: from, to: to, port: port))
        return true
    }

    func disconnect(to: UUID, port: Int) {
        edges.removeAll { $0.to == to && $0.port == port }
    }

    /// Is `target` downstream of `from`? Used to keep the graph acyclic.
    func reaches(from: UUID, target: UUID) -> Bool {
        var seen: Set<UUID> = []
        var stack = [from]
        while let cur = stack.popLast() {
            if cur == target { return true }
            if !seen.insert(cur).inserted { continue }
            stack.append(contentsOf: consumers(of: cur))
        }
        return false
    }

    /// Splices a node into an existing wire — dropping a node on a connection
    /// is how Blender, Unreal and Substance all insert into a chain.
    func insert(_ id: UUID, into edge: GraphEdge) {
        guard let kind = node(id)?.kind, kind.fieldInputs > 0, kind.hasOutput else { return }
        guard !reaches(from: edge.to, target: id), !reaches(from: id, target: edge.from) else { return }
        edges.removeAll { $0.id == edge.id }
        edges.append(GraphEdge(from: edge.from, to: id, port: 0))
        edges.append(GraphEdge(from: id, to: edge.to, port: edge.port))
    }

    // MARK: Conversion

    /// Lays the current layer stack out as a graph. The stack is a graph that
    /// happens to be a line, so opening the drawer should show the work
    /// already done rather than an empty canvas asking to start over.
    static func from(stack: [TitanLayer], base: [String: Double]) -> TerrainGraph {
        let g = TerrainGraph()
        var x: CGFloat = 80
        let y: CGFloat = 260
        let step: CGFloat = 210

        var terrain = GraphNode(kind: .terrain, at: CGPoint(x: x, y: y))
        for (k, v) in base where terrain.params[k] != nil { terrain.params[k] = v }
        g.nodes.append(terrain)
        var previous = terrain.id
        x += step

        for layer in stack {
            guard let kind = NodeKind.allCases.first(where: { $0.layerKind == layer.kind }) else {
                continue  // .water and the stack's imported-field .combine have no node form
            }
            var node = GraphNode(kind: kind, at: CGPoint(x: x, y: y))
            node.params = layer.params
            node.curve = layer.curve
            node.enabled = layer.enabled
            g.nodes.append(node)
            g.edges.append(GraphEdge(from: previous, to: node.id, port: 0))
            previous = node.id
            x += step
        }

        let output = GraphNode(kind: .output, at: CGPoint(x: x, y: y))
        g.nodes.append(output)
        g.edges.append(GraphEdge(from: previous, to: output.id, port: 0))
        return g
    }

    /// The graph read back as a stack, when it is still a straight line.
    /// Returns nil once it forks — at which point the stack genuinely cannot
    /// express it, and saying so is better than silently dropping branches.
    func asStack() -> [TitanLayer]? {
        guard let outID = outputID else { return nil }
        var chain: [GraphNode] = []
        var cursor = source(of: outID, port: 0)
        var guardCount = 0
        while let id = cursor, let n = node(id), guardCount < 512 {
            guardCount += 1
            if n.kind == .terrain || n.kind == .flat { break }
            guard n.kind.layerKind != nil else { return nil }        // combine/mask: not a line
            guard source(of: id, port: maskPort) == nil else { return nil }
            guard consumers(of: id).count == 1 else { return nil }   // feeds two places
            chain.append(n)
            cursor = source(of: id, port: 0)
        }
        guard let rootID = cursor, node(rootID)?.kind == .terrain || node(rootID)?.kind == .flat
        else { return nil }

        return chain.reversed().map { n in
            var layer = TitanLayer(kind: n.kind.layerKind!, params: n.params)
            layer.enabled = n.enabled
            layer.curve = n.curve
            return layer
        }
    }

    /// A starter graph: base terrain, some erosion, out. Same shape as the
    /// stack a new document starts with.
    static func starter() -> TerrainGraph {
        let g = TerrainGraph()
        let t = g.add(.terrain, at: CGPoint(x: 80, y: 240))
        let h = g.add(.hydraulic, at: CGPoint(x: 320, y: 240))
        let o = g.add(.output, at: CGPoint(x: 560, y: 240))
        g.connect(from: t, to: h, port: 0)
        g.connect(from: h, to: o, port: 0)
        return g
    }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

/// A value copy of the graph, so evaluation can run off the main thread
/// without holding a reference to the observable document.
struct GraphPlan {
    var nodes: [UUID: GraphNode] = [:]
    var incoming: [UUID: [Int: UUID]] = [:]
    var root: UUID?

    init(_ graph: TerrainGraph) {
        for n in graph.nodes { nodes[n.id] = n }
        for e in graph.edges { incoming[e.to, default: [:]][e.port] = e.from }
        // Previewing an intermediate node evaluates only what feeds it, which
        // is the point: you can look at an expensive branch without paying for
        // the rest of the graph.
        root = graph.previewNode.flatMap { nodes[$0] != nil ? $0 : nil }
            ?? graph.outputID.flatMap { graph.source(of: $0, port: 0) }
    }
}

enum GraphEvaluator {
    struct Result {
        var thumbnails: [UUID: CGImage] = [:]
        var evaluated = 0
        var error: String? = nil
    }

    /// Evaluates the plan, leaving the engine holding the root's terrain.
    ///
    /// Depth-first post-order, memoized: a node feeding two consumers is
    /// computed once, and the root is executed last so whatever it left in the
    /// engine beyond the height field — lava, snow, water — is still there for
    /// the mesh. That is why this returns fields rather than only the root's:
    /// a fork has to park one branch while the other runs.
    static func evaluate(plan: GraphPlan, engine: OpaquePointer?, gridSize: Int32,
                         cellSize: Float, seed: UInt32,
                         thumbnails: Bool,
                         progress: (String) -> Void) -> Result {
        var result = Result()
        guard let root = plan.root else {
            result.error = "Nothing is connected to Output"
            return result
        }

        let count = Int(gridSize) * Int(gridSize)
        var cache: [UUID: [Float]] = [:]
        var visiting: Set<UUID> = []

        // Whose field the engine is currently carrying.
        //
        // A cached field is only the surface. titan_set_height takes it as
        // bedrock and clears everything derived — sediment, flow, snow, water,
        // lava, and the volcano vent list — because a bare height field has no
        // history. That is correct for a fork, where the engine genuinely has
        // to be rewound, and destructive everywhere else: a Lava node fed by a
        // Volcano node restored its input, wiped the vents the volcano had just
        // registered, and SimulateLava returned immediately with nothing to
        // erupt. Volcano -> Lava produced no lava at all, while the same two
        // layers in the stack do, because the stack never rewinds.
        //
        // So don't rewind when there is nothing to rewind to: if the engine is
        // already holding exactly the field this node wants, leave it alone and
        // the derived state survives. A straight chain now runs the identical
        // call sequence the stack runs, which is the point of the node sharing
        // the layer's execute(). Only an actual fork pays for a restore.
        var engineHolds: UUID? = nil

        /// Puts `field` on the engine unless it is already there.
        func place(_ field: [Float], from src: UUID?) {
            if let src, src == engineHolds { return }
            restore(engine, field)
            engineHolds = src
        }

        func fieldOf(_ id: UUID) -> [Float]? {
            if let cached = cache[id] { return cached }
            guard let node = plan.nodes[id] else { return nil }
            // connect() rejects cycles, but evaluation must not hang even if a
            // document arrives from elsewhere with one in it.
            guard visiting.insert(id).inserted else {
                result.error = "The graph contains a loop"
                return nil
            }
            defer { visiting.remove(id) }

            // Inputs first, so the engine is free to be rewound below.
            var inputs: [[Float]] = []
            for port in 0..<node.kind.fieldInputs {
                guard let src = plan.incoming[id]?[port], let f = fieldOf(src) else {
                    inputs.append([Float](repeating: 0, count: count))
                    continue
                }
                inputs.append(f)
            }
            var maskField: [Float]? = nil
            if node.kind.takesMask, let src = plan.incoming[id]?[maskPort] {
                maskField = fieldOf(src)
            }

            progress(node.title + "…")

            // A disabled node passes its input through untouched, the same as
            // unticking a layer in the stack.
            if !node.enabled, let passthrough = inputs.first {
                if id == root { place(passthrough, from: plan.incoming[id]?[0]) }
                cache[id] = passthrough
                return passthrough
            }

            switch node.kind {
            case .terrain:
                func f(_ k: String) -> Float { Float(node.params[k] ?? 0) }
                let typeIndex = Int(node.params["noiseType"] ?? 0)
                titan_configure(engine, gridSize, cellSize, f("scale"), f("height"), seed,
                                Int32(node.params["octaves"] ?? 6), f("persistence"),
                                f("lacunarity"), f("exponent"),
                                Int32(layerNoiseIDs[min(max(typeIndex, 0), layerNoiseIDs.count - 1)]),
                                f("warp"), 1.0, 2.0, 0.0, 0.0)
                titan_generate(engine)
            case .flat:
                titan_clear_terrain(engine)
            case .output:
                if let passthrough = inputs.first {
                    place(passthrough, from: plan.incoming[id]?[0])
                }
            case .mask:
                place(inputs[0], from: plan.incoming[id]?[0])
                let feature = Int32(node.params["feature"] ?? 0)
                titan_mask_by_feature(engine, feature, Float(node.params["lo"] ?? 0),
                                      Float(node.params["hi"] ?? 1),
                                      Float(node.params["feather"] ?? 0.05),
                                      Int32(node.params["invert"] ?? 0))
                // A mask node's *output* is the band itself, not terrain, so it
                // is read from scratch rather than from the height field.
                var band = [Float](repeating: 0, count: count)
                if let scratch = titan_scratch_ptr(engine) {
                    for i in 0..<count { band[i] = scratch[i] }
                }
                cache[id] = band
                // Previewing a mask shows the band itself, which means putting
                // it on the engine as if it were terrain — so the engine is no
                // longer holding this node's input.
                if id == root {
                    restore(engine, band)
                    engineHolds = id
                }
                if thumbnails, let img = makeThumbnail(band, size: Int(gridSize), grayscale: true) {
                    result.thumbnails[id] = img
                }
                result.evaluated += 1
                return band
            case .combine:
                // B is the surface; A is blended onto it. Two branches meet
                // here through the same combiner the stack's import path uses.
                place(inputs[1], from: plan.incoming[id]?[1])
                inputs[0].withUnsafeBufferPointer { buf in
                    titan_apply_heightfield(engine, buf.baseAddress, gridSize,
                                            Float(node.params["strength"] ?? 1),
                                            Int32(node.params["blend"] ?? 0),
                                            Float(node.params["alpha"] ?? 0.5))
                }
            default:
                place(inputs[0], from: plan.incoming[id]?[0])
                let masked = activate(mask: maskField, on: engine, count: count)
                var layer = TitanLayer(kind: node.kind.layerKind!, params: node.params)
                layer.curve = node.curve
                EngineModel.execute(layer, on: engine, gridSize: gridSize,
                                    heightMultiplier: 1.0, imported: nil)
                if masked { titan_set_mask(engine, nil, 0) }
            }

            // Everything reaching here ran on the engine, so the engine is now
            // carrying this node's result — including the derived fields a
            // cached height field cannot describe.
            engineHolds = id

            var out = [Float](repeating: 0, count: count)
            out.withUnsafeMutableBufferPointer { buf in
                titan_read_height(engine, buf.baseAddress, Int32(count))
            }
            cache[id] = out
            result.evaluated += 1
            if thumbnails, let img = makeThumbnail(out, size: Int(gridSize), grayscale: false) {
                result.thumbnails[id] = img
            }
            return out
        }

        _ = fieldOf(root)
        return result
    }

    private static func restore(_ engine: OpaquePointer?, _ field: [Float]) {
        field.withUnsafeBufferPointer { buf in
            titan_set_height(engine, buf.baseAddress, Int32(field.count))
        }
    }

    /// A Mask input drives the same engine mask a stack layer's mask sets —
    /// the field arrives already 0–1 from a Mask node, or as terrain from
    /// anywhere else, in which case it is normalized to its own range.
    private static func activate(mask: [Float]?, on engine: OpaquePointer?, count: Int) -> Bool {
        guard var m = mask, m.count == count else { return false }
        var lo = Float.greatestFiniteMagnitude
        var hi = -Float.greatestFiniteMagnitude
        for v in m { lo = min(lo, v); hi = max(hi, v) }
        if hi > 1.0 || lo < 0.0 {
            let span = hi - lo > 0 ? hi - lo : 1
            for i in 0..<count { m[i] = (m[i] - lo) / span }
        }
        m.withUnsafeBufferPointer { buf in
            titan_set_mask(engine, buf.baseAddress, Int32(count))
        }
        return true
    }

    /// Node thumbnails, the way Substance Designer and Gaea show them: the
    /// graph is unreadable without being able to see what each step did.
    static func makeThumbnail(_ field: [Float], size: Int, grayscale: Bool,
                              side: Int = 84) -> CGImage? {
        guard size > 1, field.count >= size * size else { return nil }
        var small = [Float](repeating: 0, count: side * side)
        let stepF = Float(size - 1) / Float(side - 1)
        for y in 0..<side {
            let sy = min(size - 1, Int(Float(y) * stepF))
            for x in 0..<side {
                let sx = min(size - 1, Int(Float(x) * stepF))
                small[y * side + x] = field[sy * size + sx]
            }
        }
        return EngineModel.colorize(small, size: side, seaLevel: -.greatestFiniteMagnitude,
                                    grayscale: grayscale)
    }
}
