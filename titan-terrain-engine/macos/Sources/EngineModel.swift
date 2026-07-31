// EngineModel — the app's single source of truth. Owns the libTitanCore
// handle, runs the layer pipeline on a background queue, and publishes mesh
// snapshots for the renderer. All terrain math stays in C++.
//
// The layer stack, presets, and .titan project format mirror the web lab
// (src/core/pipeline.ts) exactly — a project saved on either platform loads
// on the other and reproduces the same terrain.

import CoreGraphics
import Foundation
import ImageIO
import SwiftUI

/// Stable seed-string hash, computed by libTitanCore.
///
/// Deliberately delegates to the engine instead of reimplementing FNV-1a.
/// This app hashed UTF-8 bytes, the web lab hashed UTF-16 code units, and the
/// Unreal plugin hashed truncated wide characters — three "identical" hashes
/// that agreed only on ASCII, so any seed with an accent or emoji produced
/// three different terrains. titan_hash_seed is the single definition now.
func hashSeed(_ seed: String) -> UInt32 {
    return seed.withCString { titan_hash_seed($0) }
}

func randomSeed() -> String {
    let chars = "abcdefghijklmnopqrstuvwxyz0123456789"
    return String((0..<7).map { _ in chars.randomElement()! })
}

/// Project file version, shared with the web lab (src/core/pipeline.ts).
///
/// 1 — world extent was implicitly the sample count (engine cellSize 1.0).
/// 2 — `worldSize` is explicit and `size` is pure sample density.
/// 3 — curve layers carry arbitrary control points instead of five fixed ones.
///
/// Old files always load: v1 sets worldSize = size, exactly what the old model
/// computed, and a v2 curve layer is rebuilt from its y0..y4 params.
///
/// A file is stamped with the *minimum* version needed to read it correctly,
/// not simply the newest this build knows, so a project using no custom curve
/// still opens in a build that predates them.
let titanProjectVersion = 3

/// A control point of a height-remap curve. Both axes are 0..1.
struct CurvePoint: Equatable {
    var x: Double
    var y: Double
}

let defaultCurve: [CurvePoint] = [
    CurvePoint(x: 0, y: 0), CurvePoint(x: 0.25, y: 0.25), CurvePoint(x: 0.5, y: 0.5),
    CurvePoint(x: 0.75, y: 0.75), CurvePoint(x: 1, y: 1),
]

let maxCurvePoints = 12

/// Sorted by x, endpoints pinned, everything inside the unit square.
func sanitizeCurve(_ raw: [CurvePoint]) -> [CurvePoint]? {
    guard raw.count >= 2 else { return nil }
    var pts = raw.prefix(maxCurvePoints).map {
        CurvePoint(x: min(1, max(0, $0.x)), y: min(1, max(0, $0.y)))
    }
    pts.sort { $0.x < $1.x }
    // The engine's spline assumes strictly increasing x; equal x would divide
    // by a clamped epsilon and produce a near-vertical segment.
    pts[0].x = 0
    pts[pts.count - 1].x = 1
    for i in 1..<pts.count where pts[i].x <= pts[i - 1].x {
        pts[i].x = min(1, pts[i - 1].x + 1e-3)
    }
    return pts
}

// Names shared with the web lab's .titan format.
let noiseTypeNames = ["none", "standard", "ridged", "billow", "voronoi",
                      "voronoiRidge", "worleyManhattan", "worleyChebyshev", "hybrid"]
let biomeNames = ["arctic", "temperate", "volcanic", "desert"]

struct MeshSnapshot {
    // pos(3) + normal(3) + splat(4) + lava(4) + surface(4), stride 18 floats.
    // The lava attribute is molten depth, heat, chilled-rock depth, glow —
    // always present (zeroed without volcanism) so the vertex layout and the
    // shader are the same whether or not anything erupted. The surface
    // attribute is ambient occlusion, curvature, snow depth and water depth,
    // all simulated by the engine.
    var interleaved: [Float]
    var indices: [UInt32]
    var vertexCount: Int
    var heightMax: Float
    /// Edge length in world units (size * cellSize).
    var terrainExtent: Float
    /// Samples per edge, for world <-> cell conversion in the picker.
    var gridSize: Int
}

struct FloraInstance {
    var posRot: SIMD4<Float>  // xyz position, w = rotation around Y
    var scale: SIMD4<Float>   // xyz scale, w unused
}

// ---------------------------------------------------------------------------
// Layer stack — mirrors src/core/pipeline.ts
// ---------------------------------------------------------------------------

enum LayerKind: String, CaseIterable {
    // v0.6 additions (snow, water) were engine-only until now.
    case fluvial, hydraulic, thermal, terrace, plateau
    case noise, gradient, clamp, curve, blur, sharpen, transform, combine
    case snow
    case water
    // v0.7 volcanism. One `volcano` layer per cone; a single `lava` layer
    // erupts every vent above it.
    case volcano
    case lava
}

// Per-layer mask: 0 off, 1 height, 2 slope, 3 curvature, 4 noise.
struct LayerMask {
    var mode = 0
    var lo = 0.35
    var hi = 1.0
    var invert = false
}

let maskModeLabels = ["Off", "Height", "Slope", "Curve", "Noise"]

struct TitanLayer: Identifiable {
    let id = UUID()
    var kind: LayerKind
    var enabled = true
    var params: [String: Double]
    var mask = LayerMask()
    /// Height-remap control points, for `.curve` layers. The engine has always
    /// accepted an arbitrary number; the UI offered five fixed sliders.
    var curve: [CurvePoint]? = nil
}

struct ParamDef {
    let key: String
    let label: String
    let range: ClosedRange<Double>
    let step: Double
    let def: Double
    var choices: [String]? = nil
    // Advanced params sit behind a disclosure so the common case stays short.
    // These map onto the engine's _ex erosion entry points, which shipped in
    // v0.4 and were reachable from no UI until now.
    var advanced: Bool = false
}

struct LayerDef {
    let label: String
    let blurb: String
    let params: [ParamDef]
}

let layerNoiseLabels = ["Simplex", "Ridged", "Billow", "Cells", "Walls", "Blocks", "Squares", "Hybrid"]
let layerNoiseIDs: [Int32] = [1, 2, 3, 4, 5, 6, 7, 8]
let blendLabels = ["Add", "Subtract", "Multiply", "Max", "Min", "Mix"]

let layerOrder: [LayerKind] = [.fluvial, .hydraulic, .thermal, .snow, .water, .terrace, .plateau,
                               .volcano, .lava,
                               .noise, .gradient, .clamp, .curve, .blur, .sharpen,
                               .transform, .combine]

let layerDefs: [LayerKind: LayerDef] = [
    .fluvial: LayerDef(
        label: "River Networks",
        blurb: "Routes rainfall map-wide and carves connected drainage networks (stream power).",
        params: [
            ParamDef(key: "passes", label: "Passes", range: 1...10, step: 1, def: 2),
            ParamDef(key: "strength", label: "Strength", range: 0.1...3, step: 0.1, def: 1.0),
            ParamDef(key: "erodeConstant", label: "K (erodibility)", range: 0.001...0.1, step: 0.001, def: 0.015, advanced: true),
            ParamDef(key: "areaExponent", label: "m (area)", range: 0.1...1.5, step: 0.05, def: 0.5, advanced: true),
            ParamDef(key: "slopeExponent", label: "n (slope)", range: 0.5...2.5, step: 0.05, def: 1.0, advanced: true),
            ParamDef(key: "depositRatio", label: "Deposit Ratio", range: 0...1, step: 0.05, def: 0.3, advanced: true),
            ParamDef(key: "maxStep", label: "Max Step", range: 0.1...10, step: 0.1, def: 2.0, advanced: true),
        ]),
    .hydraulic: LayerDef(
        label: "Hydraulic Erosion",
        blurb: "Droplet simulation carving riverbeds and depositing sediment.",
        params: [
            ParamDef(key: "iterations", label: "Droplets", range: 16384...196608, step: 16384, def: 49152),
            ParamDef(key: "spawnMode", label: "Rainfall", range: 0...1, step: 1, def: 0, choices: ["Uniform", "Highlands"]),
            ParamDef(key: "inertia", label: "Inertia", range: 0...0.95, step: 0.05, def: 0.1, advanced: true),
            ParamDef(key: "capacity", label: "Sediment Capacity", range: 0.5...16, step: 0.5, def: 4.0, advanced: true),
            ParamDef(key: "dissolve", label: "Dissolve Rate", range: 0.01...1, step: 0.01, def: 0.1, advanced: true),
            ParamDef(key: "deposit", label: "Deposit Rate", range: 0.01...1, step: 0.01, def: 0.1, advanced: true),
            ParamDef(key: "evaporate", label: "Evaporation", range: 0.001...0.1, step: 0.001, def: 0.01, advanced: true),
            ParamDef(key: "gravity", label: "Gravity", range: 0.5...20, step: 0.5, def: 4.0, advanced: true),
            ParamDef(key: "lifetime", label: "Droplet Lifetime", range: 8...128, step: 1, def: 60, advanced: true),
            ParamDef(key: "radius", label: "Erosion Radius", range: 1...12, step: 1, def: 3, advanced: true),
            ParamDef(key: "bedrockSpeed", label: "Bedrock Rate", range: 0.005...0.5, step: 0.005, def: 0.05, advanced: true),
        ]),
    .thermal: LayerDef(
        label: "Thermal Weathering",
        blurb: "Loose material settles to the angle of repose, forming talus slopes.",
        params: [
            ParamDef(key: "passes", label: "Passes", range: 1...50, step: 1, def: 10),
            ParamDef(key: "talusAngle", label: "Repose Angle", range: 20...45, step: 1, def: 33),
            ParamDef(key: "rate", label: "Rate", range: 0.1...1, step: 0.05, def: 0.5),
            ParamDef(key: "bedrockBreakdown", label: "Bedrock Breakdown", range: 0...0.5, step: 0.01, def: 0.05, advanced: true),
        ]),
    .terrace: LayerDef(
        label: "Terrace",
        blurb: "Steps the terrain into geological banding.",
        params: [
            ParamDef(key: "interval", label: "Interval", range: 2...30, step: 0.5, def: 10),
            ParamDef(key: "strength", label: "Strength", range: 0...1, step: 0.05, def: 0.7),
            ParamDef(key: "sharpness", label: "Sharpness", range: 1...6, step: 0.5, def: 2),
        ]),
    .plateau: LayerDef(
        label: "Plateau",
        blurb: "Compresses peaks toward a ceiling with a rounded shoulder.",
        params: [
            ParamDef(key: "height", label: "Height", range: 5...200, step: 1, def: 60),
            ParamDef(key: "softness", label: "Softness", range: 1...40, step: 1, def: 10),
        ]),
    .noise: LayerDef(
        label: "Add Noise",
        blurb: "Stacks a second noise field onto the terrain with a blend mode.",
        params: [
            ParamDef(key: "noiseType", label: "Type", range: 0...7, step: 1, def: 0, choices: layerNoiseLabels),
            ParamDef(key: "blend", label: "Blend", range: 0...5, step: 1, def: 0, choices: blendLabels),
            ParamDef(key: "scale", label: "Scale", range: 0.5...12, step: 0.5, def: 4),
            ParamDef(key: "amplitude", label: "Amplitude", range: 1...80, step: 1, def: 15),
            ParamDef(key: "octaves", label: "Octaves", range: 1...10, step: 1, def: 5),
            ParamDef(key: "alpha", label: "Mix Alpha", range: 0...1, step: 0.05, def: 0.5),
            ParamDef(key: "seedOffset", label: "Variant", range: 0...9, step: 1, def: 1),
        ]),
    .gradient: LayerDef(
        label: "Gradient",
        blurb: "Linear or radial height ramp across the whole map.",
        params: [
            ParamDef(key: "kind", label: "Shape", range: 0...1, step: 1, def: 0, choices: ["Linear", "Radial"]),
            ParamDef(key: "op", label: "Operation", range: 0...2, step: 1, def: 0, choices: ["Raise", "Lower", "Union"]),
            ParamDef(key: "angle", label: "Angle", range: 0...360, step: 5, def: 0),
            ParamDef(key: "height", label: "Height", range: 1...100, step: 1, def: 25),
        ]),
    .clamp: LayerDef(
        label: "Clamp",
        blurb: "Clips terrain heights into a min/max band — including below the datum for sea floors.",
        params: [
            // Negative minimum is meaningful now that the engine represents
            // below-datum terrain — that is what makes "sea floors" real.
            ParamDef(key: "min", label: "Min Height", range: -100...200, step: 1, def: 0),
            ParamDef(key: "max", label: "Max Height", range: 1...200, step: 1, def: 60),
        ]),
    .curve: LayerDef(
        label: "Curves",
        blurb: "Custom height remap over the terrain's own height range, like Photoshop curves. Drag the points; click the curve to add one, double-click a point to remove it.",
        // No sliders: this layer is edited through the curve widget, which
        // writes layer.curve. The engine has always taken arbitrary points.
        params: []),
    .blur: LayerDef(
        label: "Blur",
        blurb: "Smooths the terrain. Pair with a slope mask to soften only the flats.",
        params: [
            ParamDef(key: "radius", label: "Radius", range: 1...10, step: 1, def: 2),
            ParamDef(key: "strength", label: "Strength", range: 0...1, step: 0.05, def: 1),
        ]),
    .sharpen: LayerDef(
        label: "Sharpen",
        blurb: "Unsharp mask — amplifies ridgelines and surface detail.",
        params: [
            ParamDef(key: "radius", label: "Radius", range: 1...10, step: 1, def: 2),
            ParamDef(key: "strength", label: "Strength", range: 0.1...3, step: 0.1, def: 0.8),
        ]),
    .transform: LayerDef(
        label: "Transform",
        blurb: "Vertical scale, height offset, and terrain inversion.",
        params: [
            ParamDef(key: "scale", label: "V-Scale", range: 0.25...3, step: 0.05, def: 1),
            ParamDef(key: "offset", label: "Offset", range: -30...30, step: 1, def: 0),
            ParamDef(key: "invert", label: "Invert", range: 0...1, step: 1, def: 0, choices: ["Off", "On"]),
        ]),
    .snow: LayerDef(
        label: "Snow",
        blurb: "Accumulates snow above an altitude line, sheds it off steep faces, then settles and melts it.",
        params: [
            ParamDef(key: "snowLine", label: "Snow Line", range: 0...1, step: 0.05, def: 0.55),
            ParamDef(key: "amount", label: "Depth", range: 0.5...30, step: 0.5, def: 6),
            // Titan terrain is steep — a median slope near 60 degrees at
            // default params — so the shed angle sits high for snow to hold.
            ParamDef(key: "maxSlopeDeg", label: "Shed Angle", range: 20...85, step: 1, def: 65),
            ParamDef(key: "settlePasses", label: "Settle Passes", range: 0...32, step: 1, def: 8),
            ParamDef(key: "melt", label: "Melt", range: 0...1, step: 0.05, def: 0.35),
        ]),
    .water: LayerDef(
        label: "Lakes",
        blurb: "Priority-flood fill: depressions that cannot drain to the map edge fill with water.",
        params: []),
    .volcano: LayerDef(
        label: "Volcano",
        blurb: "Drops a stratovolcano onto the terrain — concave flanks, a jagged summit crater, barranca gullies, and a rim breached on one side for lava to pour through. Add one layer per cone.",
        params: [
            // Position is normalized 0..1 across the grid, not in cells, so a
            // volcano stays where it was dropped when the resolution changes.
            ParamDef(key: "x", label: "Position X", range: 0...1, step: 0.005, def: 0.5),
            ParamDef(key: "y", label: "Position Y", range: 0...1, step: 0.005, def: 0.5),
            // Wide and comparatively low. A cone whose height rivals its
            // radius reads as a blade, not a volcano — these defaults are
            // what a click on the terrain drops, so they have to look right
            // untouched.
            ParamDef(key: "radius", label: "Base Radius", range: 0.04...0.5, step: 0.01, def: 0.28),
            ParamDef(key: "height", label: "Summit Height", range: 5...300, step: 1, def: 55),
            ParamDef(key: "craterRadius", label: "Crater Size", range: 0.03...0.5, step: 0.01, def: 0.16),
            ParamDef(key: "craterDepth", label: "Crater Depth", range: 0...0.6, step: 0.01, def: 0.16),
            ParamDef(key: "rimJaggedness", label: "Rim Jaggedness", range: 0...1, step: 0.05, def: 0.6),
            ParamDef(key: "roughness", label: "Surface Detail", range: 0...1, step: 0.05, def: 0.5),
            ParamDef(key: "coneExponent", label: "Flank Profile", range: 0.8...3, step: 0.05, def: 1.75, advanced: true),
            // -1 lets the seed choose, so a field of volcanoes does not all
            // breach in the same direction.
            ParamDef(key: "breachAngle", label: "Breach Bearing", range: -1...360, step: 1, def: -1, advanced: true),
            ParamDef(key: "breachWidth", label: "Breach Width", range: 10...140, step: 5, def: 46, advanced: true),
            ParamDef(key: "variant", label: "Variant", range: 0...99, step: 1, def: 0, advanced: true),
        ]),
    .lava: LayerDef(
        label: "Lava Flow",
        blurb: "Erupts every volcano above it in the stack. Lava pools in the craters, spills through the breached rims, and runs downhill as channelled streams — chilling into basalt that diverts what follows.",
        params: [
            ParamDef(key: "steps", label: "Flow Length", range: 50...2500, step: 50, def: 600),
            ParamDef(key: "eruptionRate", label: "Eruption Rate", range: 0.1...8, step: 0.1, def: 1.5),
            // Low = fluid basalt running out into long streams; high = stiff
            // lava piling into short, thick lobes near the vent.
            ParamDef(key: "viscosity", label: "Viscosity", range: 0...1, step: 0.05, def: 0.35),
            ParamDef(key: "sustain", label: "Eruption", range: 0...1, step: 1, def: 1, choices: ["Single Burst", "Continuous"]),
            ParamDef(key: "coolRate", label: "Cooling", range: 0.0002...0.02, step: 0.0002, def: 0.0015, advanced: true),
            ParamDef(key: "solidifyRate", label: "Solidify Rate", range: 0.002...0.2, step: 0.002, def: 0.02, advanced: true),
            ParamDef(key: "ventRadius", label: "Vent Radius", range: 1...20, step: 0.5, def: 2.5, advanced: true),
        ]),
    .combine: LayerDef(
        label: "Combine Import",
        blurb: "Blends the imported heightmap into the terrain (no-op until one is imported).",
        params: [
            ParamDef(key: "blend", label: "Blend", range: 0...5, step: 1, def: 3, choices: blendLabels),
            ParamDef(key: "strength", label: "Strength", range: 0.1...2, step: 0.05, def: 1),
            ParamDef(key: "alpha", label: "Mix Alpha", range: 0...1, step: 0.05, def: 0.5),
        ]),
]

func makeLayer(_ kind: LayerKind) -> TitanLayer {
    var params: [String: Double] = [:]
    for p in layerDefs[kind]!.params { params[p.key] = p.def }
    var layer = TitanLayer(kind: kind, params: params)
    if kind == .curve { layer.curve = defaultCurve }
    return layer
}

/// Samples the remap curve the engine would apply, for previewing it.
///
/// Calls into C++ rather than reimplementing the monotone-cubic spline in
/// Swift (and again in TypeScript): a curve editor whose preview disagrees
/// with the result is worse than no editor.
func sampleCurve(_ points: [CurvePoint], samples: Int) -> [Float] {
    guard points.count >= 2, samples >= 2 else { return [] }
    let xs = points.map { Float($0.x) }
    let ys = points.map { Float($0.y) }
    var out = [Float](repeating: 0, count: samples)
    xs.withUnsafeBufferPointer { xb in
        ys.withUnsafeBufferPointer { yb in
            out.withUnsafeMutableBufferPointer { ob in
                titan_sample_curve(xb.baseAddress, yb.baseAddress,
                                   Int32(points.count), ob.baseAddress, Int32(samples))
            }
        }
    }
    return out
}

// ---------------------------------------------------------------------------
// Presets — mirror PRESETS in src/core/pipeline.ts
// ---------------------------------------------------------------------------

struct Preset: Identifiable {
    let id = UUID()
    let name: String
    let tagline: String
    let apply: (EngineModel) -> Void
}

private func presetLayer(_ kind: LayerKind, _ params: [String: Double],
                         mask: LayerMask? = nil,
                         curve: [CurvePoint]? = nil) -> TitanLayer {
    var layer = makeLayer(kind)
    for (k, v) in params { layer.params[k] = v }
    if let mask { layer.mask = mask }
    if let curve { layer.curve = curve }
    return layer
}

// ---------------------------------------------------------------------------
// EngineModel
// ---------------------------------------------------------------------------

struct ProbeData: Equatable {
    var x = 0, y = 0
    var height: Float = 0, sediment: Float = 0, flow: Float = 0, slope: Float = 0
}

@MainActor
final class EngineModel: ObservableObject {
    // Base parameters (mirror the web lab defaults; flat start, fresh seed).
    @Published var size: Double = 128       // 64...2048, step 64 — matches web slider
    /// Edge length of the terrain in world units.
    ///
    /// Split from `size` in .titan v2. Both apps used to hardcode the engine's
    /// cellSize to 1.0, which made the world extent equal to the sample count:
    /// raising Resolution widened the map while Height stayed absolute, so the
    /// same seed came out dramatically flatter at higher settings. Resolution
    /// is now detail density and this is the world. The default matches the
    /// historical extent so existing sessions look unchanged.
    @Published var worldSize: Double = 128  // 64...8192, step 64

    // Preview mesh cap: the engine simulates and exports at full resolution;
    // the mesh handed to Metal is decimated to this many vertices per edge.
    // Decoupling the two is what lets the resolution slider go past 512.
    static let previewMaxEdgeVertices: Int32 = 512
    @Published var scale: Double = 2.0
    @Published var heightMultiplier: Double = 40
    @Published var octaves: Double = 6
    @Published var persistence: Double = 0.5
    @Published var lacunarity: Double = 2.0
    @Published var exponent: Double = 1.2
    @Published var warpStrength: Double = 0.5
    @Published var noiseType: Int = 0       // index into noiseTypeNames
    @Published var biome: Int = 1           // index into biomeNames (temperate)
    @Published var seed: String = randomSeed()
    @Published var seedLocked = false

    // Layer stack (ordered; user can add/remove/reorder — web parity).
    @Published var stack: [TitanLayer] = []

    // Actual post-stack height range. Normalizing exports (.r16/.png16)
    // stretch to exactly this, so users need it to set a correct Z scale on
    // import — it is NOT [0, heightMultiplier] once erosion and deposition
    // have run, which is what the export docs used to claim.
    @Published var heightRange: (min: Float, max: Float) = (0, 0)
    /// The range the normalizing exporters stretch to. Differs from the
    /// terrain's own range when droplet erosion has left sediment towers — and
    /// it, not the terrain range, is what an importing tool's Z scale must be
    /// set from, because it is what the file encodes.
    @Published var exportRange: (min: Float, max: Float) = (0, 0)

    // Imported heightfield (normalized 0..1), additive base after generate.
    @Published var importedName: String? = nil
    /// Provenance for a real-world DEM: source dimensions and true elevation
    /// range, which is the reason to import one rather than a greyscale image.
    @Published var importNote: String? = nil
    private(set) var importedField: [Float] = []
    private(set) var importedSize: Int = 0

    // Render settings (viewer-side; mirror the web Render tab).
    @Published var seaLevel: Double = -10
    @Published var sunElevation: Double = 45
    @Published var sunAzimuth: Double = 45
    @Published var sunIntensity: Double = 1.4
    @Published var fogDensity: Double = 0.002
    @Published var wireframe = false
    @Published var showGrid = true
    @Published var inspectorOn = false
    @Published var carverOn = false
    @Published var carveRadius: Double = 4
    @Published var carveDepth: Double = 2
    @Published var probe: ProbeData? = nil
    /// Drop-a-volcano mode: press the terrain to place one, drag to position it.
    @Published var volcanoPlacementOn = false

    // Flora scattering (viewer-side, like the web lab).
    @Published var floraDensity: Double = 2000
    @Published var floraCount = 0

    // 2D top-down map.
    @Published var showTopDown = false
    @Published var topDownFullscreen = false
    @Published var topDownZoom: Double = 1.0
    @Published var topDownImage: CGImage? = nil

    @Published var isGenerating = false
    @Published var statusText = "Ready — flat canvas. Pick a preset or choose a noise structure."
    @Published var lastComputeMs: Int = 0

    private let engine: OpaquePointer?
    private let queue = DispatchQueue(label: "com.titanterrain.engine", qos: .userInitiated)
    private var runCounter = 0
    private var lastSnapshot: MeshSnapshot?
    private var lastFlora: [FloraInstance] = []
    // A volcano was dragged while a rebuild was already running; re-run once
    // that one lands. See moveVolcano.
    private var volcanoMovePending = false

    // Renderer hookups. Re-emit the latest data when a renderer (re)attaches.
    var onMesh: ((MeshSnapshot) -> Void)? {
        didSet { if let s = lastSnapshot { onMesh?(s) } }
    }
    var onFlora: (([FloraInstance], Int) -> Void)? {
        didSet { onFlora?(lastFlora, biome) }
    }

    init() {
        engine = titan_create()
    }

    deinit {
        titan_destroy(engine)
    }

    var engineVersion: String {
        String(cString: titan_version())
    }

    // MARK: - Stack editing

    func addLayer(_ kind: LayerKind) {
        stack.append(makeLayer(kind))
        rebuild()
    }

    func removeLayer(_ id: UUID) {
        stack.removeAll { $0.id == id }
        rebuild()
    }

    func moveLayer(_ id: UUID, by delta: Int) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        let j = i + delta
        guard j >= 0, j < stack.count else { return }
        stack.swapAt(i, j)
        rebuild()
    }

    func toggleLayer(_ id: UUID, enabled: Bool) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        stack[i].enabled = enabled
        rebuild()
    }

    func setLayerParam(_ id: UUID, _ key: String, _ value: Double) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        stack[i].params[key] = value
    }

    func setLayerCurve(_ id: UUID, _ points: [CurvePoint]) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        stack[i].curve = points
        rebuild()
    }

    func setLayerMask(_ id: UUID, _ transformMask: (inout LayerMask) -> Void) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        transformMask(&stack[i].mask)
    }

    // MARK: - Volcano placement

    /// Drops a volcano at a normalized grid position and returns its layer id
    /// so a drag can keep moving it.
    ///
    /// A volcano is inert without an eruption, so the first one placed also
    /// appends a Lava Flow layer. That layer goes last because it has to run
    /// after every volcano feeding it; placing a second cone reuses it, which
    /// is what makes several vents erupt together into one shared flow field
    /// rather than each getting a pass of its own.
    @discardableResult
    func placeVolcano(atX nx: Double, y ny: Double) -> UUID {
        var volcano = makeLayer(.volcano)
        volcano.params["x"] = min(1, max(0, nx))
        volcano.params["y"] = min(1, max(0, ny))
        volcano.params["variant"] = Double(stack.filter { $0.kind == .volcano }.count % 100)

        if let lavaIndex = stack.firstIndex(where: { $0.kind == .lava }) {
            stack.insert(volcano, at: lavaIndex)
        } else {
            stack.append(volcano)
            stack.append(makeLayer(.lava))
        }
        rebuild()
        return volcano.id
    }

    /// Repositions an already-placed volcano (used while dragging).
    ///
    /// Rebuilds are coalesced rather than queued. A drag emits a new position
    /// every few milliseconds and each rebuild runs the entire layer stack on
    /// a serial queue — including, here, a lava simulation. Enqueueing one per
    /// sample would leave the queue grinding through dozens of runs that were
    /// obsolete before they started, and the viewport lagging seconds behind
    /// the cursor. Instead the position is always current, and if a run is in
    /// flight the next one fires when it lands.
    func moveVolcano(_ id: UUID, toX nx: Double, y ny: Double) {
        guard let i = stack.firstIndex(where: { $0.id == id }) else { return }
        stack[i].params["x"] = min(1, max(0, nx))
        stack[i].params["y"] = min(1, max(0, ny))
        if isGenerating {
            volcanoMovePending = true
            return
        }
        rebuild()
    }

    func clearVolcanoes() {
        stack.removeAll { $0.kind == .volcano || $0.kind == .lava }
        rebuild()
    }

    var volcanoCount: Int {
        stack.filter { $0.kind == .volcano }.count
    }

    // MARK: - Presets (mirror src/core/pipeline.ts)

    static func presets() -> [Preset] {
        [
            Preset(name: "Alpine Peaks", tagline: "Ridged ranges, drainage, talus") { m in
                m.worldSize = 128; m.scale = 2.5; m.heightMultiplier = 70; m.octaves = 8; m.persistence = 0.5
                m.exponent = 1.1; m.warpStrength = 0.6; m.noiseType = 2; m.biome = 1
                m.stack = [
                    presetLayer(.fluvial, ["passes": 3, "strength": 1.2]),
                    presetLayer(.hydraulic, ["iterations": 65536, "spawnMode": 1]),
                    presetLayer(.thermal, ["passes": 12, "talusAngle": 35, "rate": 0.5]),
                ]
            },
            Preset(name: "Island Chain", tagline: "Soft archipelago rising from the sea") { m in
                m.worldSize = 128; m.scale = 1.8; m.heightMultiplier = 45; m.octaves = 6; m.persistence = 0.5
                m.exponent = 1.9; m.warpStrength = 0.9; m.noiseType = 1; m.biome = 1
                m.stack = [
                    presetLayer(.hydraulic, ["iterations": 49152, "spawnMode": 0]),
                    presetLayer(.thermal, ["passes": 8, "talusAngle": 33, "rate": 0.5]),
                ]
            },
            Preset(name: "Canyonlands", tagline: "Terraced mesas cut by deep river channels") { m in
                m.worldSize = 128; m.scale = 1.5; m.heightMultiplier = 60; m.octaves = 6; m.persistence = 0.5
                m.exponent = 1.4; m.warpStrength = 0.3; m.noiseType = 1; m.biome = 3
                m.stack = [
                    presetLayer(.terrace, ["interval": 12, "strength": 0.85, "sharpness": 3]),
                    presetLayer(.fluvial, ["passes": 4, "strength": 1.6]),
                    presetLayer(.thermal, ["passes": 6, "talusAngle": 38, "rate": 0.4]),
                ]
            },
            Preset(name: "Rolling Dunes", tagline: "Wind-settled billows of soft sand") { m in
                m.worldSize = 128; m.scale = 3.0; m.heightMultiplier = 25; m.octaves = 4; m.persistence = 0.45
                m.exponent = 1.0; m.warpStrength = 0.4; m.noiseType = 3; m.biome = 3
                m.stack = [
                    presetLayer(.thermal, ["passes": 20, "talusAngle": 30, "rate": 0.7]),
                ]
            },
            Preset(name: "Worley Plateaus", tagline: "Cellular mesas remapped by curves, cut by rivers") { m in
                m.worldSize = 128; m.scale = 1.6; m.heightMultiplier = 55; m.octaves = 5; m.persistence = 0.5
                m.exponent = 1.0; m.warpStrength = 0.3; m.noiseType = 4; m.biome = 3
                m.stack = [
                    // Was five fixed y-values on a quarter grid; same shape.
                    presetLayer(.curve, [:], curve: [
                        CurvePoint(x: 0, y: 0), CurvePoint(x: 0.25, y: 0.15),
                        CurvePoint(x: 0.5, y: 0.55), CurvePoint(x: 0.75, y: 0.85),
                        CurvePoint(x: 1, y: 1),
                    ]),
                    presetLayer(.noise, ["noiseType": 0, "blend": 0, "scale": 6, "amplitude": 6,
                                         "octaves": 6, "alpha": 0.5, "seedOffset": 3]),
                    presetLayer(.fluvial, ["passes": 2, "strength": 1.2]),
                    presetLayer(.blur, ["radius": 2, "strength": 0.8],
                                mask: LayerMask(mode: 2, lo: 0, hi: 0.15, invert: false)),
                    presetLayer(.thermal, ["passes": 8, "talusAngle": 36, "rate": 0.5]),
                ]
            },
            Preset(name: "Erupting Stratovolcano", tagline: "A breached cone pouring lava down to the sea") { m in
                m.worldSize = 128; m.scale = 2.2; m.heightMultiplier = 30; m.octaves = 6; m.persistence = 0.5
                m.exponent = 1.3; m.warpStrength = 0.5; m.noiseType = 1; m.biome = 2
                m.stack = [
                    // A radial ramp first: the island drains outward, which is
                    // what lets the flows run off the map instead of ponding at
                    // the cone's foot.
                    presetLayer(.gradient, ["kind": 1, "op": 0, "angle": 0, "height": 26]),
                    // Wide and comparatively low: a cone whose height rivals
                    // its radius reads as a spire, not a volcano.
                    presetLayer(.volcano, ["x": 0.5, "y": 0.5, "radius": 0.36, "height": 62]),
                    presetLayer(.thermal, ["passes": 6, "talusAngle": 36, "rate": 0.4]),
                    presetLayer(.lava, ["steps": 900, "eruptionRate": 2.0, "viscosity": 0.3]),
                ]
            },
            Preset(name: "Volcanic Twins", tagline: "Two vents whose flows collide and divert each other") { m in
                m.worldSize = 128; m.scale = 2.0; m.heightMultiplier = 26; m.octaves = 6; m.persistence = 0.5
                m.exponent = 1.2; m.warpStrength = 0.6; m.noiseType = 1; m.biome = 2
                m.stack = [
                    presetLayer(.gradient, ["kind": 1, "op": 0, "angle": 0, "height": 28]),
                    presetLayer(.volcano, ["x": 0.33, "y": 0.42, "radius": 0.26,
                                           "height": 54, "variant": 1, "breachAngle": 25]),
                    presetLayer(.volcano, ["x": 0.67, "y": 0.60, "radius": 0.22,
                                           "height": 44, "variant": 2, "breachAngle": 205]),
                    presetLayer(.lava, ["steps": 800, "eruptionRate": 1.6, "viscosity": 0.35]),
                ]
            },
            Preset(name: "Volcanic Shield", tagline: "A flattened caldera dome with radial gullies") { m in
                m.worldSize = 128; m.scale = 1.2; m.heightMultiplier = 90; m.octaves = 7; m.persistence = 0.5
                m.exponent = 1.6; m.warpStrength = 0.5; m.noiseType = 2; m.biome = 2
                m.stack = [
                    presetLayer(.plateau, ["height": 70, "softness": 12]),
                    presetLayer(.fluvial, ["passes": 2, "strength": 0.8]),
                    presetLayer(.hydraulic, ["iterations": 32768, "spawnMode": 1]),
                    presetLayer(.thermal, ["passes": 10, "talusAngle": 33, "rate": 0.5]),
                ]
            },
        ]
    }

    func applyPreset(_ preset: Preset) {
        preset.apply(self)
        if !seedLocked { seed = randomSeed() }
        rebuild()
    }

    func regenerateWithFreshSeed() {
        if !seedLocked { seed = randomSeed() }
        rebuild()
    }

    // MARK: - Pipeline execution (mirrors runPipeline in pipeline.ts)

    func rebuild() {
        runCounter += 1
        let run = runCounter
        isGenerating = true
        statusText = "Generating…"
        probe = nil

        let p = (size: Int32(size), scale: Float(scale), height: Float(heightMultiplier),
                 seed: hashSeed(seed), octaves: Int32(octaves), persistence: Float(persistence),
                 lacunarity: Float(lacunarity), exponent: Float(exponent),
                 noise: Int32(noiseType), warp: Float(warpStrength),
                 // Sample spacing: `size` is detail density, `worldSize` the world.
                 cell: Float(worldSize / max(1, size)))
        let imported = importedField.isEmpty ? nil : (data: importedField, size: Int32(importedSize))
        let layers = stack.filter { $0.enabled }
        let wantTopDown = showTopDown || topDownFullscreen
        let water = Float(seaLevel)
        let engine = self.engine

        queue.async { [weak self] in
            let started = Date()

            titan_configure(engine, p.size, p.cell, p.scale, p.height, p.seed, p.octaves,
                            p.persistence, p.lacunarity, p.exponent, p.noise, p.warp,
                            1.0, 2.0, 0.0, 0.0)
            titan_generate(engine)

            let stage: (String) -> Void = { label in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.runCounter == run else { return }
                    self.statusText = label
                }
            }

            // Imported heightmap: additive base, scaled by the height slider.
            if let imported {
                stage("Applying imported heightmap…")
                imported.data.withUnsafeBufferPointer { buf in
                    titan_apply_heightfield(engine, buf.baseAddress, imported.size,
                                            p.height, 0, 1.0)
                }
            }

            for layer in layers {
                let def = layerDefs[layer.kind]!
                stage("\(def.label)…")
                let masked = Self.activateMask(engine, layer.mask, size: p.size)
                Self.execute(layer, on: engine, gridSize: p.size,
                             heightMultiplier: p.height, imported: imported)
                if masked { titan_set_mask(engine, nil, 0) }
            }

            // Trace ambient occlusion once the stack has settled, so the mesh
            // snapshot below carries it. Deliberately after the layer loop: AO
            // is the engine's most expensive derived map and re-tracing it per
            // layer would cost more than every simulation pass combined.
            stage("Ambient occlusion…")
            titan_compute_ao(engine)

            var rangeLo: Float = 0
            var rangeHi: Float = 0
            titan_height_range(engine, &rangeLo, &rangeHi)
            var exportLo: Float = 0
            var exportHi: Float = 0
            titan_export_height_range(engine, &exportLo, &exportHi)

            let snapshot = Self.snapshotMesh(engine, heightMax: p.height,
                                             extent: Float(p.size) * p.cell)
            let topDown = wantTopDown
                ? Self.makeTopDownImage(engine, size: Int(p.size), seaLevel: water) : nil
            let elapsed = Int(Date().timeIntervalSince(started) * 1000)

            DispatchQueue.main.async { [weak self] in
                guard let self, self.runCounter == run else { return }
                self.isGenerating = false
                self.heightRange = (rangeLo, rangeHi)
                self.exportRange = (exportLo, exportHi)
                self.lastComputeMs = elapsed
                self.statusText = "\(self.engineVersion) — \(elapsed) ms"
                self.topDownImage = topDown
                self.lastFlora = []
                self.floraCount = 0
                self.onFlora?([], self.biome)
                if let snapshot {
                    self.lastSnapshot = snapshot
                    self.onMesh?(snapshot)
                }
                // Catch up with a drag that moved on while this run was busy.
                if self.volcanoMovePending {
                    self.volcanoMovePending = false
                    self.rebuild()
                }
            }
        }
    }

    // Activates a layer's mask on the engine (returns true if one was set).
    nonisolated private static func activateMask(_ engine: OpaquePointer?,
                                                 _ mask: LayerMask, size: Int32) -> Bool {
        guard mask.mode > 0 else { return false }
        if mask.mode == 4 {
            // Noise mask: rasterize a fractal field, then band it *in the
            // engine*. This used to band host-side, duplicating
            // MaskByFeature's curve in Swift (and again in TypeScript) —
            // three copies of one formula that had to agree forever.
            titan_noise_to_mask(engine, 9001, 1, 3.0, 5, 0.5, 2.0, 0.0)
            titan_band_scratch(engine, Float(mask.lo), Float(mask.hi), 0.05,
                               mask.invert ? 1 : 0)
            titan_set_mask_from_scratch(engine)
        } else {
            titan_mask_by_feature(engine, Int32(mask.mode - 1), Float(mask.lo),
                                  Float(mask.hi), 0.05, mask.invert ? 1 : 0)
            titan_set_mask_from_scratch(engine)
        }
        return true
    }


    // One layer's engine calls — a direct port of the switch in runPipeline.
    nonisolated private static func execute(_ layer: TitanLayer, on engine: OpaquePointer?,
                                            gridSize: Int32, heightMultiplier: Float,
                                            imported: (data: [Float], size: Int32)?) {
        let p = layer.params
        func f(_ key: String) -> Float { Float(p[key] ?? 0) }
        func i32(_ key: String) -> Int32 { Int32(p[key] ?? 0) }

        switch layer.kind {
        case .fluvial:
            titan_erode_fluvial_ex(engine, i32("passes"), f("strength"),
                                   f("erodeConstant"), f("areaExponent"),
                                   f("slopeExponent"), f("depositRatio"), f("maxStep"))
        case .hydraulic:
            titan_erode_hydraulic_ex(engine, i32("iterations"), i32("spawnMode"),
                                     f("inertia"), f("capacity"), 0.01,
                                     f("dissolve"), f("deposit"), f("evaporate"),
                                     f("gravity"), i32("lifetime"), f("radius"),
                                     f("bedrockSpeed"))
        case .thermal:
            titan_erode_thermal_ex(engine, i32("passes"), f("talusAngle"), f("rate"),
                                   f("bedrockBreakdown"))
        case .terrace:
            titan_apply_terrace(engine, f("interval"), f("strength"), f("sharpness"))
        case .plateau:
            titan_apply_plateau(engine, f("height"), f("softness"))
        case .noise:
            let typeIndex = Int(p["noiseType"] ?? 0)
            titan_apply_noise(engine, UInt32(p["seedOffset"] ?? 1),
                              layerNoiseIDs[min(max(typeIndex, 0), layerNoiseIDs.count - 1)],
                              f("scale"), f("amplitude"), i32("octaves"),
                              0.5, 2.0, 1.0, 0.0, i32("blend"), f("alpha"))
        case .gradient:
            let radial = i32("kind") == 1
            let half = Float(gridSize) * 0.5
            // Linear u must reach ±1 at the corners under any rotation.
            let extent = radial ? half : Float(gridSize) * 0.71
            let ops: [Int32] = [0, 1, 3]
            titan_apply_stamp(engine, radial ? 5 : 4, half, half, extent, extent,
                              f("angle"), f("height"), 0.5,
                              ops[min(max(Int(p["op"] ?? 0), 0), 2)])
        case .clamp:
            titan_apply_clamp(engine, f("min"), f("max"))
        case .curve:
            let pts = (layer.curve?.count ?? 0) >= 2 ? layer.curve! : defaultCurve
            let xs = pts.map { Float($0.x) }
            let ys = pts.map { Float($0.y) }
            xs.withUnsafeBufferPointer { xb in
                ys.withUnsafeBufferPointer { yb in
                    titan_apply_curve(engine, xb.baseAddress, yb.baseAddress, Int32(pts.count))
                }
            }
        case .blur:
            titan_apply_blur(engine, f("radius"), f("strength"))
        case .sharpen:
            titan_apply_sharpen(engine, f("radius"), f("strength"))
        case .transform:
            titan_apply_transform(engine, f("scale"), f("offset"), i32("invert"))
        case .snow:
            titan_apply_snow(engine, f("snowLine"), f("amount"), f("maxSlopeDeg"),
                             i32("settlePasses"), f("melt"))
        case .water:
            titan_compute_water(engine)
        case .volcano:
            // Stored normalized so the cone stays put when resolution changes.
            titan_apply_volcano(engine,
                                f("x") * Float(gridSize), f("y") * Float(gridSize),
                                max(2, f("radius") * Float(gridSize)), f("height"),
                                f("coneExponent"), f("craterRadius"), f("craterDepth"),
                                f("rimJaggedness"), f("roughness"),
                                f("breachAngle"), f("breachWidth"),
                                UInt32(max(0, i32("variant"))))
        case .lava:
            titan_simulate_lava(engine, i32("steps"), f("eruptionRate"),
                                f("viscosity"), f("solidifyRate"), f("coolRate"),
                                f("ventRadius"), i32("sustain"))
        case .combine:
            if let imported {
                imported.data.withUnsafeBufferPointer { buf in
                    titan_apply_heightfield(engine, buf.baseAddress, imported.size,
                                            f("strength") * heightMultiplier,
                                            i32("blend"), f("alpha"))
                }
            }
        }
    }

    /// Mesh snapshot for the headless smoke test, which has no model instance.
    /// Exists so the test asserts against the *same* interleaver the renderer
    /// uses rather than a second copy that could drift out of step with the
    /// shader's vertex layout.
    nonisolated static func smokeSnapshot(_ engine: OpaquePointer?) -> MeshSnapshot? {
        snapshotMesh(engine, heightMax: 1, extent: 128)
    }

    nonisolated private static func snapshotMesh(_ engine: OpaquePointer?, heightMax: Float,
                                                 extent: Float) -> MeshSnapshot? {
        let grid = Int(titan_size(engine))
        titan_build_mesh_lod(engine, EngineModel.previewMaxEdgeVertices)
        let vertexCount = Int(titan_mesh_vertex_count(engine))
        let indexCount = Int(titan_mesh_index_count(engine))
        guard vertexCount > 0, indexCount > 0,
              let positions = titan_mesh_positions_ptr(engine),
              let normals = titan_mesh_normals_ptr(engine),
              let colors = titan_mesh_colors_ptr(engine),
              let indexPtr = titan_mesh_indices_ptr(engine) else { return nil }

        let lava = titan_mesh_lava_ptr(engine)
        let surface = titan_mesh_surface_ptr(engine)
        var interleaved = [Float](repeating: 0, count: vertexCount * 18)
        for v in 0..<vertexCount {
            let o = v * 18
            interleaved[o + 0] = positions[v * 3 + 0]
            interleaved[o + 1] = positions[v * 3 + 1]
            interleaved[o + 2] = positions[v * 3 + 2]
            interleaved[o + 3] = normals[v * 3 + 0]
            interleaved[o + 4] = normals[v * 3 + 1]
            interleaved[o + 5] = normals[v * 3 + 2]
            interleaved[o + 6] = colors[v * 4 + 0]
            interleaved[o + 7] = colors[v * 4 + 1]
            interleaved[o + 8] = colors[v * 4 + 2]
            interleaved[o + 9] = colors[v * 4 + 3]
            if let lava {
                interleaved[o + 10] = lava[v * 4 + 0]
                interleaved[o + 11] = lava[v * 4 + 1]
                interleaved[o + 12] = lava[v * 4 + 2]
                interleaved[o + 13] = lava[v * 4 + 3]
            }
            if let surface {
                interleaved[o + 14] = surface[v * 4 + 0]
                interleaved[o + 15] = surface[v * 4 + 1]
                interleaved[o + 16] = surface[v * 4 + 2]
                interleaved[o + 17] = surface[v * 4 + 3]
            } else {
                interleaved[o + 14] = 1.0 // unoccluded
            }
        }
        let indices = Array(UnsafeBufferPointer(start: indexPtr, count: indexCount))
        return MeshSnapshot(interleaved: interleaved, indices: indices,
                            vertexCount: vertexCount, heightMax: heightMax,
                            terrainExtent: extent, gridSize: grid)
    }

    // MARK: - Inspector probe & manual carver

    func updateProbe(gridX: Int, gridY: Int) {
        guard !isGenerating else { return }
        probe = ProbeData(x: gridX, y: gridY,
                          height: titan_height_at(engine, Int32(gridX), Int32(gridY)),
                          sediment: titan_sediment_at(engine, Int32(gridX), Int32(gridY)),
                          flow: titan_flow_at(engine, Int32(gridX), Int32(gridY)),
                          slope: titan_slope_at(engine, Int32(gridX), Int32(gridY)))
    }

    func clearProbe() {
        probe = nil
    }

    func probeHeight(gridX: Int, gridY: Int) -> Float {
        titan_height_at(engine, Int32(gridX), Int32(gridY))
    }

    // Carve without re-running the pipeline: engine edit + mesh-only refresh,
    // serialized on the engine queue.
    func carve(gridX: Int, gridY: Int) {
        guard !isGenerating else { return }
        let engine = self.engine
        let radius = Float(carveRadius)
        let depth = Float(carveDepth)
        let heightMax = Float(heightMultiplier)
        let extent = Float(worldSize)
        queue.async { [weak self] in
            titan_carve(engine, Float(gridX), Float(gridY), radius, depth)
            let snapshot = Self.snapshotMesh(engine, heightMax: heightMax, extent: extent)
            DispatchQueue.main.async { [weak self] in
                guard let self, let snapshot else { return }
                self.lastSnapshot = snapshot
                self.onMesh?(snapshot)
            }
        }
    }

    // MARK: - Flora scattering (mirrors scatterFlora in the web lab)

    func scatterFlora() {
        guard !isGenerating,
              let bedrock = titan_bedrock_ptr(engine),
              let sediment = titan_sediment_ptr(engine),
              let flow = titan_flow_ptr(engine) else { return }

        let size = Int(self.size)
        // Flora is placed by cell index but drawn in world space.
        let cell = Float(worldSize / max(1, self.size))
        let heightRef = Float(heightMultiplier)
        let water = Float(seaLevel)
        var instances: [FloraInstance] = []
        instances.reserveCapacity(Int(floraDensity) / 4)

        for i in 0..<Int(floraDensity) {
            let x = Int.random(in: 0..<size)
            let y = Int.random(in: 0..<size)
            let idx = y * size + x
            let s = sediment[idx]
            let h = bedrock[idx] + s
            let f = flow[idx]
            let slope = titan_slope_at(engine, Int32(x), Int32(y))
            let heightNorm = heightRef > 0 ? h / heightRef : 0

            switch biome {
            case 0: // arctic — only low ground, extra sparse
                if heightNorm > 0.4 || h < water + 1 { continue }
                if Float.random(in: 0...1) > 0.1 { continue }
            case 2: // volcanic
                if slope > 0.3 || heightNorm > 0.8 { continue }
            case 3: // desert — only near water/flow
                if f < 0.3 || h < water + 1 { continue }
            default: // temperate
                if slope > 0.5 || heightNorm > 0.75 || h < water + 1 { continue }
            }

            // Prefer valleys with sediment and flow.
            let probability = (s / 5.0) * 0.5 + f * 0.5
            if Float.random(in: 0...1) > probability && i > 500 { continue }

            let base = Float.random(in: 0.5...1.0)
            var scale = SIMD4<Float>(base, base, base, 0)
            if biome == 3 { // desert: tall thin cactus-like
                scale = SIMD4<Float>(0.2, Float.random(in: 0.8...2.3), 0.2, 0)
            }
            instances.append(FloraInstance(
                posRot: SIMD4<Float>(Float(x - size / 2) * cell, h,
                                     Float(y - size / 2) * cell,
                                     Float.random(in: 0...(2 * .pi))),
                scale: scale))
        }

        lastFlora = instances
        floraCount = instances.count
        statusText = "Scattered \(instances.count) flora instances"
        onFlora?(instances, biome)
    }

    func clearFlora() {
        lastFlora = []
        floraCount = 0
        onFlora?([], biome)
    }

    // MARK: - 2D top-down map

    func refreshTopDown() {
        guard showTopDown || topDownFullscreen else {
            topDownImage = nil
            return
        }
        let engine = self.engine
        let size = Int(self.size)
        let water = Float(seaLevel)
        queue.async { [weak self] in
            let image = Self.makeTopDownImage(engine, size: size, seaLevel: water)
            DispatchQueue.main.async { self?.topDownImage = image }
        }
    }

    // Hypsometric tint + NW hillshade + sea-level water, straight from the
    // height buffers (same recipe as the web minimap). North (row 0) is the
    // -Z edge of the 3D scene.
    nonisolated private static func makeTopDownImage(_ engine: OpaquePointer?,
                                                     size: Int, seaLevel: Float) -> CGImage? {
        guard size > 1,
              let bedrock = titan_bedrock_ptr(engine),
              let sediment = titan_sediment_ptr(engine) else { return nil }
        let count = size * size

        var h = [Float](repeating: 0, count: count)
        var minH = Float.greatestFiniteMagnitude
        var maxH = -Float.greatestFiniteMagnitude
        for i in 0..<count {
            h[i] = bedrock[i] + sediment[i]
            minH = min(minH, h[i])
            maxH = max(maxH, h[i])
        }
        let range = maxH - minH > 0 ? maxH - minH : 1

        let stops: [(Float, Float, Float)] = [
            (46, 102, 60), (110, 139, 61), (190, 171, 110),
            (139, 105, 74), (120, 115, 110), (245, 248, 250),
        ]
        func ramp(_ t: Float) -> (Float, Float, Float) {
            let f = min(0.9999, max(0, t)) * Float(stops.count - 1)
            let i = Int(f)
            let u = f - Float(i)
            return (stops[i].0 + (stops[i + 1].0 - stops[i].0) * u,
                    stops[i].1 + (stops[i + 1].1 - stops[i].1) * u,
                    stops[i].2 + (stops[i + 1].2 - stops[i].2) * u)
        }

        var pixels = [UInt8](repeating: 255, count: count * 4)
        for y in 0..<size {
            for x in 0..<size {
                let i = y * size + x
                var r: Float, g: Float, b: Float
                if h[i] < seaLevel {
                    let depth = min(1, (seaLevel - h[i]) / 20)
                    r = 30 - 15 * depth; g = 90 - 45 * depth; b = 160 - 60 * depth
                } else {
                    let xm = x > 0 ? h[i - 1] : h[i]
                    let xp = x < size - 1 ? h[i + 1] : h[i]
                    let ym = y > 0 ? h[i - size] : h[i]
                    let yp = y < size - 1 ? h[i + size] : h[i]
                    let shade = 0.7 + min(0.6, max(-0.6, ((xm - xp) + (ym - yp)) * 0.12))
                    let c = ramp((h[i] - minH) / range)
                    r = c.0 * shade; g = c.1 * shade; b = c.2 * shade
                }
                pixels[i * 4 + 0] = UInt8(min(255, max(0, r)))
                pixels[i * 4 + 1] = UInt8(min(255, max(0, g)))
                pixels[i * 4 + 2] = UInt8(min(255, max(0, b)))
            }
        }

        let data = Data(pixels)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: size, height: size, bitsPerComponent: 8,
                       bitsPerPixel: 32, bytesPerRow: size * 4,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }

    // MARK: - Heightmap import

    func importHeightmap(url: URL) {
        let ext = url.pathExtension.lowercased()
        var decoded: (size: Int, data: [Float])? = nil

        if ext == "tif" || ext == "tiff" || ext == "hgt" || ext == "dem" {
            // Real-world elevation. Decoded by the engine so the web lab,
            // TitanLab and Unreal all read the same file identically.
            if let raw = try? Data(contentsOf: url) {
                titan_clear_error()
                let side: Int32 = raw.withUnsafeBytes { (b: UnsafeRawBufferPointer) in
                    titan_decode_dem(engine, b.bindMemory(to: UInt8.self).baseAddress,
                                     Int32(raw.count))
                }
                if side > 0, let ptr = titan_dem_ptr(engine) {
                    let n = Int(side) * Int(side)
                    decoded = (Int(side), Array(UnsafeBufferPointer(start: ptr, count: n)))
                    var lo: Float = 0, hi: Float = 0
                    titan_dem_elevation_range(engine, &lo, &hi)
                    let sw = Int(titan_dem_source_width(engine))
                    let sh = Int(titan_dem_source_height(engine))
                    importNote = String(format: "%d×%d · elevation %.0f → %.0f%@",
                                        sw, sh, lo, hi,
                                        sw == sh ? "" : " · stretched to square")
                } else if let err = titan_last_error() {
                    statusText = "Import failed — \(String(cString: err))"
                    return
                }
            }
        } else if ext == "r16" || ext == "raw" {
            if let raw = try? Data(contentsOf: url) {
                let count = raw.count / 2
                let side = Int(Double(count).squareRoot())
                if side >= 2 && side * side == count {
                    var data = [Float](repeating: 0, count: count)
                    raw.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
                        let u16 = bytes.bindMemory(to: UInt16.self)
                        for i in 0..<count { data[i] = Float(UInt16(littleEndian: u16[i])) / 65535.0 }
                    }
                    decoded = (side, data)
                }
            }
        } else if ext == "r32" {
            if let raw = try? Data(contentsOf: url) {
                let count = raw.count / 4
                let side = Int(Double(count).squareRoot())
                if side >= 2 && side * side == count {
                    var data = [Float](repeating: 0, count: count)
                    raw.withUnsafeBytes { (bytes: UnsafeRawBufferPointer) in
                        let f32 = bytes.bindMemory(to: Float.self)
                        var peak: Float = 0
                        for i in 0..<count { peak = max(peak, f32[i]) }
                        for i in 0..<count { data[i] = peak > 0 ? max(0, f32[i]) / peak : 0 }
                    }
                    decoded = (side, data)
                }
            }
        } else {
            decoded = Self.decodeImage(url: url)
        }

        guard let decoded else {
            statusText = "Import failed — need a .png/.r16/.r32 heightmap, GeoTIFF or .hgt"
            return
        }
        importedField = decoded.data
        importedSize = decoded.size
        importedName = url.lastPathComponent
        noiseType = 0 // flat base: the import IS the terrain
        rebuild()
    }

    func clearImport() {
        importedField = []
        importedSize = 0
        importedName = nil
        importNote = nil
        rebuild()
    }

    private static func decodeImage(url: URL) -> (size: Int, data: [Float])? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        let side = min(1024, max(image.width, image.height))
        guard side >= 2,
              let ctx = CGContext(data: nil, width: side, height: side,
                                  bitsPerComponent: 8, bytesPerRow: side,
                                  space: CGColorSpaceCreateDeviceGray(),
                                  bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return nil }
        ctx.interpolationQuality = .high
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: side, height: side))
        guard let bytes = ctx.data else { return nil }
        let buf = bytes.bindMemory(to: UInt8.self, capacity: side * side)
        var data = [Float](repeating: 0, count: side * side)
        // Bitmap memory row 0 is the image's top row — same orientation as
        // the web importer's canvas read, so no flip.
        for i in 0..<(side * side) {
            data[i] = Float(buf[i]) / 255.0
        }
        return (side, data)
    }

    // MARK: - Project save/load (.titan, web-compatible JSON)

    func serializeProject() -> Data? {
        var params: [String: Any] = [
            "size": Int(size), "worldSize": Int(worldSize),
            "scale": scale, "heightMultiplier": heightMultiplier,
            "seed": seed, "octaves": Int(octaves), "persistence": persistence,
            "lacunarity": lacunarity, "exponent": exponent, "warpStrength": warpStrength,
            "noiseType": noiseTypeNames[min(max(noiseType, 0), noiseTypeNames.count - 1)],
            "biome": biomeNames[min(max(biome, 0), biomeNames.count - 1)],
        ]
        params["size"] = Int(size)

        let stackJSON: [[String: Any]] = stack.map { layer in
            var entry: [String: Any] = [
                "id": layer.id.uuidString,
                "type": layer.kind.rawValue,
                "enabled": layer.enabled,
                "params": layer.params,
                "mask": ["mode": layer.mask.mode, "lo": layer.mask.lo,
                         "hi": layer.mask.hi, "invert": layer.mask.invert],
            ]
            if let curve = layer.curve, !curve.isEmpty {
                entry["curve"] = curve.map { ["x": $0.x, "y": $0.y] }
            }
            return entry
        }

        // Minimum version a reader needs: a project using no custom curve is
        // still a valid v2 and still opens in a build that predates them.
        let usesCustomCurve = stack.contains { $0.kind == .curve && !($0.curve ?? []).isEmpty }
        let fileVersion = usesCustomCurve ? titanProjectVersion : 2

        var dict: [String: Any] = ["version": fileVersion,
                                   "params": params, "stack": stackJSON]
        if !importedField.isEmpty {
            let bytes = importedField.withUnsafeBufferPointer { Data(buffer: $0) }
            var imp: [String: Any] = ["size": importedSize, "dataB64": bytes.base64EncodedString()]
            if let importedName { imp["name"] = importedName }
            dict["imported"] = imp
        }
        return try? JSONSerialization.data(withJSONObject: dict, options: [.prettyPrinted, .sortedKeys])
    }

    func loadProject(from data: Data) -> Bool {
        guard let raw = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let fileVersion = raw["version"] as? Int,
              fileVersion >= 1, fileVersion <= titanProjectVersion,
              let p = raw["params"] as? [String: Any],
              let rawStack = raw["stack"] as? [[String: Any]] else { return false }

        func num(_ key: String, _ fallback: Double) -> Double {
            (p[key] as? NSNumber)?.doubleValue ?? fallback
        }
        size = min(2048, max(64, num("size", 128)))
        // A v1 project predates the world/detail split: its extent *was* its
        // sample count, so worldSize = size reproduces it exactly.
        if fileVersion >= 2, let w = (p["worldSize"] as? NSNumber)?.doubleValue, w > 0 {
            worldSize = min(8192, max(64, w))
        } else {
            worldSize = size
        }
        scale = num("scale", 2.0)
        heightMultiplier = num("heightMultiplier", 40)
        octaves = num("octaves", 6)
        persistence = num("persistence", 0.5)
        lacunarity = num("lacunarity", 2.0)
        exponent = num("exponent", 1.2)
        warpStrength = num("warpStrength", 0.5)
        if let s = p["seed"] as? String { seed = s }
        noiseType = noiseTypeNames.firstIndex(of: (p["noiseType"] as? String) ?? "none") ?? 0
        biome = biomeNames.firstIndex(of: (p["biome"] as? String) ?? "temperate") ?? 1

        var loaded: [TitanLayer] = []
        for entry in rawStack {
            guard let typeName = entry["type"] as? String,
                  let kind = LayerKind(rawValue: typeName) else { continue }
            var layer = makeLayer(kind)
            layer.enabled = (entry["enabled"] as? Bool) ?? true
            if let params = entry["params"] as? [String: Any] {
                for def in layerDefs[kind]!.params {
                    if let v = (params[def.key] as? NSNumber)?.doubleValue {
                        layer.params[def.key] = min(def.range.upperBound,
                                                    max(def.range.lowerBound, v))
                    }
                }
            }
            // Curve control points. A v2 file predates them, so its five
            // fixed y-values are lifted onto the x positions they always used,
            // reproducing exactly the curve the file described.
            if kind == .curve {
                var pts: [CurvePoint] = []
                if let raw = entry["curve"] as? [[String: Any]] {
                    for p in raw {
                        if let x = (p["x"] as? NSNumber)?.doubleValue,
                           let y = (p["y"] as? NSNumber)?.doubleValue {
                            pts.append(CurvePoint(x: x, y: y))
                        }
                    }
                }
                if let cleaned = sanitizeCurve(pts) {
                    layer.curve = cleaned
                } else if let params = entry["params"] as? [String: Any] {
                    let keys = ["y0", "y1", "y2", "y3", "y4"]
                    let ys = keys.compactMap { (params[$0] as? NSNumber)?.doubleValue }
                    layer.curve = ys.count == 5
                        ? zip([0, 0.25, 0.5, 0.75, 1], ys).map { CurvePoint(x: $0, y: min(1, max(0, $1))) }
                        : defaultCurve
                } else {
                    layer.curve = defaultCurve
                }
            }
            if let mask = entry["mask"] as? [String: Any] {
                layer.mask.mode = min(4, max(0, (mask["mode"] as? NSNumber)?.intValue ?? 0))
                layer.mask.lo = min(1, max(0, (mask["lo"] as? NSNumber)?.doubleValue ?? 0))
                layer.mask.hi = min(1, max(0, (mask["hi"] as? NSNumber)?.doubleValue ?? 1))
                layer.mask.invert = (mask["invert"] as? Bool) ?? false
            }
            loaded.append(layer)
        }
        stack = loaded

        importedField = []
        importedSize = 0
        importedName = nil
        if let imp = raw["imported"] as? [String: Any],
           let side = (imp["size"] as? NSNumber)?.intValue,
           let b64 = imp["dataB64"] as? String,
           let bytes = Data(base64Encoded: b64), side >= 2 {
            let floats: [Float] = bytes.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
            if floats.count == side * side {
                importedField = floats
                importedSize = side
                importedName = imp["name"] as? String ?? "imported heightmap"
            }
        }

        rebuild()
        return true
    }

    // MARK: - Export

    // MARK: - Tiled export

    /// Samples per edge a tile would have, or 0 if the split is invalid.
    func tileResolution(tilesPerSide: Int, overlap: Int) -> Int {
        Int(titan_tile_resolution(engine, Int32(tilesPerSide), Int32(overlap)))
    }

    /// One tile of the already-simulated terrain.
    ///
    /// Tiles are sliced rather than regenerated at their own world origins.
    /// World-space noise sampling does make independently generated tiles line
    /// up exactly on raw terrain, but erosion is not local — droplets do not
    /// cross a tile boundary — so separately eroded tiles seam by several
    /// percent of the relief. Every tile also normalizes against the whole
    /// terrain's range, so the set assembles without steps.
    ///
    /// format: 0 = .r16, 1 = 16-bit PNG, 2 = .r32.
    func exportTileData(tileX: Int, tileY: Int, tilesPerSide: Int,
                        overlap: Int, format: Int) -> Data? {
        titan_clear_error()
        let bytes = titan_export_tile(engine, Int32(tileX), Int32(tileY),
                                      Int32(tilesPerSide), Int32(overlap), Int32(format))
        guard bytes > 0, let ptr = titan_export_data_ptr(engine) else {
            if let err = titan_last_error() {
                statusText = "Tiled export failed — \(String(cString: err))"
            }
            return nil
        }
        return Data(bytes: ptr, count: Int(bytes))
    }

    /// Writes every tile into `directory`, named for Unreal's tiled import.
    /// Returns the number of files written.
    @discardableResult
    func exportTiles(to directory: URL, tilesPerSide: Int, overlap: Int,
                     format: Int, ext: String) -> Int {
        guard tileResolution(tilesPerSide: tilesPerSide, overlap: overlap) > 0 else {
            statusText = "\(tilesPerSide)x\(tilesPerSide) tiles do not divide a \(Int(size)) grid evenly"
            return 0
        }
        var written = 0
        for ty in 0..<tilesPerSide {
            for tx in 0..<tilesPerSide {
                guard let data = exportTileData(tileX: tx, tileY: ty,
                                                tilesPerSide: tilesPerSide,
                                                overlap: overlap, format: format) else {
                    return written
                }
                // x0_y0 naming is what Unreal's tiled landscape import expects.
                let url = directory.appendingPathComponent(
                    "titan_\(seed)_x\(tx)_y\(ty).\(ext)")
                do { try data.write(to: url) } catch {
                    statusText = "Could not write \(url.lastPathComponent)"
                    return written
                }
                written += 1
            }
        }
        statusText = "Wrote \(written) tiles to \(directory.lastPathComponent)"
        return written
    }

    /// Runs a C++ exporter and copies the result out.
    ///
    /// Sizes are Int64: a full-resolution OBJ of a large grid exceeds 2 GB,
    /// which the previous Int32 return silently truncated. A zero size means
    /// the engine failed — `titan_last_error` carries the reason (out of
    /// memory on a large grid being the realistic one).
    func exportData(kind: String) -> Data? {
        titan_clear_error()
        let sizeBytes: Int64
        switch kind {
        case "r16": sizeBytes = titan_export_r16(engine)
        case "png16": sizeBytes = titan_export_png16(engine)
        case "r32": sizeBytes = titan_export_r32(engine)
        case "exr": sizeBytes = titan_export_exr(engine)
        case "obj": sizeBytes = titan_export_obj(engine)
        case "normal": sizeBytes = titan_export_normal_png(engine)
        case "ao": sizeBytes = titan_export_ao_png(engine)
        // Splatmap now comes from the engine, sharing its channel definition
        // with the mesh vertex colours so it matches the viewport.
        case "splat": sizeBytes = titan_export_splat_png(engine)
        default: return nil
        }
        guard sizeBytes > 0, let ptr = titan_export_data_ptr(engine) else {
            if let err = titan_last_error() {
                statusText = "Export failed — \(String(cString: err))"
            }
            return nil
        }
        return Data(bytes: ptr, count: Int(sizeBytes))
    }
}
