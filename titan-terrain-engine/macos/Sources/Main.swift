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
