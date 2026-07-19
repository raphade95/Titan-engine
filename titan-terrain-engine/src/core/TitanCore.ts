// Thin TypeScript wrapper around the WASM build of libTitanCore.
//
// All terrain math lives in C++ (cpp/libTitanCore). This file only marshals
// parameters in and copies buffers out — it must never contain generation or
// erosion logic. The same C++ compiles natively for the macOS app and the
// Unreal plugin.

import createTitanModule from '../wasm/titan_core.js';
import { TerrainParams, MeshData } from './types';

const NOISE_TYPE_IDS: Record<string, number> = {
  none: 0,
  standard: 1,
  ridged: 2,
  billow: 3,
};

// FNV-1a: stable string → uint32 seed hash.
export function hashSeed(seed: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < seed.length; i++) {
    h ^= seed.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

export class TitanCore {
  private module: any;
  private handle: number;
  private size = 0;

  private constructor(module: any, handle: number) {
    this.module = module;
    this.handle = handle;
  }

  static async create(): Promise<TitanCore> {
    const module = await createTitanModule();
    const handle = module._titan_create();
    return new TitanCore(module, handle);
  }

  version(): string {
    return this.module.UTF8ToString(this.module._titan_version());
  }

  configure(params: TerrainParams): void {
    this.size = params.size;
    this.module._titan_configure(
      this.handle,
      params.size,
      1.0, // cellSize
      params.scale,
      params.heightMultiplier,
      hashSeed(params.seed),
      params.octaves,
      params.persistence,
      params.lacunarity,
      params.exponent,
      NOISE_TYPE_IDS[params.noiseType] ?? 1,
      params.warpStrength,
      1.0, // ridgeOffset
      2.0, // ridgeGain
      0.0, // originX
      0.0  // originY
    );
  }

  generate(): void {
    this.module._titan_generate(this.handle);
  }

  erodeHydraulic(iterations: number): void {
    this.module._titan_erode_hydraulic(this.handle, iterations);
  }

  erodeThermal(passes: number, talusAngleDeg = 33, rate = 0.5): void {
    this.module._titan_erode_thermal(this.handle, passes, talusAngleDeg, rate);
  }

  erodeFluvial(iterations: number, strength = 1.0): void {
    this.module._titan_erode_fluvial(this.handle, iterations, strength);
  }

  carve(x: number, y: number, radius: number, depth: number): void {
    this.module._titan_carve(this.handle, x, y, radius, depth);
  }

  buildMesh(): MeshData {
    const m = this.module;
    m._titan_build_mesh(this.handle);

    const vertexCount = m._titan_mesh_vertex_count(this.handle);
    const indexCount = m._titan_mesh_index_count(this.handle);

    // Copy out of the WASM heap: the heap can move on growth, and Three.js
    // holds onto these arrays.
    const f32 = (ptr: number, n: number) =>
      m.HEAPF32.slice(ptr >> 2, (ptr >> 2) + n) as Float32Array;

    return {
      vertices: f32(m._titan_mesh_positions_ptr(this.handle), vertexCount * 3),
      normals: f32(m._titan_mesh_normals_ptr(this.handle), vertexCount * 3),
      colors: f32(m._titan_mesh_colors_ptr(this.handle), vertexCount * 4),
      uvs: f32(m._titan_mesh_uvs_ptr(this.handle), vertexCount * 2),
      indices: m.HEAPU32.slice(
        m._titan_mesh_indices_ptr(this.handle) >> 2,
        (m._titan_mesh_indices_ptr(this.handle) >> 2) + indexCount
      ) as Uint32Array,
    };
  }

  // Layer snapshots (copies — safe to hold across engine calls).
  getBedrockMap(): Float32Array {
    return this.copyLayer(this.module._titan_bedrock_ptr(this.handle));
  }

  getSedimentMap(): Float32Array {
    return this.copyLayer(this.module._titan_sediment_ptr(this.handle));
  }

  getFlowMap(): Float32Array {
    return this.copyLayer(this.module._titan_flow_ptr(this.handle));
  }

  // Point probes for the inspector.
  getHeight(x: number, y: number): number {
    return this.module._titan_height_at(this.handle, x, y);
  }

  getSediment(x: number, y: number): number {
    return this.module._titan_sediment_at(this.handle, x, y);
  }

  getFlow(x: number, y: number): number {
    return this.module._titan_flow_at(this.handle, x, y);
  }

  calculateSlope(x: number, y: number): number {
    return this.module._titan_slope_at(this.handle, x, y);
  }

  dispose(): void {
    if (this.handle) {
      this.module._titan_destroy(this.handle);
      this.handle = 0;
    }
  }

  private copyLayer(ptr: number): Float32Array {
    const n = this.size * this.size;
    return this.module.HEAPF32.slice(ptr >> 2, (ptr >> 2) + n) as Float32Array;
  }
}
