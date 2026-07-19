export type NoiseType = 'none' | 'standard' | 'ridged' | 'billow';
export type BiomeType = 'arctic' | 'temperate' | 'volcanic' | 'desert';

export interface TerrainParams {
  size: number;
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

export interface MeshData {
  vertices: Float32Array;
  normals: Float32Array;
  indices: Uint32Array;
  colors: Float32Array;
  uvs: Float32Array;
}
