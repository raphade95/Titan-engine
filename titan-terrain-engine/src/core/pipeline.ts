// The Titan layer stack: an ordered, serializable pipeline of simulation and
// shaping passes applied on top of the base noise. The stack (plus the base
// params and seed) fully determines the terrain — the engine is
// deterministic, so a .titan project file is a perfect reproduction recipe.

import { TitanCore } from './TitanCore';
import { TerrainParams } from './types';

export type LayerType = 'hydraulic' | 'fluvial' | 'thermal' | 'terrace' | 'plateau';

export interface ParamDef {
  key: string;
  label: string;
  min: number;
  max: number;
  step: number;
  defaultValue: number;
  choices?: string[]; // when set, render as a segmented choice instead of a slider
}

export interface LayerDef {
  type: LayerType;
  label: string;
  description: string;
  params: ParamDef[];
}

export interface Layer {
  id: string;
  type: LayerType;
  enabled: boolean;
  params: Record<string, number>;
}

export interface TitanProject {
  version: 1;
  params: TerrainParams;
  stack: Layer[];
}

export const LAYER_DEFS: Record<LayerType, LayerDef> = {
  fluvial: {
    type: 'fluvial',
    label: 'River Networks',
    description: 'Routes rainfall map-wide and carves connected drainage networks (stream power).',
    params: [
      { key: 'passes', label: 'Passes', min: 1, max: 10, step: 1, defaultValue: 2 },
      { key: 'strength', label: 'Strength', min: 0.1, max: 3, step: 0.1, defaultValue: 1.0 },
    ],
  },
  hydraulic: {
    type: 'hydraulic',
    label: 'Hydraulic Erosion',
    description: 'Droplet simulation carving riverbeds and depositing sediment.',
    params: [
      { key: 'iterations', label: 'Droplets', min: 16384, max: 196608, step: 16384, defaultValue: 49152 },
      { key: 'spawnMode', label: 'Rainfall', min: 0, max: 1, step: 1, defaultValue: 0, choices: ['Uniform', 'Highlands'] },
    ],
  },
  thermal: {
    type: 'thermal',
    label: 'Thermal Weathering',
    description: 'Loose material settles to the angle of repose, forming talus slopes.',
    params: [
      { key: 'passes', label: 'Passes', min: 1, max: 50, step: 1, defaultValue: 10 },
      { key: 'talusAngle', label: 'Repose Angle', min: 20, max: 45, step: 1, defaultValue: 33 },
      { key: 'rate', label: 'Rate', min: 0.1, max: 1, step: 0.05, defaultValue: 0.5 },
    ],
  },
  terrace: {
    type: 'terrace',
    label: 'Terrace',
    description: 'Steps the terrain into geological banding.',
    params: [
      { key: 'interval', label: 'Interval', min: 2, max: 30, step: 0.5, defaultValue: 10 },
      { key: 'strength', label: 'Strength', min: 0, max: 1, step: 0.05, defaultValue: 0.7 },
      { key: 'sharpness', label: 'Sharpness', min: 1, max: 6, step: 0.5, defaultValue: 2 },
    ],
  },
  plateau: {
    type: 'plateau',
    label: 'Plateau',
    description: 'Compresses peaks toward a ceiling with a rounded shoulder.',
    params: [
      { key: 'height', label: 'Height', min: 5, max: 200, step: 1, defaultValue: 60 },
      { key: 'softness', label: 'Softness', min: 1, max: 40, step: 1, defaultValue: 10 },
    ],
  },
};

let nextLayerId = 1;

export function makeLayer(type: LayerType): Layer {
  const params: Record<string, number> = {};
  for (const p of LAYER_DEFS[type].params) params[p.key] = p.defaultValue;
  return { id: `layer-${nextLayerId++}-${Math.random().toString(36).slice(2, 7)}`, type, enabled: true, params };
}

// ---------------------------------------------------------------------------
// Pipeline runner. Work is chunked so the UI stays responsive and the mesh
// can update live; chunk boundaries are chosen to preserve the engine's
// determinism contract (hydraulic chunks = whole rounds).
// ---------------------------------------------------------------------------

export interface RunCallbacks {
  onProgress: (fraction: number, label: string) => void;
  onChunk: () => void; // typically: rebuild the mesh
  shouldCancel: () => boolean;
}

const yieldFrame = () => new Promise<void>(resolve => setTimeout(resolve, 0));

function layerWorkUnits(layer: Layer): number {
  if (!layer.enabled) return 0;
  switch (layer.type) {
    case 'hydraulic': return Math.ceil(layer.params.iterations / TitanCore.DROPLETS_PER_ROUND);
    case 'fluvial': return layer.params.passes * 2;
    case 'thermal': return Math.ceil(layer.params.passes / 4);
    default: return 1;
  }
}

export async function runPipeline(
  engine: TitanCore,
  project: TitanProject,
  cb: RunCallbacks
): Promise<boolean> {
  const totalUnits = 1 + project.stack.reduce((sum, l) => sum + layerWorkUnits(l), 0);
  let doneUnits = 0;
  const tick = (label: string) => cb.onProgress(Math.min(1, doneUnits / totalUnits), label);

  tick('Generating base terrain');
  engine.configure(project.params);
  engine.generate();
  doneUnits += 1;
  cb.onChunk();
  await yieldFrame();
  if (cb.shouldCancel()) return false;

  for (const layer of project.stack) {
    if (!layer.enabled) continue;
    const def = LAYER_DEFS[layer.type];

    switch (layer.type) {
      case 'hydraulic': {
        const rounds = Math.ceil(layer.params.iterations / TitanCore.DROPLETS_PER_ROUND);
        for (let r = 0; r < rounds; ++r) {
          engine.erodeHydraulic(TitanCore.DROPLETS_PER_ROUND, layer.params.spawnMode);
          doneUnits += 1;
          tick(`${def.label} — ${r + 1}/${rounds}`);
          cb.onChunk();
          await yieldFrame();
          if (cb.shouldCancel()) return false;
        }
        break;
      }
      case 'fluvial': {
        const passes = layer.params.passes;
        for (let i = 0; i < passes; ++i) {
          engine.erodeFluvial(1, layer.params.strength);
          doneUnits += 2;
          tick(`${def.label} — ${i + 1}/${passes}`);
          cb.onChunk();
          await yieldFrame();
          if (cb.shouldCancel()) return false;
        }
        break;
      }
      case 'thermal': {
        let remaining = layer.params.passes;
        let done = 0;
        while (remaining > 0) {
          const step = Math.min(4, remaining);
          engine.erodeThermal(step, layer.params.talusAngle, layer.params.rate);
          remaining -= step;
          done += step;
          doneUnits += 1;
          tick(`${def.label} — ${done}/${layer.params.passes}`);
          cb.onChunk();
          await yieldFrame();
          if (cb.shouldCancel()) return false;
        }
        break;
      }
      case 'terrace':
        engine.applyTerrace(layer.params.interval, layer.params.strength, layer.params.sharpness);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'plateau':
        engine.applyPlateau(layer.params.height, layer.params.softness);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
    }
  }

  cb.onProgress(1, 'Done');
  return true;
}

// ---------------------------------------------------------------------------
// Project (de)serialization — the .titan file format.
// ---------------------------------------------------------------------------

export function serializeProject(project: TitanProject): string {
  return JSON.stringify(project, null, 2);
}

export function deserializeProject(json: string): TitanProject {
  const raw = JSON.parse(json);
  if (raw?.version !== 1 || !raw.params || !Array.isArray(raw.stack)) {
    throw new Error('Not a valid .titan project file');
  }
  const stack: Layer[] = raw.stack
    .filter((l: any) => l && LAYER_DEFS[l.type as LayerType])
    .map((l: any) => {
      const layer = makeLayer(l.type as LayerType);
      layer.enabled = l.enabled !== false;
      for (const p of LAYER_DEFS[layer.type].params) {
        const v = Number(l.params?.[p.key]);
        if (Number.isFinite(v)) layer.params[p.key] = Math.min(p.max, Math.max(p.min, v));
      }
      return layer;
    });
  return { version: 1, params: raw.params as TerrainParams, stack };
}

// ---------------------------------------------------------------------------
// Presets — each is a complete recipe: base params + stack. Seeds are filled
// in fresh at apply time (unless the user locked theirs).
// ---------------------------------------------------------------------------

export interface Preset {
  name: string;
  tagline: string;
  params: Omit<TerrainParams, 'seed' | 'size'>;
  stack: Array<{ type: LayerType; params?: Record<string, number> }>;
}

export const PRESETS: Preset[] = [
  {
    name: 'Alpine Peaks',
    tagline: 'Ridged ranges with carved drainages and talus',
    params: {
      scale: 2.5, heightMultiplier: 70, octaves: 8, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.1, warpStrength: 0.6,
      noiseType: 'ridged', biome: 'temperate',
    },
    stack: [
      { type: 'fluvial', params: { passes: 3, strength: 1.2 } },
      { type: 'hydraulic', params: { iterations: 65536, spawnMode: 1 } },
      { type: 'thermal', params: { passes: 12, talusAngle: 35, rate: 0.5 } },
    ],
  },
  {
    name: 'Island Chain',
    tagline: 'Soft archipelago rising from the sea',
    params: {
      scale: 1.8, heightMultiplier: 45, octaves: 6, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.9, warpStrength: 0.9,
      noiseType: 'standard', biome: 'temperate',
    },
    stack: [
      { type: 'hydraulic', params: { iterations: 49152, spawnMode: 0 } },
      { type: 'thermal', params: { passes: 8, talusAngle: 33, rate: 0.5 } },
    ],
  },
  {
    name: 'Canyonlands',
    tagline: 'Terraced mesas cut by deep river channels',
    params: {
      scale: 1.5, heightMultiplier: 60, octaves: 6, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.4, warpStrength: 0.3,
      noiseType: 'standard', biome: 'desert',
    },
    stack: [
      { type: 'terrace', params: { interval: 12, strength: 0.85, sharpness: 3 } },
      { type: 'fluvial', params: { passes: 4, strength: 1.6 } },
      { type: 'thermal', params: { passes: 6, talusAngle: 38, rate: 0.4 } },
    ],
  },
  {
    name: 'Rolling Dunes',
    tagline: 'Wind-settled billows of soft sand',
    params: {
      scale: 3.0, heightMultiplier: 25, octaves: 4, persistence: 0.45,
      lacunarity: 2.0, exponent: 1.0, warpStrength: 0.4,
      noiseType: 'billow', biome: 'desert',
    },
    stack: [
      { type: 'thermal', params: { passes: 20, talusAngle: 30, rate: 0.7 } },
    ],
  },
  {
    name: 'Volcanic Shield',
    tagline: 'A flattened caldera dome with radial gullies',
    params: {
      scale: 1.2, heightMultiplier: 90, octaves: 7, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.6, warpStrength: 0.5,
      noiseType: 'ridged', biome: 'volcanic',
    },
    stack: [
      { type: 'plateau', params: { height: 70, softness: 12 } },
      { type: 'fluvial', params: { passes: 2, strength: 0.8 } },
      { type: 'hydraulic', params: { iterations: 32768, spawnMode: 1 } },
      { type: 'thermal', params: { passes: 10, talusAngle: 33, rate: 0.5 } },
    ],
  },
];

export function instantiatePreset(preset: Preset): { params: Omit<TerrainParams, 'seed' | 'size'>; stack: Layer[] } {
  const stack = preset.stack.map(entry => {
    const layer = makeLayer(entry.type);
    if (entry.params) Object.assign(layer.params, entry.params);
    return layer;
  });
  return { params: preset.params, stack };
}
