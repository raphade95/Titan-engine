// EngineModel — the app's single source of truth. Owns the libTitanCore
// handle, runs the layer pipeline on a background queue, and publishes mesh
// snapshots for the renderer. All terrain math stays in C++.

import Foundation
import SwiftUI

// FNV-1a, identical to the web lab so seeds are portable across products.
func hashSeed(_ seed: String) -> UInt32 {
    var h: UInt32 = 0x811c9dc5
    for byte in seed.utf8 {
        h ^= UInt32(byte)
        h = h &* 0x01000193
    }
    return h
}

func randomSeed() -> String {
    let chars = "abcdefghijklmnopqrstuvwxyz0123456789"
    return String((0..<7).map { _ in chars.randomElement()! })
}

struct MeshSnapshot {
    var interleaved: [Float] // pos(3) + normal(3) + color(4), stride 10 floats
    var indices: [UInt32]
    var vertexCount: Int
    var heightMax: Float
    var terrainExtent: Float
}

struct Preset: Identifiable {
    let id = UUID()
    let name: String
    let tagline: String
    let apply: (EngineModel) -> Void
}

@MainActor
final class EngineModel: ObservableObject {
    // Base parameters (mirror the web lab defaults; flat start, fresh seed).
    @Published var size: Double = 128
    @Published var scale: Double = 2.0
    @Published var heightMultiplier: Double = 40
    @Published var octaves: Double = 6
    @Published var persistence: Double = 0.5
    @Published var lacunarity: Double = 2.0
    @Published var exponent: Double = 1.2
    @Published var warpStrength: Double = 0.5
    @Published var noiseType: Int = 0 // 0 flat, 1 simplex, 2 ridged, 3 billow
    @Published var seed: String = randomSeed()
    @Published var seedLocked = false

    // Erosion stack (fixed order: rivers -> droplets -> thermal).
    @Published var riversEnabled = false
    @Published var riverPasses: Double = 2
    @Published var riverStrength: Double = 1.0
    @Published var dropletsEnabled = false
    @Published var dropletRounds: Double = 3 // x16384
    @Published var dropletSpawnMode: Int = 0
    @Published var thermalEnabled = false
    @Published var thermalPasses: Double = 10
    @Published var thermalAngle: Double = 33

    @Published var isGenerating = false
    @Published var statusText = "Ready — flat canvas. Pick a preset or choose a noise structure."
    @Published var lastComputeMs: Int = 0

    private let engine: OpaquePointer?
    private let queue = DispatchQueue(label: "com.titanterrain.engine", qos: .userInitiated)
    private var runCounter = 0

    var onMesh: ((MeshSnapshot) -> Void)?

    init() {
        engine = titan_create()
    }

    deinit {
        titan_destroy(engine)
    }

    var engineVersion: String {
        String(cString: titan_version())
    }

    func regenerateWithFreshSeed() {
        if !seedLocked { seed = randomSeed() }
        rebuild()
    }

    // Runs the whole pipeline off the main thread; a newer run supersedes.
    func rebuild() {
        runCounter += 1
        let run = runCounter
        isGenerating = true
        statusText = "Generating…"

        let p = (size: Int32(size), scale: Float(scale), height: Float(heightMultiplier),
                 seed: hashSeed(seed), octaves: Int32(octaves), persistence: Float(persistence),
                 lacunarity: Float(lacunarity), exponent: Float(exponent),
                 noise: Int32(noiseType), warp: Float(warpStrength))
        let rivers = riversEnabled ? (passes: Int32(riverPasses), strength: Float(riverStrength)) : nil
        let droplets = dropletsEnabled ? (count: Int32(dropletRounds) * 16384, mode: Int32(dropletSpawnMode)) : nil
        let thermal = thermalEnabled ? (passes: Int32(thermalPasses), angle: Float(thermalAngle)) : nil
        let engine = self.engine

        queue.async { [weak self] in
            let started = Date()

            titan_configure(engine, p.size, 1.0, p.scale, p.height, p.seed, p.octaves,
                            p.persistence, p.lacunarity, p.exponent, p.noise, p.warp,
                            1.0, 2.0, 0.0, 0.0)
            titan_generate(engine)

            let stage: (String) -> Void = { label in
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.runCounter == run else { return }
                    self.statusText = label
                }
            }

            if let rivers {
                stage("Carving river networks…")
                titan_erode_fluvial(engine, rivers.passes, rivers.strength)
            }
            if let droplets {
                stage("Hydraulic erosion (\(droplets.count) droplets)…")
                titan_erode_hydraulic(engine, droplets.count, droplets.mode)
            }
            if let thermal {
                stage("Thermal weathering…")
                titan_erode_thermal(engine, thermal.passes, thermal.angle, 0.5)
            }

            let snapshot = Self.snapshotMesh(engine, heightMax: p.height, extent: Float(p.size))
            let elapsed = Int(Date().timeIntervalSince(started) * 1000)

            DispatchQueue.main.async { [weak self] in
                guard let self, self.runCounter == run else { return }
                self.isGenerating = false
                self.lastComputeMs = elapsed
                self.statusText = "\(self.engineVersion) — \(elapsed) ms"
                if let snapshot { self.onMesh?(snapshot) }
            }
        }
    }

    nonisolated private static func snapshotMesh(_ engine: OpaquePointer?, heightMax: Float,
                                                 extent: Float) -> MeshSnapshot? {
        titan_build_mesh(engine)
        let vertexCount = Int(titan_mesh_vertex_count(engine))
        let indexCount = Int(titan_mesh_index_count(engine))
        guard vertexCount > 0, indexCount > 0,
              let positions = titan_mesh_positions_ptr(engine),
              let normals = titan_mesh_normals_ptr(engine),
              let colors = titan_mesh_colors_ptr(engine),
              let indexPtr = titan_mesh_indices_ptr(engine) else { return nil }

        var interleaved = [Float](repeating: 0, count: vertexCount * 10)
        for v in 0..<vertexCount {
            let o = v * 10
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
        }
        let indices = Array(UnsafeBufferPointer(start: indexPtr, count: indexCount))
        return MeshSnapshot(interleaved: interleaved, indices: indices,
                            vertexCount: vertexCount, heightMax: heightMax,
                            terrainExtent: extent)
    }

    // MARK: - Export

    func exportData(kind: String) -> Data? {
        let sizeBytes: Int32
        switch kind {
        case "r16": sizeBytes = titan_export_r16(engine)
        case "png16": sizeBytes = titan_export_png16(engine)
        case "r32": sizeBytes = titan_export_r32(engine)
        case "exr": sizeBytes = titan_export_exr(engine)
        case "obj": sizeBytes = titan_export_obj(engine)
        default: return nil
        }
        guard sizeBytes > 0, let ptr = titan_export_data_ptr(engine) else { return nil }
        return Data(bytes: ptr, count: Int(sizeBytes))
    }

    // MARK: - Presets (mirror src/core/pipeline.ts)

    static func presets() -> [Preset] {
        [
            Preset(name: "Alpine Peaks", tagline: "Ridged ranges, drainage, talus") { m in
                m.scale = 2.5; m.heightMultiplier = 70; m.octaves = 8; m.exponent = 1.1
                m.warpStrength = 0.6; m.noiseType = 2
                m.riversEnabled = true; m.riverPasses = 3; m.riverStrength = 1.2
                m.dropletsEnabled = true; m.dropletRounds = 4; m.dropletSpawnMode = 1
                m.thermalEnabled = true; m.thermalPasses = 12; m.thermalAngle = 35
            },
            Preset(name: "Island Chain", tagline: "Soft archipelago") { m in
                m.scale = 1.8; m.heightMultiplier = 45; m.octaves = 6; m.exponent = 1.9
                m.warpStrength = 0.9; m.noiseType = 1
                m.riversEnabled = false
                m.dropletsEnabled = true; m.dropletRounds = 3; m.dropletSpawnMode = 0
                m.thermalEnabled = true; m.thermalPasses = 8; m.thermalAngle = 33
            },
            Preset(name: "Canyonlands", tagline: "Terraced mesas, deep channels") { m in
                m.scale = 1.5; m.heightMultiplier = 60; m.octaves = 6; m.exponent = 1.4
                m.warpStrength = 0.3; m.noiseType = 1
                m.riversEnabled = true; m.riverPasses = 4; m.riverStrength = 1.6
                m.dropletsEnabled = false
                m.thermalEnabled = true; m.thermalPasses = 6; m.thermalAngle = 38
            },
            Preset(name: "Rolling Dunes", tagline: "Wind-settled billows") { m in
                m.scale = 3.0; m.heightMultiplier = 25; m.octaves = 4; m.exponent = 1.0
                m.warpStrength = 0.4; m.noiseType = 3
                m.riversEnabled = false; m.dropletsEnabled = false
                m.thermalEnabled = true; m.thermalPasses = 20; m.thermalAngle = 30
            },
            Preset(name: "Volcanic Shield", tagline: "Caldera dome, radial gullies") { m in
                m.scale = 1.2; m.heightMultiplier = 90; m.octaves = 7; m.exponent = 1.6
                m.warpStrength = 0.5; m.noiseType = 2
                m.riversEnabled = true; m.riverPasses = 2; m.riverStrength = 0.8
                m.dropletsEnabled = true; m.dropletRounds = 2; m.dropletSpawnMode = 1
                m.thermalEnabled = true; m.thermalPasses = 10; m.thermalAngle = 33
            },
        ]
    }
}
