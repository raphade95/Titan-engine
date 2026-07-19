// Metal renderer: MTKView with orbit/zoom input, plus the MTKViewDelegate
// that owns pipeline state and mesh buffers. Triple-buffer-safe by
// replacing buffers wholesale on mesh updates (small meshes; simplicity
// beats cleverness at this stage).

import AppKit
import Metal
import MetalKit
import simd

struct Uniforms {
    var mvp: float4x4
    var sunDirAndIntensity: SIMD4<Float>
    var cameraPosAndFog: SIMD4<Float>
    var skyColorAndTime: SIMD4<Float>
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

    func zoom(_ delta: Float) {
        distance = min(max(distance * (1.0 - delta * 0.05), 20), 1500)
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

    override var acceptsFirstResponder: Bool { true }

    override func mouseDragged(with event: NSEvent) {
        camera.orbit(dx: Float(event.deltaX), dy: Float(event.deltaY))
    }

    override func scrollWheel(with event: NSEvent) {
        camera.zoom(Float(event.scrollingDeltaY) * 0.1)
    }
}

final class TerrainRenderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private var pipeline: MTLRenderPipelineState?
    private var depthState: MTLDepthStencilState?

    private var vertexBuffer: MTLBuffer?
    private var indexBuffer: MTLBuffer?
    private var indexCount = 0

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
            let desc = MTLRenderPipelineDescriptor()
            desc.vertexFunction = library.makeFunction(name: "terrain_vertex")
            desc.fragmentFunction = library.makeFunction(name: "terrain_fragment")
            desc.colorAttachments[0].pixelFormat = view.colorPixelFormat
            desc.depthAttachmentPixelFormat = view.depthStencilPixelFormat
            pipeline = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            NSLog("TitanLab: shader compilation failed: \(error)")
            return nil
        }

        let depthDesc = MTLDepthStencilDescriptor()
        depthDesc.depthCompareFunction = .less
        depthDesc.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: depthDesc)
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
        view?.camera.target = SIMD3<Float>(0, mesh.heightMax * 0.25, 0)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let terrainView = view as? TerrainMTKView,
              let pipeline,
              let descriptor = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let commandBuffer = commandQueue.makeCommandBuffer(),
              let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: descriptor) else { return }

        if let vertexBuffer, let indexBuffer, indexCount > 0 {
            let aspect = Float(view.drawableSize.width / max(view.drawableSize.height, 1))
            let camera = terrainView.camera
            var uniforms = Uniforms(
                mvp: camera.viewProjection(aspect: aspect),
                sunDirAndIntensity: SIMD4<Float>(0.45, 0.7, 0.35, 1.3),
                cameraPosAndFog: SIMD4<Float>(camera.position, 0.002),
                skyColorAndTime: SIMD4<Float>(skyColor, Float(Date().timeIntervalSince(startTime)))
            )

            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(depthState)
            encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.setFragmentBytes(&uniforms, length: MemoryLayout<Uniforms>.size, index: 1)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: indexCount,
                                          indexType: .uint32, indexBuffer: indexBuffer,
                                          indexBufferOffset: 0)
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
