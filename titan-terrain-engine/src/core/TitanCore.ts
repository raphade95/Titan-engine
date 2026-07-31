// Thin TypeScript wrapper around the WASM build of libTitanCore.
//
// All terrain math lives in C++ (cpp/libTitanCore). This file only marshals
// parameters in and copies buffers out — it must never contain generation or
// erosion logic. The same C++ compiles natively for the macOS app and the
// Unreal plugin.

import createTitanModule from '../wasm/titan_core.js';
import { TerrainParams, MeshData, ExportKind } from './types';

const NOISE_TYPE_IDS: Record<string, number> = {
  none: 0,
  standard: 1,
  ridged: 2,
  billow: 3,
  voronoi: 4,
  voronoiRidge: 5,
  worleyManhattan: 6,
  worleyChebyshev: 7,
  hybrid: 8,
};

/**
 * Stable string → uint32 seed hash, computed by the engine.
 *
 * This deliberately calls into C++ rather than reimplementing FNV-1a here.
 * The previous JS version hashed UTF-16 code units, TitanLab hashed UTF-8
 * bytes, and the Unreal plugin hashed truncated wide characters — three
 * "identical" hashes that agreed only on ASCII, so a seed containing an
 * accent, an emoji, or any non-Latin script produced three different terrains
 * across the three products. titan_hash_seed is now the single definition.
 */
export function hashSeed(module: any, seed: string): number {
  const bytes = module.lengthBytesUTF8(seed) + 1;
  const ptr = module._malloc(bytes);
  try {
    module.stringToUTF8(seed, ptr, bytes);
    return module._titan_hash_seed(ptr) >>> 0;
  } finally {
    module._free(ptr);
  }
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
    // Sample spacing, so `size` is detail density and `worldSize` is the world.
    // Noise is sampled in world space at a frequency of scale/extent, so with
    // the extent pinned, refining the grid resolves more detail on the same
    // landform instead of stretching it into a larger, flatter one.
    const cellSize = params.worldSize / Math.max(1, params.size);
    this.module._titan_configure(
      this.handle,
      params.size,
      cellSize,
      params.scale,
      params.heightMultiplier,
      hashSeed(this.module, params.seed),
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

  // spawnMode: 0 uniform, 1 altitude-weighted, 2 precipitation map.
  // Chunk calls in multiples of DROPLETS_PER_ROUND for bit-identical results
  // to a single large call.
  static readonly DROPLETS_PER_ROUND = 16384;

  erodeHydraulic(iterations: number, spawnMode = 0): void {
    this.module._titan_erode_hydraulic(this.handle, iterations, spawnMode);
  }

  erodeThermal(passes: number, talusAngleDeg = 33, rate = 0.5): void {
    this.module._titan_erode_thermal(this.handle, passes, talusAngleDeg, rate);
  }

  erodeFluvial(iterations: number, strength = 1.0): void {
    this.module._titan_erode_fluvial(this.handle, iterations, strength);
  }

  applyTerrace(interval: number, strength: number, sharpness = 2.0): void {
    this.module._titan_apply_terrace(this.handle, interval, strength, sharpness);
  }

  applyPlateau(height: number, softness: number): void {
    this.module._titan_apply_plateau(this.handle, height, softness);
  }

  // --- v0.4 ---------------------------------------------------------------

  clearTerrain(): void {
    this.module._titan_clear_terrain(this.handle);
  }

  /** Active mask gating subsequent ops; null clears. */
  setMask(mask: Float32Array | null): void {
    const m = this.module;
    if (!mask) {
      m._titan_set_mask(this.handle, 0, 0);
      return;
    }
    const bytes = mask.length * 4;
    const ptr = m._malloc(bytes);
    m.HEAPF32.set(mask, ptr >> 2);
    m._titan_set_mask(this.handle, ptr, this.size);
    m._free(ptr);
  }

  applyNoise(p: {
    seedOffset: number; noiseType: number; scale: number; amplitude: number;
    octaves: number; persistence: number; lacunarity: number; exponent: number;
    warpStrength: number; blendMode: number; blendAlpha: number;
  }): void {
    this.module._titan_apply_noise(this.handle, p.seedOffset, p.noiseType,
      p.scale, p.amplitude, p.octaves, p.persistence, p.lacunarity,
      p.exponent, p.warpStrength, p.blendMode, p.blendAlpha);
  }

  applyStamp(p: {
    shape: number; centerX: number; centerY: number; sizeX: number;
    sizeY: number; rotationDeg: number; height: number; falloff: number; op: number;
  }): void {
    this.module._titan_apply_stamp(this.handle, p.shape, p.centerX, p.centerY,
      p.sizeX, p.sizeY, p.rotationDeg, p.height, p.falloff, p.op);
  }

  stampToMask(p: {
    shape: number; centerX: number; centerY: number; sizeX: number;
    sizeY: number; rotationDeg: number; falloff: number;
  }): Float32Array {
    const m = this.module;
    m._titan_stamp_to_mask(this.handle, p.shape, p.centerX, p.centerY,
      p.sizeX, p.sizeY, p.rotationDeg, p.falloff);
    return this.copyLayer(m._titan_scratch_ptr(this.handle));
  }

  applySnow(p: { snowLine: number; amount: number; maxSlopeDeg: number; settlePasses: number; melt: number }): void {
    this.module._titan_apply_snow(this.handle, p.snowLine, p.amount,
      p.maxSlopeDeg, p.settlePasses, p.melt);
  }

  computeWater(): void {
    this.module._titan_compute_water(this.handle);
  }

  getSnowMap(): Float32Array {
    return this.copyLayer(this.module._titan_snow_ptr(this.handle));
  }

  getWaterMap(): Float32Array {
    return this.copyLayer(this.module._titan_water_ptr(this.handle));
  }

  // --- Volcanism ----------------------------------------------------------

  /**
   * Stamps a volcanic edifice and registers its eruption vent.
   *
   * Call once per volcano; every vent registered erupts in the next
   * simulateLava call, so several volcanoes share one flow field and their
   * streams divert each other.
   */
  applyVolcano(p: {
    centerX: number; centerY: number; radius: number; height: number;
    coneExponent: number; craterRadius: number; craterDepth: number;
    rimJaggedness: number; roughness: number; breachAngleDeg: number;
    breachWidthDeg: number; seedOffset: number;
  }): void {
    this.module._titan_apply_volcano(this.handle, p.centerX, p.centerY,
      p.radius, p.height, p.coneExponent, p.craterRadius, p.craterDepth,
      p.rimJaggedness, p.roughness, p.breachAngleDeg, p.breachWidthDeg,
      p.seedOffset);
  }

  /** Runs the cellular lava flow from every registered vent. */
  simulateLava(p: {
    steps: number; eruptionRate: number; viscosity: number;
    solidifyRate: number; coolRate: number; ventRadius: number;
    sustain: boolean;
  }): void {
    this.module._titan_simulate_lava(this.handle, p.steps, p.eruptionRate,
      p.viscosity, p.solidifyRate, p.coolRate, p.ventRadius, p.sustain ? 1 : 0);
  }

  clearLava(): void {
    this.module._titan_clear_lava(this.handle);
  }

  /** Vents registered since the last configure/generate/clear. */
  ventCount(): number {
    return this.module._titan_vent_count(this.handle);
  }

  /**
   * Molten lava depth per cell, or null when nothing has erupted — the engine
   * does not allocate the lava fields for terrain without volcanism.
   */
  getLavaMap(): Float32Array | null {
    const ptr = this.module._titan_lava_ptr(this.handle);
    return ptr ? this.copyLayer(ptr) : null;
  }

  /** Chilled-lava thickness per cell (basalt shading record), or null. */
  getLavaRockMap(): Float32Array | null {
    const ptr = this.module._titan_lava_rock_ptr(this.handle);
    return ptr ? this.copyLayer(ptr) : null;
  }

  erodeHydraulicEx(iterations: number, p: {
    spawnMode: number; inertia: number; capacity: number; minCapacity: number;
    dissolve: number; deposit: number; evaporate: number; gravity: number;
    lifetime: number; radius: number; bedrockSpeed: number;
  }): void {
    this.module._titan_erode_hydraulic_ex(this.handle, iterations, p.spawnMode,
      p.inertia, p.capacity, p.minCapacity, p.dissolve, p.deposit,
      p.evaporate, p.gravity, p.lifetime, p.radius, p.bedrockSpeed);
  }

  erodeThermalEx(passes: number, talusAngleDeg: number, rate: number, bedrockBreakdown: number): void {
    this.module._titan_erode_thermal_ex(this.handle, passes, talusAngleDeg, rate, bedrockBreakdown);
  }

  erodeFluvialEx(iterations: number, p: {
    strength: number; erodeConstant: number; areaExponent: number;
    slopeExponent: number; depositRatio: number; maxStep: number;
  }): void {
    this.module._titan_erode_fluvial_ex(this.handle, iterations, p.strength,
      p.erodeConstant, p.areaExponent, p.slopeExponent, p.depositRatio, p.maxStep);
  }

  // --- v0.5: filters, combiner/import, feature masks, derived maps --------

  applyClamp(minH: number, maxH: number): void {
    this.module._titan_apply_clamp(this.handle, minH, maxH);
  }

  applyTransform(scaleV: number, offset: number, invert: boolean): void {
    this.module._titan_apply_transform(this.handle, scaleV, offset, invert ? 1 : 0);
  }

  applyBlur(radius: number, strength: number): void {
    this.module._titan_apply_blur(this.handle, radius, strength);
  }

  applySharpen(radius: number, strength: number): void {
    this.module._titan_apply_sharpen(this.handle, radius, strength);
  }

  /**
   * Samples the remap curve the engine would apply, for previewing it.
   *
   * Calls into C++ rather than reimplementing the monotone-cubic spline in
   * TypeScript (and again in Swift): a curve editor whose preview disagrees
   * with the result is worse than no editor, and this codebase has already
   * paid once for the same formula living in three places.
   */
  sampleCurve(xs: number[], ys: number[], samples: number): Float32Array {
    const m = this.module;
    const n = Math.min(xs.length, ys.length);
    const ptr = m._malloc(n * 8 + samples * 4);
    try {
      m.HEAPF32.set(xs.slice(0, n), ptr >> 2);
      m.HEAPF32.set(ys.slice(0, n), (ptr >> 2) + n);
      const outPtr = ptr + n * 8;
      m._titan_sample_curve(ptr, ptr + n * 4, n, outPtr, samples);
      return m.HEAPF32.slice(outPtr >> 2, (outPtr >> 2) + samples) as Float32Array;
    } finally {
      m._free(ptr);
    }
  }

  /** Custom transfer curve: control points in [0,1], sorted by x. */
  applyCurve(xs: number[], ys: number[]): void {
    const m = this.module;
    const n = Math.min(xs.length, ys.length);
    const ptr = m._malloc(n * 8);
    m.HEAPF32.set(xs.slice(0, n), ptr >> 2);
    m.HEAPF32.set(ys.slice(0, n), (ptr >> 2) + n);
    m._titan_apply_curve(this.handle, ptr, ptr + n * 4, n);
    m._free(ptr);
  }

  /**
   * General combiner / heightfield import. Resamples data (srcSize x srcSize)
   * to the terrain grid, scales samples by heightScale, blends with the
   * given mode (0 add, 1 sub, 2 mul, 3 max, 4 min, 5 mix by alpha).
   */
  applyHeightfield(data: Float32Array, srcSize: number, heightScale: number,
                   blendMode: number, alpha = 1.0): void {
    const m = this.module;
    const ptr = m._malloc(data.length * 4);
    m.HEAPF32.set(data, ptr >> 2);
    m._titan_apply_heightfield(this.handle, ptr, srcSize, heightScale, blendMode, alpha);
    m._free(ptr);
  }

  /**
   * Horizon-traced ambient occlusion into the engine's AO field, which the
   * next buildMesh carries into the vertex surface attribute.
   *
   * Explicit rather than automatic because it is the engine's most expensive
   * derived map and buildMesh runs once per pipeline chunk. Call it once the
   * stack has settled, then rebuild the mesh.
   */
  computeAO(): void {
    this.module._titan_compute_ao(this.handle);
  }

  /** Grid-wide slope map (rise/run), copied out of the scratch buffer. */
  computeSlopeMap(): Float32Array {
    this.module._titan_compute_slope_map(this.handle);
    return this.copyLayer(this.module._titan_scratch_ptr(this.handle));
  }

  /** Grid-wide curvature map (Laplacian: >0 concave), copied out. */
  computeCurvatureMap(): Float32Array {
    this.module._titan_compute_curvature_map(this.handle);
    return this.copyLayer(this.module._titan_scratch_ptr(this.handle));
  }

  /**
   * Build a feature mask in the engine's scratch buffer.
   * feature: 0 height (normalized), 1 slope (angle/90), 2 curvature.
   */
  maskByFeature(feature: number, lo: number, hi: number, softness: number, invert: boolean): void {
    this.module._titan_mask_by_feature(this.handle, feature, lo, hi, softness, invert ? 1 : 0);
  }

  /** Rasterize a fractal noise field (0..1) into the scratch buffer. */
  noiseToMask(p: {
    seedOffset: number; noiseType: number; scale: number; octaves: number;
    persistence: number; lacunarity: number; warpStrength: number;
  }): void {
    this.module._titan_noise_to_mask(this.handle, p.seedOffset, p.noiseType,
      p.scale, p.octaves, p.persistence, p.lacunarity, p.warpStrength);
  }

  /**
   * Apply MaskByFeature's soft band to whatever is in the scratch buffer.
   *
   * Calls into C++ rather than banding host-side: this curve previously existed
   * in three places at once (here, EngineModel.swift, and MaskByFeature in the
   * engine), three copies of one formula that had to stay in agreement forever.
   */
  bandScratch(lo: number, hi: number, softness: number, invert: boolean): void {
    this.module._titan_band_scratch(this.handle, lo, hi, softness, invert ? 1 : 0);
  }

  /** Promote the scratch buffer (clamped 0..1) to the active mask. */
  setMaskFromScratch(): void {
    this.module._titan_set_mask_from_scratch(this.handle);
  }

  /** Copy of the scratch buffer (derived maps, stamp fields, noise masks). */
  getScratch(): Float32Array {
    return this.copyLayer(this.module._titan_scratch_ptr(this.handle));
  }

  // --- Tiled export -------------------------------------------------------

  /**
   * Samples per edge a tile would have, or 0 if the split is invalid.
   * Use it to validate a tile count before offering it.
   */
  tileResolution(tilesPerSide: number, overlap: number): number {
    return this.module._titan_tile_resolution(this.handle, tilesPerSide, overlap);
  }

  /**
   * One tile of the already-simulated terrain.
   *
   * Tiles are sliced rather than regenerated at their own world origins.
   * World-space noise sampling does make independently generated tiles line up
   * exactly on raw terrain, but erosion is not local — droplets do not cross a
   * tile boundary — so separately eroded tiles seam by several percent of the
   * relief. Every tile also normalizes against the whole terrain's range, so
   * the set assembles without steps.
   *
   * format: 0 = .r16, 1 = 16-bit PNG, 2 = .r32.
   */
  exportTile(tileX: number, tileY: number, tilesPerSide: number,
             overlap: number, format: number): Uint8Array {
    const m = this.module;
    m._titan_clear_error();
    const size = Number(m._titan_export_tile(this.handle, tileX, tileY,
                                             tilesPerSide, overlap, format));
    if (!size) throw new Error(this.lastError() ?? 'tile export failed');
    const ptr = m._titan_export_data_ptr(this.handle) as number;
    return new Uint8Array(m.HEAPU8.buffer, ptr, size).slice();
  }

  /** Last engine error on this thread, or null. */
  lastError(): string | null {
    const ptr = this.module._titan_last_error();
    return ptr ? this.module.UTF8ToString(ptr) : null;
  }

  // Export via the C++ exporters; returns a copy safe to hand to Blob.
  exportFile(kind: ExportKind): Uint8Array {
    const m = this.module;
    m._titan_clear_error();
    const fn = {
      png16: m._titan_export_png16,
      r16: m._titan_export_r16,
      r32: m._titan_export_r32,
      exr: m._titan_export_exr,
      obj: m._titan_export_obj,
      normal: m._titan_export_normal_png,
      ao: m._titan_export_ao_png,
      // Splatmap comes from the engine now. The old JS version computed its
      // rock mask as slope * 2.5 while the mesh used (slope - 0.36) / 0.48, so
      // the exported masks did not match the terrain on screen.
      splat: m._titan_export_splat_png,
    }[kind];

    // Sizes are int64 (a full-resolution OBJ can exceed 2 GB), which the
    // WASM_BIGINT build surfaces as a BigInt.
    const size = Number(fn(this.handle));
    if (!size) {
      throw new Error(this.lastError() ?? 'export failed');
    }
    const ptr = m._titan_export_data_ptr(this.handle) as number;
    return new Uint8Array(m.HEAPU8.buffer, ptr, size).slice();
  }

  /**
   * Actual min/max of the current surface, in world height units.
   *
   * This is NOT [0, heightMultiplier] once a stack has run — erosion lowers
   * peaks while fluvial deposition can pile material well above the Height
   * slider. The normalizing exporters (.png16, .r16) stretch to exactly this
   * range, so it is what a user needs to set a correct Z scale on import.
   */
  heightRange(): { min: number; max: number } {
    const m = this.module;
    const ptr = m._malloc(8);
    try {
      m._titan_height_range(this.handle, ptr, ptr + 4);
      return { min: m.HEAPF32[ptr >> 2], max: m.HEAPF32[(ptr >> 2) + 1] };
    } finally {
      m._free(ptr);
    }
  }

  /**
   * The range the normalizing exporters (.r16/.png16) actually stretch to.
   *
   * Usually identical to heightRange(). It differs when droplet erosion has
   * left a few single-cell sediment towers, which would otherwise spend most
   * of the 16-bit depth on the gap between the terrain and a handful of
   * pixels. This is the range an importing tool's Z scale must be set from,
   * because it is what the file encodes.
   */
  exportHeightRange(): { min: number; max: number } {
    const m = this.module;
    const ptr = m._malloc(8);
    try {
      m._titan_export_height_range(this.handle, ptr, ptr + 4);
      return { min: m.HEAPF32[ptr >> 2], max: m.HEAPF32[(ptr >> 2) + 1] };
    } finally {
      m._free(ptr);
    }
  }

  carve(x: number, y: number, radius: number, depth: number): void {
    this.module._titan_carve(this.handle, x, y, radius, depth);
  }

  /**
   * Build the preview mesh, decimated to at most `maxEdgeVertices` per edge.
   *
   * Preview cost is decoupled from simulation resolution: a 2048 terrain is a
   * fine simulation, but a full-resolution mesh of it is 4.2M vertices, and
   * copying that out of the 32-bit WASM heap into JS typed arrays is roughly
   * 300 MB. Exports always read the full-resolution field.
   */
  buildMesh(maxEdgeVertices = 0): MeshData {
    const m = this.module;
    if (maxEdgeVertices > 0) {
      m._titan_build_mesh_lod(this.handle, maxEdgeVertices);
    } else {
      m._titan_build_mesh(this.handle);
    }

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
      // vec4 per vertex: molten depth, heat, chilled-rock depth, glow. Always
      // present (zeroed without volcanism) so the attribute can be bound
      // unconditionally and the shader needs no variant.
      lava: f32(m._titan_mesh_lava_ptr(this.handle), vertexCount * 4),
      // ao / curvature / snow depth / water depth, all simulated by the engine.
      surface: f32(m._titan_mesh_surface_ptr(this.handle), vertexCount * 4),
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
