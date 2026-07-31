// TitanLab entry point.
//
// `TitanLab --smoke-test` runs the full engine + Metal pipeline headlessly
// (no window) and exits nonzero on failure — this is what CI and the build
// script use to verify the app without a display.

import AppKit
import Metal
import SwiftUI

@main
enum Main {
    static func main() {
        if CommandLine.arguments.contains("--smoke-test") {
            exit(runSmokeTest())
        }
        TitanLabApp.main()
    }
}

struct TitanLabApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup("TitanLab") {
            ContentView()
                .frame(minWidth: 1100, minHeight: 700)
        }
        .windowStyle(.titleBar)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

// MARK: - Headless smoke test

func runSmokeTest() -> Int32 {
    var failures = 0
    func check(_ ok: Bool, _ label: String) {
        print("  \(ok ? "PASS" : "FAIL")  \(label)")
        if !ok { failures += 1 }
    }

    // Unbuffered: if a check crashes the process, the output up to that
    // point is what says where it died.
    setbuf(stdout, nil)

    print("=== TitanLab smoke test ===")

    // 1. Engine linkage + full pipeline.
    guard let engine = titan_create() else {
        print("  FAIL  titan_create returned null")
        return 1
    }
    defer { titan_destroy(engine) }

    check(titan_api_version() == 1, "C API version is 1")
    let version = String(cString: titan_version())
    check(version.contains("libTitanCore"), "engine version string: \(version)")

    titan_configure(engine, 128, 1.0, 2.5, 70.0, 4242, 8, 0.5, 2.0, 1.1, 2, 0.6, 1.0, 2.0, 0.0, 0.0)
    titan_generate(engine)
    titan_erode_fluvial(engine, 2, 1.2)
    titan_erode_hydraulic(engine, 32768, 1)
    titan_erode_thermal(engine, 8, 33.0, 0.5)
    titan_build_mesh(engine)

    let vertexCount = titan_mesh_vertex_count(engine)
    let indexCount = titan_mesh_index_count(engine)
    check(vertexCount == 128 * 128, "mesh vertex count \(vertexCount)")
    check(indexCount == 127 * 127 * 6, "mesh index count \(indexCount)")

    var finite = true
    if let positions = titan_mesh_positions_ptr(engine) {
        for i in 0..<(vertexCount * 3) where !positions[Int(i)].isFinite { finite = false; break }
    } else {
        finite = false
    }
    check(finite, "mesh positions finite")

    // 2. v0.5 API: filters, masks, combiner, derived maps.
    titan_mask_by_feature(engine, 1, 0.2, 1.0, 0.05, 0)
    titan_set_mask_from_scratch(engine)
    titan_apply_blur(engine, 2.0, 0.5)
    titan_apply_sharpen(engine, 2.0, 0.5)
    titan_apply_clamp(engine, 0.0, 65.0)
    let xs: [Float] = [0, 0.5, 1]
    let ys: [Float] = [0, 0.4, 1]
    xs.withUnsafeBufferPointer { xb in
        ys.withUnsafeBufferPointer { yb in
            titan_apply_curve(engine, xb.baseAddress, yb.baseAddress, 3)
        }
    }
    titan_set_mask(engine, nil, 0)
    let combined = [Float](repeating: 0.5, count: 64 * 64)
    combined.withUnsafeBufferPointer { buf in
        titan_apply_heightfield(engine, buf.baseAddress, 64, 10.0, 3, 1.0)
    }
    titan_compute_slope_map(engine)
    titan_compute_curvature_map(engine)
    titan_build_mesh(engine)
    var v05Finite = true
    if let positions = titan_mesh_positions_ptr(engine) {
        for i in 0..<(vertexCount * 3) where !positions[Int(i)].isFinite { v05Finite = false; break }
    } else {
        v05Finite = false
    }
    check(v05Finite, "v0.5 filter/mask/combiner chain stays finite")

    // 3. Exporters.
    let r16Size = titan_export_r16(engine)
    check(r16Size == 128 * 128 * 2, "r16 export size \(r16Size)")
    let pngSize = titan_export_png16(engine)
    check(pngSize > 8, "png16 export size \(pngSize)")
    if let data = titan_export_data_ptr(engine) {
        check(data[0] == 137 && data[1] == 80, "png16 signature")
    }
    let normalSize = titan_export_normal_png(engine)
    check(normalSize > 8, "normal map export size \(normalSize)")
    let aoSize = titan_export_ao_png(engine)
    check(aoSize > 8, "AO export size \(aoSize)")

    // 4. Volcanism: edifice, eruption, and the vertex layout the Metal shader
    //    reads. A stride mismatch between the Swift interleaver and the MSL
    //    VertexIn struct is invisible until the terrain renders as garbage, so
    //    it is worth asserting rather than eyeballing.
    titan_configure(engine, 128, 1.0, 2.2, 30.0, 4242, 6, 0.5, 2.0, 1.3, 1, 0.5, 1.0, 2.0, 0.0, 0.0)
    titan_generate(engine)
    check(titan_vent_count(engine) == 0, "no vents before a volcano is placed")
    check(titan_lava_ptr(engine) == nil, "lava buffers unallocated without volcanism")

    titan_apply_volcano(engine, 64, 64, 46, 62, 1.75, 0.16, 0.16, 0.6, 0.5, -1, 46, 0)
    check(titan_vent_count(engine) == 1, "volcano registers a vent")
    titan_simulate_lava(engine, 600, 2.0, 0.3, 0.02, 0.0015, 2.5, 1)

    var moltenCells = 0
    var chilledCells = 0
    if let lava = titan_lava_ptr(engine), let rock = titan_lava_rock_ptr(engine) {
        for i in 0..<(128 * 128) {
            if lava[i] > 1e-3 { moltenCells += 1 }
            if rock[i] > 1e-3 { chilledCells += 1 }
        }
    }
    check(moltenCells > 0, "eruption leaves molten lava (\(moltenCells) cells)")
    check(chilledCells > 0, "eruption leaves chilled basalt (\(chilledCells) cells)")

    // 5. Ambient occlusion, and the full vertex layout the Metal shader reads.
    titan_compute_ao(engine)
    var aoVaries = false
    if let ao = titan_ao_ptr(engine) {
        var lo: Float = 2, hi: Float = -1
        for i in 0..<(128 * 128) { lo = min(lo, ao[i]); hi = max(hi, ao[i]) }
        aoVaries = hi - lo > 0.1 && lo >= 0 && hi <= 1
    }
    check(aoVaries, "ambient occlusion field varies across the terrain")

    if let snapshot = EngineModel.smokeSnapshot(engine) {
        check(snapshot.interleaved.count == snapshot.vertexCount * 18,
              "interleaved vertex stride is 18 floats")
        // Floats 10..13 are the lava attribute, 14..17 the surface attribute.
        // At least one vertex must carry each or the shader has nothing to
        // draw with. A stride mismatch between this interleaver and the MSL
        // VertexIn struct is invisible until the terrain renders as garbage.
        var sawLava = false
        var sawAO = false
        for v in 0..<snapshot.vertexCount {
            if snapshot.interleaved[v * 18 + 10] > 1e-3 { sawLava = true }
            if snapshot.interleaved[v * 18 + 14] < 0.999 { sawAO = true }
            if sawLava && sawAO { break }
        }
        check(sawLava, "mesh carries the lava attribute to the shader")
        check(sawAO, "mesh carries the surface/AO attribute to the shader")
    } else {
        check(false, "mesh snapshot for volcanism")
    }

    // 6. Node graph: the evaluator, the fork it exists for, and the round trip
    //    back to the layer stack. None of this is reachable from the smoke
    //    test's headless path except by driving it directly, and all of it is
    //    the kind of thing that breaks silently — a graph that quietly
    //    evaluates the wrong branch still renders a plausible mountain.
    func evaluate(_ graph: TerrainGraph, thumbnails: Bool = false) -> GraphEvaluator.Result {
        GraphEvaluator.evaluate(plan: GraphPlan(graph), engine: engine, gridSize: 128,
                                cellSize: 1.0, seed: 4242, thumbnails: thumbnails,
                                progress: { _ in })
    }
    func currentField() -> [Float] {
        var f = [Float](repeating: 0, count: 128 * 128)
        f.withUnsafeMutableBufferPointer { titan_read_height(engine, $0.baseAddress, 128 * 128) }
        return f
    }

    let linear = TerrainGraph.starter()
    let linearResult = evaluate(linear, thumbnails: true)
    check(linearResult.error == nil, "starter graph evaluates (\(linearResult.error ?? "no error"))")
    check(linearResult.evaluated == 2, "starter graph runs both its nodes")
    check(linearResult.thumbnails.count == 2, "every evaluated node gets a thumbnail")
    let linearField = currentField()
    check(linearField.contains { $0.isFinite && $0 != 0 }, "graph leaves terrain in the engine")

    // A graph that forks and rejoins — the thing a layer stack cannot express.
    // Branch A blurs the base, branch B terraces it, Combine takes the max.
    let forked = TerrainGraph()
    let base = forked.add(.terrain, at: CGPoint(x: 0, y: 0))
    let blur = forked.add(.blur, at: CGPoint(x: 200, y: 0))
    let terrace = forked.add(.terrace, at: CGPoint(x: 200, y: 200))
    let join = forked.add(.combine, at: CGPoint(x: 400, y: 100))
    let out = forked.add(.output, at: CGPoint(x: 600, y: 100))
    forked.connect(from: base, to: blur, port: 0)
    forked.connect(from: base, to: terrace, port: 0)
    forked.connect(from: blur, to: join, port: 0)
    forked.connect(from: terrace, to: join, port: 1)
    forked.connect(from: join, to: out, port: 0)
    let forkResult = evaluate(forked)
    check(forkResult.error == nil, "a forked graph evaluates")
    // Five nodes, but the base feeds two branches and must run once.
    check(forkResult.evaluated == 4, "a shared upstream node is evaluated once, not twice")

    let joined = currentField()
    // Max of two branches is >= either, and must differ from a plain blur or
    // the combine did nothing.
    let blurOnly = TerrainGraph()
    let b2 = blurOnly.add(.terrain, at: .zero)
    let b3 = blurOnly.add(.blur, at: .zero)
    let b4 = blurOnly.add(.output, at: .zero)
    blurOnly.connect(from: b2, to: b3, port: 0)
    blurOnly.connect(from: b3, to: b4, port: 0)
    _ = evaluate(blurOnly)
    let blurField = currentField()
    var aboveBlur = true
    var differs = false
    for i in 0..<joined.count {
        if joined[i] < blurField[i] - 0.02 { aboveBlur = false }
        if abs(joined[i] - blurField[i]) > 0.02 { differs = true }
    }
    check(aboveBlur, "a max combine is never below the branch it merged")
    check(differs, "the second branch actually contributes")

    // The preview eye evaluates only what feeds the node being looked at.
    forked.previewNode = blur
    let previewResult = evaluate(forked)
    check(previewResult.evaluated == 2, "previewing a node evaluates only its inputs")
    forked.previewNode = nil

    // A loop must be refused at the point of connecting, not discovered later.
    check(forked.connect(from: join, to: blur, port: 0) == false,
          "a connection that would close a loop is refused")

    // Stack round trip: a line converts both ways, a fork does not pretend to.
    check(forked.asStack() == nil, "a forked graph reports that it is not a stack")
    let stackLayers: [TitanLayer] = [
        TitanLayer(kind: .hydraulic, params: ["iterations": 16384, "spawnMode": 0]),
        TitanLayer(kind: .thermal, params: ["passes": 4, "talusAngle": 33, "rate": 0.5]),
    ]
    let converted = TerrainGraph.from(stack: stackLayers, base: ["scale": 2.5, "height": 70])
    check(converted.nodes.count == 4, "a 2-layer stack becomes 4 nodes (terrain + 2 + output)")
    let back = converted.asStack()
    check(back?.map(\.kind) == [.hydraulic, .thermal], "and converts back in the same order")
    check(back?.first?.params["iterations"] == 16384, "carrying its parameters with it")

    // Deleting the Output node would leave the graph with nowhere to end.
    if let outID = converted.outputID {
        converted.remove(outID)
        check(converted.outputID != nil, "the Output node cannot be deleted")
    }

    // 7. Canvas geometry. Ports, wires and hit-testing are computed from node
    //    positions rather than measured from laid-out views, so that wires
    //    land exactly where the ports are drawn — which also makes the whole
    //    interaction model checkable without a display.
    let probe = GraphNode(kind: .combine, at: CGPoint(x: 100, y: 100))
    let card = NodeGeometry.rect(probe)
    let inA = NodeGeometry.inputPoint(probe, port: 0)
    let inB = NodeGeometry.inputPoint(probe, port: 1)
    let outP = NodeGeometry.outputPoint(probe)
    check(inA.x == card.minX && outP.x == card.maxX,
          "inputs sit on the left edge and the output on the right")
    check(inA.y > card.minY + NodeGeometry.headerHeight && inB.y < card.maxY,
          "both inputs sit inside the card body")
    check(inB.y - inA.y > 20, "two inputs are far enough apart to hit separately")
    let maskP = NodeGeometry.inputPoint(probe, port: maskPort)
    check(maskP.y > inB.y && maskP.y < card.maxY,
          "the mask port sits below the field inputs, inside the card")

    // A single-input node centres its one port rather than crowding the top.
    let single = GraphNode(kind: .blur, at: CGPoint(x: 0, y: 0))
    check(NodeGeometry.inputPoint(single, port: 0).y == NodeGeometry.outputPoint(single).y,
          "a one-input node lines its port up with its output")

    // Wire hit-testing: on the curve hits, well off it misses.
    let wireA = CGPoint(x: 0, y: 0)
    let wireB = CGPoint(x: 200, y: 0)
    check(NodeGeometry.distance(to: CGPoint(x: 100, y: 0), from: wireA, to: wireB) < 1,
          "a point on a wire measures as on it")
    check(NodeGeometry.distance(to: CGPoint(x: 100, y: 80), from: wireA, to: wireB) > 40,
          "a point well off a wire measures as off it")

    // Splicing: dropping an unconnected node on a wire rewires both ends.
    let spliceGraph = TerrainGraph.starter()
    let before = spliceGraph.edges.count
    let sharpen = spliceGraph.add(.sharpen, at: CGPoint(x: 400, y: 400))
    if let edge = spliceGraph.edges.first {
        let upstream = edge.from
        let downstream = edge.to
        spliceGraph.insert(sharpen, into: edge)
        check(spliceGraph.edges.count == before + 1, "splicing adds exactly one link")
        check(spliceGraph.source(of: sharpen, port: 0) == upstream
              && spliceGraph.source(of: downstream, port: 0) == sharpen,
              "the spliced node takes over both ends of the wire")
    }

    // An input holds one wire: connecting again replaces rather than stacks.
    let replaceGraph = TerrainGraph.starter()
    if let outID = replaceGraph.outputID {
        let extra = replaceGraph.add(.noise, at: .zero)
        let terrainID = replaceGraph.nodes.first { $0.kind == .terrain }!.id
        replaceGraph.connect(from: terrainID, to: extra, port: 0)
        replaceGraph.connect(from: extra, to: outID, port: 0)
        check(replaceGraph.edges.filter { $0.to == outID }.count == 1,
              "an input accepts one wire, replacing what was there")
    }

    // 8. .titan v4: a graph has to survive a save, and a file that does not
    //    need v4 must not claim it — an older reader that could reproduce the
    //    project exactly should still be allowed to open it.
    // EngineModel is main-actor isolated and main() runs this on the main
    // thread, so the isolation is real rather than assumed away.
    MainActor.assumeIsolated {
    let model = EngineModel()
    model.graph = forked
    model.graphMode = true
    if let saved = model.serializeProject(),
       let parsed = try? JSONSerialization.jsonObject(with: saved) as? [String: Any] {
        check((parsed["version"] as? Int) == 4, "a graph-driven project is stamped v4")
        let reloaded = EngineModel()
        check(reloaded.loadProject(from: saved), "a v4 project loads")
        check(reloaded.graph.nodes.count == forked.nodes.count,
              "every node comes back (\(reloaded.graph.nodes.count))")
        check(reloaded.graph.edges.count == forked.edges.count,
              "every wire comes back (\(reloaded.graph.edges.count))")
        check(reloaded.graphMode, "and the graph is still what drives the terrain")
        // The fork has to survive, not just the node count: Combine's second
        // input is the whole reason the file needs a graph at all.
        if let join = reloaded.graph.nodes.first(where: { $0.kind == .combine }) {
            check(reloaded.graph.source(of: join.id, port: 0) != nil
                  && reloaded.graph.source(of: join.id, port: 1) != nil,
                  "both branches are still wired into Combine")
        } else {
            check(false, "the Combine node survived the round trip")
        }
    } else {
        check(false, "graph project serializes")
    }

    // The same model with the graph parked rather than driving stays readable
    // by a build that predates graphs entirely.
    model.graphMode = false
    if let saved = model.serializeProject(),
       let parsed = try? JSONSerialization.jsonObject(with: saved) as? [String: Any] {
        check((parsed["version"] as? Int) ?? 9 < 4,
              "a graph that is not driving the terrain does not force v4")
        check(parsed["graph"] != nil, "but the graph is still saved with the file")
    }
    }

    // 3. Metal: device, runtime shader compile, pipeline state.
    guard let device = MTLCreateSystemDefaultDevice() else {
        print("  FAIL  no Metal device")
        return 1
    }
    check(true, "Metal device: \(device.name)")

    do {
        let library = try device.makeLibrary(source: titanShaderSource, options: nil)
        let vfn = library.makeFunction(name: "terrain_vertex")
        let ffn = library.makeFunction(name: "terrain_fragment")
        check(vfn != nil && ffn != nil, "shader functions present")

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vfn
        desc.fragmentFunction = ffn
        desc.colorAttachments[0].pixelFormat = .bgra8Unorm
        desc.depthAttachmentPixelFormat = .depth32Float
        _ = try device.makeRenderPipelineState(descriptor: desc)
        check(true, "render pipeline state compiles")
    } catch {
        check(false, "Metal shader/pipeline: \(error)")
    }

    print(failures == 0 ? "SMOKE TEST PASSED" : "SMOKE TEST FAILED (\(failures))")
    return failures == 0 ? 0 : 1
}
