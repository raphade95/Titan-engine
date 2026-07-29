// Metal renderer: MTKView with orbit/zoom/picking input, plus the
// MTKViewDelegate that owns pipeline state and mesh buffers. Draw order:
// terrain (opaque, optional wireframe) -> flora (instanced) -> grid lines ->
// water plane (translucent). Buffers are replaced wholesale on updates —
// small meshes; simplicity beats cleverness at this stage.

import AppKit
import Metal
import MetalKit
import simd

struct Uniforms {
    var mvp: float4x4
    var sunDirAndIntensity: SIMD4<Float>
    var cameraPosAndFog: SIMD4<Float>
    var skyColorAndTime: SIMD4<Float>
    var misc: SIMD4<Float> // x biome, y sea level, z height scale
}

// Viewer-side settings pushed from the SwiftUI model on every update.
struct RenderSettings {
    var sunElevation: Float = 45
    var sunAzimuth: Float = 45
    var sunIntensity: Float = 1.4
    var fogDensity: Float = 0.002
    var seaLevel: Float = -10
    var biome: Int = 1
    var wireframe = false
    var showGrid = true
}

final class OrbitCamera {
    var azimuth: Float = 0.8
    var elevation: Float = 0.6
    var distance: Float = 220
    var target = SIMD3<Float>(0, 10, 0)

    var position: SIMD3<Float> {
        let x = distance * cos(elevation) * sin(azimuth)
        let y = distance * sin(elevation)
        let z = distance * cos(elevation) * cos(azimuth)
        return target + SIMD3<Float>(x, y, z)
    }

    func orbit(dx: Float, dy: Float) {
        azimuth -= dx * 0.008
        elevation = min(max(elevation + dy * 0.008, 0.05), 1.5)
    }

    /// Orbit limits scale with the terrain — a fixed 20..1500 range could not
    /// frame a large world at all.
    var minDistance: Float = 20
    var maxDistance: Float = 1500

    func zoom(_ delta: Float) {
        distance = min(max(distance * (1.0 - delta * 0.05), minDistance), maxDistance)
    }

    func viewProjection(aspect: Float) -> float4x4 {
        let view = float4x4.lookAt(eye: position, center: target, up: SIMD3<Float>(0, 1, 0))
        let proj = float4x4.perspective(fovYRadians: 60 * .pi / 180, aspect: aspect,
                                        near: 0.5, far: 4000)
        return proj * view
    }
}

final class TerrainMTKView: MTKView {
    let camera = OrbitCamera()

    // Picking state — set by the SwiftUI layer.
    var inspectEnabled = false
    var carveEnabled = false
    var volcanoPlaceEnabled = false
    var terrainSize = 128
    /// World units per cell. Picking works in world space and has to divide by
    /// this to land on a cell — it used to assume one cell was one world unit.
    var cellSize: Float = 1
    var heightAt: ((Int, Int) -> Float)?
    var onProbe: ((Int, Int) -> Void)?
    var onProbeMiss: (() -> Void)?
    var onCarve: ((Int, Int) -> Void)?
    var onPlaceVolcano: ((Int, Int) -> Void)?
    var onDragVolcano: ((Int, Int) -> Void)?

    // Last cell a drag reported, so moving the mouse within one cell does not
    // re-run the whole layer stack.
    private var lastVolcanoCell: (Int, Int)?

    private var trackingArea: NSTrackingArea?

    override var acceptsFirstResponder: Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea { removeTrackingArea(trackingArea) }
        let area = NSTrackingArea(rect: bounds,
                                  options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        trackingArea = area
    }

    override func mouseMoved(with event: NSEvent) {
        guard inspectEnabled || carveEnabled else { return }
        if let hit = raycastTerrain(event) {
            onProbe?(hit.0, hit.1)
        } else {
            onProbeMiss?()
        }
    }

    override func mouseDown(with event: NSEvent) {
        if volcanoPlaceEnabled {
            if let hit = raycastTerrain(event) {
                lastVolcanoCell = hit
                onPlaceVolcano?(hit.0, hit.1)
            }
            return
        }
        if carveEnabled, let hit = raycastTerrain(event) {
            onCarve?(hit.0, hit.1)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        if volcanoPlaceEnabled {
            // Dragging repositions the cone just dropped rather than orbiting.
            guard let hit = raycastTerrain(event) else { return }
            if let last = lastVolcanoCell, last == hit { return }
            lastVolcanoCell = hit
            onDragVolcano?(hit.0, hit.1)
            return
        }
        if carveEnabled {
            if let hit = raycastTerrain(event) { onCarve?(hit.0, hit.1) }
            return
        }
        camera.orbit(dx: Float(event.deltaX), dy: Float(event.deltaY))
    }

    override func mouseUp(with event: NSEvent) {
        lastVolcanoCell = nil
    }

    override func scrollWheel(with event: NSEvent) {
        camera.zoom(Float(event.scrollingDeltaY) * 0.1)
    }

    // Ray-march from the camera through the mouse position until the ray dips
    // below the heightfield. Grid coordinates on hit; nil on miss.
    private func raycastTerrain(_ event: NSEvent) -> (Int, Int)? {
        guard let heightAt, bounds.width > 1, bounds.height > 1 else { return nil }
        let loc = convert(event.locationInWindow, from: nil)
        let ndcX = Float(loc.x / bounds.width) * 2 - 1
        let ndcY = Float(loc.y / bounds.height) * 2 - 1

        let aspect = Float(bounds.width / max(bounds.height, 1))
        let invVP = camera.viewProjection(aspect: aspect).inverse

        func unproject(_ z: Float) -> SIMD3<Float> {
            let clip = SIMD4<Float>(ndcX, ndcY, z, 1)
            let world = invVP * clip
            return SIMD3<Float>(world.x, world.y, world.z) / world.w
        }
        let origin = unproject(0)
        let dir = simd_normalize(unproject(1) - origin)

        let cell = max(cellSize, 1e-4)
        let half = Float(terrainSize) * 0.5 * cell
        let extent = Float(terrainSize) * cell
        var t: Float = 0
        // Step in world units scaled to the sample spacing, so a fine grid over
        // a small world is not marched straight through.
        let fineStep = max(cell * 0.75, 1e-3)
        while t < extent * 8 {
            let p = origin + dir * t
            let gx = (p.x + half) / cell
            let gy = (p.z + half) / cell
            if gx >= 0, gx < Float(terrainSize), gy >= 0, gy < Float(terrainSize) {
                if p.y <= heightAt(Int(gx), Int(gy)) {
                    return (Int(gx), Int(gy))
                }
            }
            // Coarse march far away, fine near the surface band.
            t += p.y > 200 ? fineStep * 5 : fineStep
        }
        return nil
    }
}

final class TerrainRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private var terrainPipeline: MTLRenderPipelineState?
    private var flatPipeline: MTLRenderPipelineState?
    private var waterPipeline: MTLRenderPipelineState?
    private var floraPipeline: MTLRenderPipelineState?
    private var depthState: MTLDepthStencilState?
    private var depthStateNoWrite: MTLDepthStencilState?

    private var vertexBuffer: MTLBuffer?
    private var indexBuffer: MTLBuffer?
    private var indexCount = 0
    // Height scale of the current mesh, pushed to the shader so strata
    // banding keeps a constant band count at any Height setting.
    private var terrainHeightMax: Float = 1
    // World extent of the terrain, so haze reads the same at any resolution.
    private var terrainExtent: Float = 128

    private var gridBuffer: MTLBuffer?
    private var gridVertexCount = 0
    private var gridExtent: Float = 0
    private var waterBuffer: MTLBuffer?
    private var waterVertexCount = 0
    private var floraVertexBuffer: MTLBuffer?
    private var floraVertexCount = 0
    private var floraInstanceBuffer: MTLBuffer?
    private var floraInstanceCount = 0
    private var floraColor = SIMD4<Float>(0.10, 0.20, 0.0, 1)

    var settings = RenderSettings()

    private let skyColor = SIMD3<Float>(0.53, 0.81, 0.92)
    private let startTime = Date()

    weak var view: TerrainMTKView?

    init?(view: TerrainMTKView) {
        guard let device = view.device ?? MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        self.device = device
        self.commandQueue = queue
        self.view = view
        super.init()

        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.depthStencilPixelFormat = .depth32Float
        view.clearColor = MTLClearColor(red: 0.53, green: 0.81, blue: 0.92, alpha: 1)
        view.preferredFramesPerSecond = 60

        do {
            let library = try device.makeLibrary(source: titanShaderSource, options: nil)

            func pipeline(_ vertexFn: String, _ fragmentFn: String,
                          blending: Bool = false) throws -> MTLRenderPipelineState {
                let desc = MTLRenderPipelineDescriptor()
                desc.vertexFunction = library.makeFunction(name: vertexFn)
                desc.fragmentFunction = library.makeFunction(name: fragmentFn)
                desc.colorAttachments[0].pixelFormat = view.colorPixelFormat
                desc.depthAttachmentPixelFormat = view.depthStencilPixelFormat
                if blending {
                    let att = desc.colorAttachments[0]!
                    att.isBlendingEnabled = true
                    att.rgbBlendOperation = .add
                    att.alphaBlendOperation = .add
                    att.sourceRGBBlendFactor = .sourceAlpha
                    att.destinationRGBBlendFactor = .oneMinusSourceAlpha
                    att.sourceAlphaBlendFactor = .one
                    att.destinationAlphaBlendFactor = .oneMinusSourceAlpha
                }
                return try device.makeRenderPipelineState(descriptor: desc)
            }

            terrainPipeline = try pipeline("terrain_vertex", "terrain_fragment")
            flatPipeline = try pipeline("flat_vertex", "flat_fragment")
            waterPipeline = try pipeline("water_vertex", "water_fragment", blending: true)
            floraPipeline = try pipeline("flora_vertex", "flora_fragment")
        } catch {
            NSLog("TitanLab: shader compilation failed: \(error)")
            return nil
        }

        let depthDesc = MTLDepthStencilDescriptor()
        depthDesc.depthCompareFunction = .less
        depthDesc.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: depthDesc)
        depthDesc.isDepthWriteEnabled = false
        depthStateNoWrite = device.makeDepthStencilState(descriptor: depthDesc)

        buildGrid()
        buildWaterPlane()
        buildFloraMesh()
    }

    func setMesh(_ mesh: MeshSnapshot) {
        guard !mesh.interleaved.isEmpty, !mesh.indices.isEmpty else { return }
        vertexBuffer = device.makeBuffer(bytes: mesh.interleaved,
                                         length: mesh.interleaved.count * MemoryLayout<Float>.size,
                                         options: .storageModeShared)
        indexBuffer = device.makeBuffer(bytes: mesh.indices,
                                        length: mesh.indices.count * MemoryLayout<UInt32>.size,
                                        options: .storageModeShared)
        indexCount = mesh.indices.count
        terrainHeightMax = max(1, mesh.heightMax)
        let newExtent = max(1, mesh.terrainExtent)
        if let camera = view?.camera {
            camera.minDistance = newExtent * 0.05
            camera.maxDistance = newExtent * 8
            // Reframe when the world itself resizes, not merely when the mesh
            // is rebuilt — otherwise raising World Size leaves the camera at
            // its old distance and buries it inside the terrain. A pure
            // Resolution change leaves the extent alone and must not disturb
            // whatever view the user has set up.
            if abs(newExtent - terrainExtent) > terrainExtent * 0.01 {
                camera.distance = newExtent * 1.3
            }
            camera.distance = min(max(camera.distance, camera.minDistance), camera.maxDistance)
        }
        terrainExtent = newExtent
        if abs(newExtent - gridExtent) > gridExtent * 0.01 {
            buildGrid(extent: newExtent)
        }
        view?.camera.target = SIMD3<Float>(0, mesh.heightMax * 0.25, 0)
        // terrainExtent is in world units; the picker needs cells plus spacing.
        view?.cellSize = mesh.terrainExtent / Float(max(1, mesh.gridSize))
        view?.terrainSize = mesh.gridSize
    }

    // Flora colors per biome: arctic, temperate, volcanic, desert.
    private static let floraColors: [SIMD4<Float>] = [
        SIMD4<Float>(0.18, 0.31, 0.31, 1),
        SIMD4<Float>(0.10, 0.20, 0.00, 1),
        SIMD4<Float>(0.20, 0.13, 0.07, 1),
        SIMD4<Float>(0.18, 0.35, 0.15, 1),
    ]

    func setFlora(_ instances: [FloraInstance], biome: Int) {
        floraColor = Self.floraColors[min(max(biome, 0), 3)]
        floraInstanceCount = instances.count
        guard !instances.isEmpty else {
            floraInstanceBuffer = nil
            return
        }
        floraInstanceBuffer = device.makeBuffer(
            bytes: instances,
            length: instances.count * MemoryLayout<FloraInstance>.stride,
            options: .storageModeShared)
    }

    // MARK: - Static geometry

    /// Reference grid spanning the terrain plus a margin, 24 divisions.
    ///
    /// This was a fixed 200 world units, which is a postage stamp under a large
    /// world and stops reading as a scale reference at all.
    private func buildGrid(extent: Float = 128) {
        var verts: [Float] = []
        let half = max(32, extent * 0.75)
        let step = half * 2 / 24
        var x = -half
        while x <= half + step * 0.5 {
            verts += [x, -0.1, -half, x, -0.1, half]
            verts += [-half, -0.1, x, half, -0.1, x]
            x += step
        }
        gridVertexCount = verts.count / 3
        gridBuffer = device.makeBuffer(bytes: verts, length: verts.count * 4,
                                       options: .storageModeShared)
        gridExtent = extent
    }

    private func buildWaterPlane() {
        // Tessellated grid centred on the origin; the shader lifts it to sea
        // level and displaces it into swell.
        //
        // This used to be two triangles. The vertex shader has always computed
        // a wave displacement, but with only the four corners to displace,
        // every ripple was interpolated flat across the entire ocean and none
        // of it was ever visible.
        let half: Float = 2000
        let segments = 160
        let step = (half * 2) / Float(segments)
        var verts: [Float] = []
        verts.reserveCapacity(segments * segments * 18)
        for j in 0..<segments {
            for i in 0..<segments {
                let x0 = -half + Float(i) * step, x1 = x0 + step
                let z0 = -half + Float(j) * step, z1 = z0 + step
                verts += [x0, 0, z0,  x1, 0, z0,  x1, 0, z1]
                verts += [x0, 0, z0,  x1, 0, z1,  x0, 0, z1]
            }
        }
        waterVertexCount = verts.count / 3
        waterBuffer = device.makeBuffer(bytes: verts, length: verts.count * 4,
                                        options: .storageModeShared)
    }

    private func buildFloraMesh() {
        // Unit cone: base ring at y=0 (radius 0.4), apex at y=1.5 — roughly
        // the web lab's conifer proportions before per-instance scaling.
        let segments = 6
        let radius: Float = 0.4
        let height: Float = 1.5
        var verts: [Float] = [] // pos3 + normal3

        let slopeY = radius / height
        for s in 0..<segments {
            let a0 = Float(s) / Float(segments) * 2 * .pi
            let a1 = Float(s + 1) / Float(segments) * 2 * .pi
            let p0 = SIMD3<Float>(cos(a0) * radius, 0, sin(a0) * radius)
            let p1 = SIMD3<Float>(cos(a1) * radius, 0, sin(a1) * radius)
            let apex = SIMD3<Float>(0, height, 0)
            let n0 = simd_normalize(SIMD3<Float>(cos(a0), slopeY, sin(a0)))
            let n1 = simd_normalize(SIMD3<Float>(cos(a1), slopeY, sin(a1)))
            let na = simd_normalize(n0 + n1)
            verts += [p0.x, p0.y, p0.z, n0.x, n0.y, n0.z]
            verts += [apex.x, apex.y, apex.z, na.x, na.y, na.z]
            verts += [p1.x, p1.y, p1.z, n1.x, n1.y, n1.z]
        }
        floraVertexCount = verts.count / 6
        floraVertexBuffer = device.makeBuffer(bytes: verts, length: verts.count * 4,
                                              options: .storageModeShared)
    }

    // MARK: - Frame

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let terrainView = view as? TerrainMTKView,
              let terrainPipeline,
              let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue.makeCommandBuffer() else { return }

        // Sun + sky from settings (same spherical mapping as the web lab).
        let phi = (90 - settings.sunElevation) * .pi / 180
        let theta = settings.sunAzimuth * .pi / 180
        let sunDir = SIMD3<Float>(sin(phi) * sin(theta), cos(phi), sin(phi) * cos(theta))
        let skyFactor = max(0.1, settings.sunElevation / 90)
        let sky = skyColor * skyFactor
        view.clearColor = MTLClearColor(red: Double(sky.x), green: Double(sky.y),
                                        blue: Double(sky.z), alpha: 1)

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        let aspect = Float(view.drawableSize.width / max(view.drawableSize.height, 1))
        let camera = terrainView.camera
        var uniforms = Uniforms(
            mvp: camera.viewProjection(aspect: aspect),
            sunDirAndIntensity: SIMD4<Float>(sunDir, settings.sunIntensity),
            cameraPosAndFog: SIMD4<Float>(camera.position, settings.fogDensity),
            skyColorAndTime: SIMD4<Float>(sky, Float(Date().timeIntervalSince(startTime))),
            misc: SIMD4<Float>(Float(settings.biome), settings.seaLevel,
                               terrainHeightMax, terrainExtent)
        )

        // Terrain.
        if let vertexBuffer, let indexBuffer, indexCount > 0 {
            encoder.setRenderPipelineState(terrainPipeline)
            encoder.setDepthStencilState(depthState)
            encoder.setTriangleFillMode(settings.wireframe ? .lines : .fill)
            encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: indexCount,
                                          indexType: .uint32, indexBuffer: indexBuffer,
                                          indexBufferOffset: 0)
            encoder.setTriangleFillMode(.fill)
        }

        // Flora (instanced cones).
        if let floraPipeline, let floraVertexBuffer, let floraInstanceBuffer,
           floraInstanceCount > 0 {
            var color = floraColor
            encoder.setRenderPipelineState(floraPipeline)
            encoder.setDepthStencilState(depthState)
            encoder.setVertexBuffer(floraVertexBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setVertexBuffer(floraInstanceBuffer, offset: 0, index: 2)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0,
                                   vertexCount: floraVertexCount,
                                   instanceCount: floraInstanceCount)
        }

        // Grid helper.
        if settings.showGrid, let flatPipeline, let gridBuffer, gridVertexCount > 0 {
            var color = SIMD4<Float>(0.15, 0.15, 0.16, 1)
            encoder.setRenderPipelineState(flatPipeline)
            encoder.setDepthStencilState(depthState)
            encoder.setVertexBuffer(gridBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
            encoder.drawPrimitives(type: .line, vertexStart: 0, vertexCount: gridVertexCount)
        }

        // Water plane (translucent — depth test on, write off).
        if let waterPipeline, let waterBuffer {
            encoder.setRenderPipelineState(waterPipeline)
            encoder.setDepthStencilState(depthStateNoWrite)
            encoder.setVertexBuffer(waterBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: waterVertexCount)
        }

        encoder.endEncoding()
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}

// MARK: - Matrix helpers

extension float4x4 {
    static func lookAt(eye: SIMD3<Float>, center: SIMD3<Float>, up: SIMD3<Float>) -> float4x4 {
        let z = simd_normalize(eye - center)
        let x = simd_normalize(simd_cross(up, z))
        let y = simd_cross(z, x)
        return float4x4(columns: (
            SIMD4<Float>(x.x, y.x, z.x, 0),
            SIMD4<Float>(x.y, y.y, z.y, 0),
            SIMD4<Float>(x.z, y.z, z.z, 0),
            SIMD4<Float>(-simd_dot(x, eye), -simd_dot(y, eye), -simd_dot(z, eye), 1)
        ))
    }

    static func perspective(fovYRadians: Float, aspect: Float, near: Float, far: Float) -> float4x4 {
        let ys = 1 / tan(fovYRadians * 0.5)
        let xs = ys / aspect
        let zs = far / (near - far)
        return float4x4(columns: (
            SIMD4<Float>(xs, 0, 0, 0),
            SIMD4<Float>(0, ys, 0, 0),
            SIMD4<Float>(0, 0, zs, -1),
            SIMD4<Float>(0, 0, zs * near, 0)
        ))
    }
}
