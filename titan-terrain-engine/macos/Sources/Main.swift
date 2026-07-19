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

    // 2. Exporters.
    let r16Size = titan_export_r16(engine)
    check(r16Size == 128 * 128 * 2, "r16 export size \(r16Size)")
    let pngSize = titan_export_png16(engine)
    check(pngSize > 8, "png16 export size \(pngSize)")
    if let data = titan_export_data_ptr(engine) {
        check(data[0] == 137 && data[1] == 80, "png16 signature")
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
