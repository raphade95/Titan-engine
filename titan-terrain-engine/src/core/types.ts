export type NoiseType =
  | 'none'
  | 'standard'
  | 'ridged'
  | 'billow'
  | 'voronoi'          // cellular F1 (plateau cells)
  | 'voronoiRidge'     // cellular F2-F1 (ridged cell walls)
  | 'worleyManhattan'  // cellular F1, manhattan distance (blocky)
  | 'worleyChebyshev'  // cellular F1, chebyshev distance (square)
  | 'hybrid';          // Musgrave hybrid multifractal (terrain mode)

export type BiomeType = 'arctic' | 'temperate' | 'volcanic' | 'desert';

export interface TerrainParams {
  /** Samples per edge. Pure detail density — see worldSize. */
  size: number;
  /**
   * Edge length of the terrain in world units.
   *
   * Split from `size` in .titan v2. The hosts used to hardcode the engine's
   * cellSize to 1.0, which made the world extent equal to the pixel count:
   * raising Resolution widened the map while Height stayed absolute, so the
   * same seed came out dramatically flatter at higher settings. Resolution is
   * now sample density and this is the world.
   */
  worldSize: number;
  scale: number;
  heightMultiplier: number;
  seed: string;
  octaves: number;
  persistence: number;
  lacunarity: number;
  exponent: number;
  warpStrength: number;
  noiseType: NoiseType;
  biome: BiomeType; // render-side only; never reaches the engine
}

export type ExportKind =
  | 'png16' | 'r16' | 'r32' | 'exr' | 'obj' | 'normal' | 'ao' | 'splat';

export interface MeshData {
  vertices: Float32Array;
  normals: Float32Array;
  indices: Uint32Array;
  colors: Float32Array;
  uvs: Float32Array;
  /** vec4 per vertex: molten depth, heat 0..1, chilled-rock depth, glow. */
  lava: Float32Array;
  /** vec4 per vertex: ambient occlusion, curvature, snow depth, water depth. */
  surface: Float32Array;
}
