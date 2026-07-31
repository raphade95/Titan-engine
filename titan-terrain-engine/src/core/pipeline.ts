// The Titan layer stack: an ordered, serializable pipeline of simulation and
// shaping passes applied on top of the base noise. The stack (plus the base
// params and seed) fully determines the terrain — the engine is
// deterministic, so a .titan project file is a perfect reproduction recipe.

import { TitanCore } from './TitanCore';
import { TerrainParams, NoiseType, BiomeType } from './types';

export type LayerType =
  | 'hydraulic' | 'fluvial' | 'thermal' | 'terrace' | 'plateau'
  // v0.5 layers:
  | 'noise' | 'gradient' | 'clamp' | 'curve' | 'blur' | 'sharpen'
  | 'transform' | 'combine'
  // v0.6: previously engine-only, reachable from no UI at all.
  | 'snow' | 'water'
  // v0.7: volcanism. One 'volcano' layer per cone — add several for a field.
  | 'volcano' | 'lava';

export interface ParamDef {
  key: string;
  label: string;
  min: number;
  max: number;
  step: number;
  defaultValue: number;
  choices?: string[]; // when set, render as a segmented choice instead of a slider
  advanced?: boolean; // hidden behind the layer's "Advanced" disclosure
}

export interface LayerDef {
  type: LayerType;
  label: string;
  description: string;
  params: ParamDef[];
}

// Per-layer mask: gates the layer's effect by a terrain feature or noise.
// mode: 0 off, 1 height, 2 slope, 3 curvature, 4 noise.
export interface LayerMask {
  mode: number;
  lo: number;      // band start (normalized feature value 0..1)
  hi: number;      // band end
  invert: boolean;
}

export const MASK_MODE_LABELS = ['Off', 'Height', 'Slope', 'Curve', 'Noise'];

export function defaultMask(): LayerMask {
  return { mode: 0, lo: 0.35, hi: 1.0, invert: false };
}

/** A control point of a height-remap curve. Both axes are 0..1. */
export interface CurvePoint {
  x: number;
  y: number;
}

/** Identity: input height maps to itself. */
export const DEFAULT_CURVE: CurvePoint[] = [
  { x: 0, y: 0 }, { x: 0.25, y: 0.25 }, { x: 0.5, y: 0.5 },
  { x: 0.75, y: 0.75 }, { x: 1, y: 1 },
];

export const MAX_CURVE_POINTS = 12;

/** Sorted by x, endpoints pinned, everything inside the unit square. */
export function sanitizeCurve(raw: any): CurvePoint[] | null {
  if (!Array.isArray(raw) || raw.length < 2) return null;
  const pts: CurvePoint[] = [];
  for (const p of raw.slice(0, MAX_CURVE_POINTS)) {
    const x = Number(p?.x);
    const y = Number(p?.y);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return null;
    pts.push({ x: Math.min(1, Math.max(0, x)), y: Math.min(1, Math.max(0, y)) });
  }
  pts.sort((a, b) => a.x - b.x);
  // The engine's spline assumes strictly increasing x; equal x would divide by
  // a clamped epsilon and produce a near-vertical segment.
  pts[0].x = 0;
  pts[pts.length - 1].x = 1;
  for (let i = 1; i < pts.length; i++) {
    if (pts[i].x <= pts[i - 1].x) pts[i].x = Math.min(1, pts[i - 1].x + 1e-3);
  }
  return pts;
}

export interface Layer {
  id: string;
  type: LayerType;
  enabled: boolean;
  params: Record<string, number>;
  mask: LayerMask;
  /**
   * Height-remap control points, for `curve` layers.
   *
   * The engine has always accepted an arbitrary number of monotone-cubic
   * control points; the UI exposed five fixed sliders. This carries what the
   * editor produces. Absent on layers loaded from a file that predates it, in
   * which case the loader rebuilds it from the old y0..y4 params.
   */
  curve?: CurvePoint[];
}

// An imported heightfield (values normalized 0..1). Acts as an additive
// base after generation, and feeds 'combine' layers.
export interface ImportedField {
  size: number;
  data: Float32Array;
  name?: string;
}

/**
 * Project file version.
 *
 * 1 — world extent was implicitly the sample count (engine cellSize 1.0).
 * 2 — `worldSize` is explicit and `size` is pure sample density.
 * 3 — curve layers carry arbitrary control points instead of five fixed ones.
 *
 * Old files always load: v1 sets worldSize = size, exactly what the old model
 * computed, and a v2 curve layer is rebuilt from its y0..y4 params.
 *
 * A file is stamped with the *minimum* version needed to read it correctly,
 * not simply the newest this build knows. A project that uses no custom curve
 * is still a valid v2 and still opens in a build that predates them; only one
 * that would be silently mis-reproduced is marked v3 and refused. Reproduction
 * is what the format is for, so a reader that cannot honour a file should say
 * so rather than approximate it.
 */
export const TITAN_PROJECT_VERSION = 3;
export type TitanProjectVersion = 1 | 2 | 3;

/** Lowest reader version that can reproduce this project faithfully. */
export function requiredVersion(project: TitanProject): TitanProjectVersion {
  const usesCustomCurve = project.stack.some(
    l => l.type === 'curve' && Array.isArray(l.curve) && l.curve.length > 0
  );
  return usesCustomCurve ? 3 : 2;
}

export interface TitanProject {
  version: TitanProjectVersion;
  params: TerrainParams;
  stack: Layer[];
  imported?: ImportedField;
}

// Engine ids for the noise-layer type choice (index -> NoiseType id).
export const LAYER_NOISE_IDS = [1, 2, 3, 4, 5, 6, 7, 8];
export const LAYER_NOISE_LABELS = [
  'Simplex', 'Ridged', 'Billow', 'Cells', 'Walls', 'Blocks', 'Squares', 'Hybrid',
];

export const BLEND_LABELS = ['Add', 'Subtract', 'Multiply', 'Max', 'Min', 'Mix'];

export const LAYER_DEFS: Record<LayerType, LayerDef> = {
  fluvial: {
    type: 'fluvial',
    label: 'River Networks',
    description: 'Routes rainfall map-wide and carves connected drainage networks (stream power).',
    params: [
      { key: 'passes', label: 'Passes', min: 1, max: 10, step: 1, defaultValue: 2 },
      { key: 'strength', label: 'Strength', min: 0.1, max: 3, step: 0.1, defaultValue: 1.0 },
      // Stream-power controls (E = K * A^m * S^n). These existed in the engine
      // as titan_erode_fluvial_ex from v0.4 but were reachable from no UI.
      { key: 'erodeConstant', label: 'K (erodibility)', min: 0.001, max: 0.1, step: 0.001, defaultValue: 0.015, advanced: true },
      { key: 'areaExponent', label: 'm (area)', min: 0.1, max: 1.5, step: 0.05, defaultValue: 0.5, advanced: true },
      { key: 'slopeExponent', label: 'n (slope)', min: 0.5, max: 2.5, step: 0.05, defaultValue: 1.0, advanced: true },
      { key: 'depositRatio', label: 'Deposit Ratio', min: 0, max: 1, step: 0.05, defaultValue: 0.3, advanced: true },
      { key: 'maxStep', label: 'Max Step', min: 0.1, max: 10, step: 0.1, defaultValue: 2.0, advanced: true },
    ],
  },
  hydraulic: {
    type: 'hydraulic',
    label: 'Hydraulic Erosion',
    description: 'Droplet simulation carving riverbeds and depositing sediment.',
    params: [
      { key: 'iterations', label: 'Droplets', min: 16384, max: 196608, step: 16384, defaultValue: 49152 },
      { key: 'spawnMode', label: 'Rainfall', min: 0, max: 1, step: 1, defaultValue: 0, choices: ['Uniform', 'Highlands'] },
      // Droplet physics. titan_erode_hydraulic_ex has carried these since
      // v0.4; nothing exposed them, so the tuning surface competitors are
      // judged on was invisible.
      { key: 'inertia', label: 'Inertia', min: 0, max: 0.95, step: 0.05, defaultValue: 0.1, advanced: true },
      { key: 'capacity', label: 'Sediment Capacity', min: 0.5, max: 16, step: 0.5, defaultValue: 4.0, advanced: true },
      { key: 'dissolve', label: 'Dissolve Rate', min: 0.01, max: 1, step: 0.01, defaultValue: 0.1, advanced: true },
      { key: 'deposit', label: 'Deposit Rate', min: 0.01, max: 1, step: 0.01, defaultValue: 0.1, advanced: true },
      { key: 'evaporate', label: 'Evaporation', min: 0.001, max: 0.1, step: 0.001, defaultValue: 0.01, advanced: true },
      { key: 'gravity', label: 'Gravity', min: 0.5, max: 20, step: 0.5, defaultValue: 4.0, advanced: true },
      { key: 'lifetime', label: 'Droplet Lifetime', min: 8, max: 128, step: 1, defaultValue: 60, advanced: true },
      { key: 'radius', label: 'Erosion Radius', min: 1, max: 12, step: 1, defaultValue: 3, advanced: true },
      { key: 'bedrockSpeed', label: 'Bedrock Rate', min: 0.005, max: 0.5, step: 0.005, defaultValue: 0.05, advanced: true },
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
      { key: 'bedrockBreakdown', label: 'Bedrock Breakdown', min: 0, max: 0.5, step: 0.01, defaultValue: 0.05, advanced: true },
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
  noise: {
    type: 'noise',
    label: 'Add Noise',
    description: 'Stacks a second noise field onto the terrain with a blend mode. Cells/Walls are voronoi; Blocks/Squares are worley distance variants.',
    params: [
      { key: 'noiseType', label: 'Type', min: 0, max: 7, step: 1, defaultValue: 0, choices: LAYER_NOISE_LABELS },
      { key: 'blend', label: 'Blend', min: 0, max: 5, step: 1, defaultValue: 0, choices: BLEND_LABELS },
      { key: 'scale', label: 'Scale', min: 0.5, max: 12, step: 0.5, defaultValue: 4 },
      { key: 'amplitude', label: 'Amplitude', min: 1, max: 80, step: 1, defaultValue: 15 },
      { key: 'octaves', label: 'Octaves', min: 1, max: 10, step: 1, defaultValue: 5 },
      { key: 'alpha', label: 'Mix Alpha', min: 0, max: 1, step: 0.05, defaultValue: 0.5 },
      { key: 'seedOffset', label: 'Variant', min: 0, max: 9, step: 1, defaultValue: 1 },
    ],
  },
  gradient: {
    type: 'gradient',
    label: 'Gradient',
    description: 'Linear or radial height ramp across the whole map — tilt a landmass or raise an island core.',
    params: [
      { key: 'kind', label: 'Shape', min: 0, max: 1, step: 1, defaultValue: 0, choices: ['Linear', 'Radial'] },
      { key: 'op', label: 'Operation', min: 0, max: 2, step: 1, defaultValue: 0, choices: ['Raise', 'Lower', 'Union'] },
      { key: 'angle', label: 'Angle', min: 0, max: 360, step: 5, defaultValue: 0 },
      { key: 'height', label: 'Height', min: 1, max: 100, step: 1, defaultValue: 25 },
    ],
  },
  clamp: {
    type: 'clamp',
    label: 'Clamp',
    description: 'Clips terrain heights into a min/max band — instant sea floors and flat-topped mesas.',
    params: [
      // Negative minimum is meaningful now that the engine represents
      // below-datum terrain — that is what makes the "sea floors" claim real.
      { key: 'min', label: 'Min Height', min: -100, max: 200, step: 1, defaultValue: 0 },
      { key: 'max', label: 'Max Height', min: 1, max: 200, step: 1, defaultValue: 60 },
    ],
  },
  curve: {
    type: 'curve',
    label: 'Curves',
    description: 'Custom height remap over the terrain\'s own height range, like Photoshop curves. Drag the points; click the curve to add one, double-click a point to remove it.',
    // No sliders: this layer is edited through the curve widget, which writes
    // layer.curve. The engine has always taken arbitrary control points.
    params: [],
  },
  blur: {
    type: 'blur',
    label: 'Blur',
    description: 'Smooths the terrain (separable ~gaussian). Pair with a slope mask to soften only the flats.',
    params: [
      { key: 'radius', label: 'Radius', min: 1, max: 10, step: 1, defaultValue: 2 },
      { key: 'strength', label: 'Strength', min: 0, max: 1, step: 0.05, defaultValue: 1 },
    ],
  },
  sharpen: {
    type: 'sharpen',
    label: 'Sharpen',
    description: 'Unsharp mask — amplifies ridgelines and surface detail.',
    params: [
      { key: 'radius', label: 'Radius', min: 1, max: 10, step: 1, defaultValue: 2 },
      { key: 'strength', label: 'Strength', min: 0.1, max: 3, step: 0.1, defaultValue: 0.8 },
    ],
  },
  transform: {
    type: 'transform',
    label: 'Transform',
    description: 'Vertical scale, height offset, and terrain inversion.',
    params: [
      { key: 'scale', label: 'V-Scale', min: 0.25, max: 3, step: 0.05, defaultValue: 1 },
      { key: 'offset', label: 'Offset', min: -30, max: 30, step: 1, defaultValue: 0 },
      { key: 'invert', label: 'Invert', min: 0, max: 1, step: 1, defaultValue: 0, choices: ['Off', 'On'] },
    ],
  },
  snow: {
    type: 'snow',
    label: 'Snow',
    description: 'Accumulates snow above an altitude line, sheds it off steep faces, then settles and melts it. Renders as a separate snowpack on top of the ground.',
    params: [
      { key: 'snowLine', label: 'Snow Line', min: 0, max: 1, step: 0.05, defaultValue: 0.55 },
      { key: 'amount', label: 'Depth', min: 0.5, max: 30, step: 0.5, defaultValue: 6 },
      // Titan terrain is steep — a median slope near 60 degrees at default
      // params — so the shed angle needs to sit high for snow to hold at all.
      { key: 'maxSlopeDeg', label: 'Shed Angle', min: 20, max: 85, step: 1, defaultValue: 65 },
      { key: 'settlePasses', label: 'Settle Passes', min: 0, max: 32, step: 1, defaultValue: 8 },
      { key: 'melt', label: 'Melt', min: 0, max: 1, step: 0.05, defaultValue: 0.35 },
    ],
  },
  water: {
    type: 'water',
    label: 'Lakes',
    description: 'Priority-flood fill: every depression that cannot drain to the map edge fills with water, giving per-cell lake depth.',
    params: [],
  },
  volcano: {
    type: 'volcano',
    label: 'Volcano',
    description: 'Drops a stratovolcano onto the terrain — concave flanks, a jagged summit crater, barranca gullies, and a rim breached on one side for lava to pour through. The flanks rise out of whatever is already there. Add one layer per cone.',
    params: [
      // Position is normalized 0..1 across the grid, not in cells, so a
      // volcano stays where the user dropped it when the resolution changes.
      { key: 'x', label: 'Position X', min: 0, max: 1, step: 0.005, defaultValue: 0.5 },
      { key: 'y', label: 'Position Y', min: 0, max: 1, step: 0.005, defaultValue: 0.5 },
      // Wide and comparatively low. A cone whose height rivals its radius
      // reads as a blade, not a volcano — these defaults are what a click
      // on the terrain drops, so they have to look right untouched.
      { key: 'radius', label: 'Base Radius', min: 0.04, max: 0.5, step: 0.01, defaultValue: 0.28 },
      { key: 'height', label: 'Summit Height', min: 5, max: 300, step: 1, defaultValue: 55 },
      { key: 'craterRadius', label: 'Crater Size', min: 0.03, max: 0.5, step: 0.01, defaultValue: 0.16 },
      { key: 'craterDepth', label: 'Crater Depth', min: 0, max: 0.6, step: 0.01, defaultValue: 0.16 },
      { key: 'rimJaggedness', label: 'Rim Jaggedness', min: 0, max: 1, step: 0.05, defaultValue: 0.6 },
      { key: 'roughness', label: 'Surface Detail', min: 0, max: 1, step: 0.05, defaultValue: 0.5 },
      // 1.75 is a stratovolcano; near 1 flattens toward a shield.
      { key: 'coneExponent', label: 'Flank Profile', min: 0.8, max: 3, step: 0.05, defaultValue: 1.75, advanced: true },
      // -1 lets the seed choose, so a field of volcanoes does not all breach
      // in the same direction.
      { key: 'breachAngle', label: 'Breach Bearing', min: -1, max: 360, step: 1, defaultValue: -1, advanced: true },
      { key: 'breachWidth', label: 'Breach Width', min: 10, max: 140, step: 5, defaultValue: 46, advanced: true },
      { key: 'variant', label: 'Variant', min: 0, max: 99, step: 1, defaultValue: 0, advanced: true },
    ],
  },
  lava: {
    type: 'lava',
    label: 'Lava Flow',
    description: 'Erupts every volcano above it in the stack. Lava pools in the craters, spills through the breached rims, and runs downhill as channelled streams — chilling into basalt that diverts what follows. Put this after your volcanoes.',
    params: [
      { key: 'steps', label: 'Flow Length', min: 50, max: 2500, step: 50, defaultValue: 600 },
      { key: 'eruptionRate', label: 'Eruption Rate', min: 0.1, max: 8, step: 0.1, defaultValue: 1.5 },
      // Low = fluid basalt running out into long streams; high = stiff lava
      // piling into short, thick lobes near the vent.
      { key: 'viscosity', label: 'Viscosity', min: 0, max: 1, step: 0.05, defaultValue: 0.35 },
      { key: 'sustain', label: 'Eruption', min: 0, max: 1, step: 1, defaultValue: 1, choices: ['Single Burst', 'Continuous'] },
      { key: 'coolRate', label: 'Cooling', min: 0.0002, max: 0.02, step: 0.0002, defaultValue: 0.0015, advanced: true },
      { key: 'solidifyRate', label: 'Solidify Rate', min: 0.002, max: 0.2, step: 0.002, defaultValue: 0.02, advanced: true },
      { key: 'ventRadius', label: 'Vent Radius', min: 1, max: 20, step: 0.5, defaultValue: 2.5, advanced: true },
    ],
  },
  combine: {
    type: 'combine',
    label: 'Combine Import',
    description: 'Blends the imported heightmap into the terrain with a blend mode (no-op until a heightmap is imported).',
    params: [
      { key: 'blend', label: 'Blend', min: 0, max: 5, step: 1, defaultValue: 3, choices: BLEND_LABELS },
      { key: 'strength', label: 'Strength', min: 0.1, max: 2, step: 0.05, defaultValue: 1 },
      { key: 'alpha', label: 'Mix Alpha', min: 0, max: 1, step: 0.05, defaultValue: 0.5 },
    ],
  },
};

let nextLayerId = 1;

export function makeLayer(type: LayerType): Layer {
  const params: Record<string, number> = {};
  for (const p of LAYER_DEFS[type].params) params[p.key] = p.defaultValue;
  const layer: Layer = {
    id: `layer-${nextLayerId++}-${Math.random().toString(36).slice(2, 7)}`,
    type,
    enabled: true,
    params,
    mask: defaultMask(),
  };
  if (type === 'curve') layer.curve = DEFAULT_CURVE.map(p => ({ ...p }));
  return layer;
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
    case 'lava': return Math.ceil(layer.params.steps / LAVA_STEPS_PER_CHUNK);
    default: return 1;
  }
}

// Fixed variant offset for noise-driven layer masks (deterministic, and out
// of the way of user-selectable layer variants).
const MASK_NOISE_SEED_OFFSET = 9001;

// Lava is run in slices so the eruption *animates* in the viewport instead of
// appearing fully formed after a freeze. The engine's flow is a plain
// accumulation over steps with no per-call state, so slicing it produces
// exactly the result one long call would — unlike hydraulic erosion, which
// needs whole rounds to compose.
const LAVA_STEPS_PER_CHUNK = 60;

// Activates the layer's mask on the engine. Returns true if a mask was set
// (caller clears it after the layer runs).
function activateMask(engine: TitanCore, layer: Layer): boolean {
  const mask = layer.mask;
  if (!mask || mask.mode === 0) return false;
  if (mask.mode === 4) {
    // Noise mask: rasterize a fractal field, then band it *in the engine* so
    // lo/hi/invert behave identically to the feature masks. This used to band
    // host-side, duplicating MaskByFeature's curve in TypeScript (and again in
    // Swift) — three copies of one formula that had to agree forever.
    engine.noiseToMask({
      seedOffset: MASK_NOISE_SEED_OFFSET, noiseType: 1, scale: 3,
      octaves: 5, persistence: 0.5, lacunarity: 2.0, warpStrength: 0,
    });
    engine.bandScratch(mask.lo, mask.hi, 0.05, mask.invert);
    engine.setMaskFromScratch();
  } else {
    engine.maskByFeature(mask.mode - 1, mask.lo, mask.hi, 0.05, mask.invert);
    engine.setMaskFromScratch();
  }
  return true;
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

  // Imported heightmap: additive base field (with a flat noise structure it
  // IS the base). Values are normalized 0..1, scaled by the height slider.
  if (project.imported && project.imported.data.length > 0) {
    engine.applyHeightfield(
      project.imported.data, project.imported.size,
      project.params.heightMultiplier, 0, 1.0
    );
  }

  doneUnits += 1;
  cb.onChunk();
  await yieldFrame();
  if (cb.shouldCancel()) return false;

  for (const layer of project.stack) {
    if (!layer.enabled) continue;
    const def = LAYER_DEFS[layer.type];
    const masked = activateMask(engine, layer);

    switch (layer.type) {
      case 'hydraulic': {
        const rounds = Math.ceil(layer.params.iterations / TitanCore.DROPLETS_PER_ROUND);
        const p = layer.params;
        for (let r = 0; r < rounds; ++r) {
          engine.erodeHydraulicEx(TitanCore.DROPLETS_PER_ROUND, {
            spawnMode: p.spawnMode, inertia: p.inertia, capacity: p.capacity,
            minCapacity: 0.01, dissolve: p.dissolve, deposit: p.deposit,
            evaporate: p.evaporate, gravity: p.gravity, lifetime: p.lifetime,
            radius: p.radius, bedrockSpeed: p.bedrockSpeed,
          });
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
          engine.erodeFluvialEx(1, {
            strength: layer.params.strength,
            erodeConstant: layer.params.erodeConstant,
            areaExponent: layer.params.areaExponent,
            slopeExponent: layer.params.slopeExponent,
            depositRatio: layer.params.depositRatio,
            maxStep: layer.params.maxStep,
          });
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
          engine.erodeThermalEx(step, layer.params.talusAngle, layer.params.rate,
                                layer.params.bedrockBreakdown);
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
      case 'noise':
        engine.applyNoise({
          seedOffset: layer.params.seedOffset,
          noiseType: LAYER_NOISE_IDS[layer.params.noiseType] ?? 1,
          scale: layer.params.scale,
          amplitude: layer.params.amplitude,
          octaves: layer.params.octaves,
          persistence: 0.5,
          lacunarity: 2.0,
          exponent: 1.0,
          warpStrength: 0,
          blendMode: layer.params.blend,
          blendAlpha: layer.params.alpha,
        });
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'gradient': {
        const size = project.params.size;
        const radial = layer.params.kind === 1;
        engine.applyStamp({
          shape: radial ? 5 : 4,
          centerX: size / 2,
          centerY: size / 2,
          // Linear u must reach ±1 at the corners under any rotation.
          sizeX: radial ? size / 2 : size * 0.71,
          sizeY: radial ? size / 2 : size * 0.71,
          rotationDeg: layer.params.angle,
          height: layer.params.height,
          falloff: 0.5,
          op: [0, 1, 3][layer.params.op] ?? 0,
        });
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      }
      case 'clamp':
        engine.applyClamp(layer.params.min, layer.params.max);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'curve': {
        const pts = layer.curve && layer.curve.length >= 2
          ? layer.curve
          : DEFAULT_CURVE;
        engine.applyCurve(pts.map(p => p.x), pts.map(p => p.y));
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      }
      case 'blur':
        engine.applyBlur(layer.params.radius, layer.params.strength);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'sharpen':
        engine.applySharpen(layer.params.radius, layer.params.strength);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'transform':
        engine.applyTransform(layer.params.scale, layer.params.offset, layer.params.invert === 1);
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'snow':
        engine.applySnow({
          snowLine: layer.params.snowLine,
          amount: layer.params.amount,
          maxSlopeDeg: layer.params.maxSlopeDeg,
          settlePasses: layer.params.settlePasses,
          melt: layer.params.melt,
        });
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'water':
        engine.computeWater();
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      case 'volcano': {
        const size = project.params.size;
        const p = layer.params;
        engine.applyVolcano({
          // Stored normalized so the cone stays put when resolution changes.
          centerX: p.x * size,
          centerY: p.y * size,
          radius: Math.max(2, p.radius * size),
          height: p.height,
          coneExponent: p.coneExponent,
          craterRadius: p.craterRadius,
          craterDepth: p.craterDepth,
          rimJaggedness: p.rimJaggedness,
          roughness: p.roughness,
          breachAngleDeg: p.breachAngle,
          breachWidthDeg: p.breachWidth,
          seedOffset: p.variant,
        });
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
      }
      case 'lava': {
        const p = layer.params;
        const total = Math.max(1, Math.round(p.steps));
        const continuous = p.sustain === 1;
        let done = 0;
        while (done < total) {
          const slice = Math.min(LAVA_STEPS_PER_CHUNK, total - done);
          engine.simulateLava({
            steps: slice,
            // A single burst erupts once, on the first slice. Continuous keeps
            // feeding the vent for the whole run.
            eruptionRate: continuous || done === 0 ? p.eruptionRate : 0,
            viscosity: p.viscosity,
            solidifyRate: p.solidifyRate,
            coolRate: p.coolRate,
            ventRadius: p.ventRadius,
            sustain: continuous,
          });
          done += slice;
          doneUnits += 1;
          tick(`${def.label} — ${done}/${total}`);
          cb.onChunk();
          await yieldFrame();
          if (cb.shouldCancel()) return false;
        }
        break;
      }
      case 'combine':
        if (project.imported && project.imported.data.length > 0) {
          engine.applyHeightfield(
            project.imported.data, project.imported.size,
            layer.params.strength * project.params.heightMultiplier,
            layer.params.blend, layer.params.alpha
          );
        }
        doneUnits += 1;
        tick(def.label);
        cb.onChunk();
        await yieldFrame();
        if (cb.shouldCancel()) return false;
        break;
    }

    if (masked) engine.setMask(null);
  }

  cb.onProgress(1, 'Done');
  return true;
}

// ---------------------------------------------------------------------------
// Project (de)serialization — the .titan file format.
// ---------------------------------------------------------------------------

function f32ToB64(a: Float32Array): string {
  const bytes = new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
  let bin = '';
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    bin += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(bin);
}

function b64ToF32(s: string): Float32Array {
  const bin = atob(s);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Float32Array(bytes.buffer);
}

export function serializeProject(project: TitanProject): string {
  const { imported, ...rest } = project;
  const out: any = { ...rest, version: requiredVersion(project) };
  if (imported && imported.data.length > 0) {
    out.imported = { size: imported.size, name: imported.name, dataB64: f32ToB64(imported.data) };
  }
  return JSON.stringify(out, null, 2);
}

// Base-parameter bounds. A .titan file is untrusted input — users share them —
// and these values go straight into titan_configure, which allocates
// size * size cells and loops octaves times per cell. `size: 100000` is a 40 GB
// allocation and an instant WASM abort; `octaves: 1e9` hangs the tab. Layer
// params were already clamped here; the base params were a bare cast.
// TitanLab has always validated these (EngineModel.swift), so this is also a
// parity fix.
const PARAM_BOUNDS = {
  // Must match the Resolution slider's range. This capped at 512 while the
  // slider went to 2048, so a project saved at 1024 silently reloaded at 512 —
  // and since TitanLab already allowed 2048, the same file reproduced two
  // different terrains depending on which app opened it.
  size: { min: 64, max: 2048, fallback: 128 },
  worldSize: { min: 64, max: 8192, fallback: 128 },
  scale: { min: 0.1, max: 20, fallback: 2 },
  heightMultiplier: { min: 1, max: 500, fallback: 40 },
  octaves: { min: 1, max: 12, fallback: 6 },
  persistence: { min: 0.05, max: 1, fallback: 0.5 },
  lacunarity: { min: 1, max: 4, fallback: 2 },
  exponent: { min: 0.1, max: 5, fallback: 1.2 },
  warpStrength: { min: 0, max: 3, fallback: 0.5 },
} as const;

const NOISE_TYPES: NoiseType[] = [
  'none', 'standard', 'ridged', 'billow', 'voronoi',
  'voronoiRidge', 'worleyManhattan', 'worleyChebyshev', 'hybrid',
];
const BIOMES: BiomeType[] = ['arctic', 'temperate', 'volcanic', 'desert'];

function sanitizeParams(raw: any, version: number): TerrainParams {
  const num = (key: keyof typeof PARAM_BOUNDS) => {
    const { min, max, fallback } = PARAM_BOUNDS[key];
    const value = raw?.[key];
    // JSON has no Infinity or NaN — JSON.stringify writes them as null — and
    // Number(null) is 0, which would silently land on the minimum instead of
    // the default. Treat missing/null as missing.
    if (value === null || value === undefined) return fallback;
    const v = Number(value);
    if (!Number.isFinite(v)) return fallback;
    return Math.min(max, Math.max(min, v));
  };

  const seed = typeof raw?.seed === 'string' ? raw.seed : String(raw?.seed ?? '');

  const size = Math.round(num('size') / 64) * 64;

  // A v1 project predates the world/detail split: its extent *was* its sample
  // count, so worldSize = size reproduces it exactly. Anything without a
  // usable worldSize gets the same treatment, which is the conservative
  // reading — it can only ever preserve the terrain the file described.
  const rawWorld = Number(raw?.worldSize);
  const worldSize = version >= 2 && Number.isFinite(rawWorld) && rawWorld > 0
    ? Math.min(PARAM_BOUNDS.worldSize.max,
               Math.max(PARAM_BOUNDS.worldSize.min, rawWorld))
    : size;

  return {
    // Resolution must land on the slider's 64-step grid.
    size,
    worldSize,
    scale: num('scale'),
    heightMultiplier: num('heightMultiplier'),
    seed: seed.slice(0, 128),
    octaves: Math.round(num('octaves')),
    persistence: num('persistence'),
    lacunarity: num('lacunarity'),
    exponent: num('exponent'),
    warpStrength: num('warpStrength'),
    noiseType: NOISE_TYPES.includes(raw?.noiseType) ? raw.noiseType : 'standard',
    biome: BIOMES.includes(raw?.biome) ? raw.biome : 'temperate',
  };
}

export function deserializeProject(json: string): TitanProject {
  const raw = JSON.parse(json);
  const version = Number(raw?.version);
  if (version !== 1 && version !== 2 && version !== 3) {
    throw new Error(
      Number.isFinite(version)
        ? `.titan version ${version} is newer than this build understands`
        : 'Not a valid .titan project file');
  }
  if (!raw.params || !Array.isArray(raw.stack)) {
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
      // Curve control points. A v2 file predates them, so its five fixed
      // y-values are lifted onto the same x positions they always used —
      // reproducing exactly the curve the file described.
      if (layer.type === 'curve') {
        const explicit = sanitizeCurve(l.curve);
        if (explicit) {
          layer.curve = explicit;
        } else {
          const ys = [l.params?.y0, l.params?.y1, l.params?.y2, l.params?.y3, l.params?.y4]
            .map(v => (Number.isFinite(Number(v)) ? Math.min(1, Math.max(0, Number(v))) : null));
          layer.curve = ys.every(v => v !== null)
            ? [0, 0.25, 0.5, 0.75, 1].map((x, i) => ({ x, y: ys[i] as number }))
            : DEFAULT_CURVE.map(p => ({ ...p }));
        }
      }
      if (l.mask && typeof l.mask === 'object') {
        const mode = Number(l.mask.mode);
        const lo = Number(l.mask.lo);
        const hi = Number(l.mask.hi);
        layer.mask = {
          mode: Number.isFinite(mode) ? Math.min(4, Math.max(0, Math.round(mode))) : 0,
          lo: Number.isFinite(lo) ? Math.min(1, Math.max(0, lo)) : 0,
          hi: Number.isFinite(hi) ? Math.min(1, Math.max(0, hi)) : 1,
          invert: l.mask.invert === true,
        };
      }
      return layer;
    });

  let imported: ImportedField | undefined;
  if (raw.imported?.dataB64 && Number.isFinite(Number(raw.imported.size))) {
    const size = Math.round(Number(raw.imported.size));
    const data = b64ToF32(String(raw.imported.dataB64));
    if (size >= 2 && size <= 4096 && data.length === size * size) {
      imported = { size, data, name: raw.imported.name ? String(raw.imported.name) : undefined };
    }
  }

  // Loaded projects are normalized to the current version; a v1 file that
  // gets re-saved carries its original world extent forward explicitly.
  return {
    version: TITAN_PROJECT_VERSION,
    params: sanitizeParams(raw.params, version),
    stack,
    imported,
  };
}

// ---------------------------------------------------------------------------
// Presets — each is a complete recipe: base params + stack. Seeds are filled
// in fresh at apply time (unless the user locked theirs).
// ---------------------------------------------------------------------------

export interface Preset {
  name: string;
  tagline: string;
  // Everything but the seed and the sample density. A preset pins worldSize
  // because its Height values are only meaningful relative to the extent they
  // were authored against — 62 units of volcano reads as a cone across 128
  // world units and as a pimple across 4096.
  params: Omit<TerrainParams, 'seed' | 'size'>;
  stack: Array<{
    type: LayerType;
    params?: Record<string, number>;
    mask?: LayerMask;
    curve?: CurvePoint[];
  }>;
}

export const PRESETS: Preset[] = [
  {
    name: 'Alpine Peaks',
    tagline: 'Ridged ranges with carved drainages and talus',
    params: {
      worldSize: 128,
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
      worldSize: 128,
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
      worldSize: 128,
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
      worldSize: 128,
      scale: 3.0, heightMultiplier: 25, octaves: 4, persistence: 0.45,
      lacunarity: 2.0, exponent: 1.0, warpStrength: 0.4,
      noiseType: 'billow', biome: 'desert',
    },
    stack: [
      { type: 'thermal', params: { passes: 20, talusAngle: 30, rate: 0.7 } },
    ],
  },
  {
    name: 'Worley Plateaus',
    tagline: 'Cellular mesas remapped by curves, cut by rivers',
    params: {
      worldSize: 128,
      scale: 1.6, heightMultiplier: 55, octaves: 5, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.0, warpStrength: 0.3,
      noiseType: 'voronoi', biome: 'desert',
    },
    stack: [
      // Was five fixed y-values on a quarter grid; the same shape as points.
      { type: 'curve', curve: [
        { x: 0, y: 0 }, { x: 0.25, y: 0.15 }, { x: 0.5, y: 0.55 },
        { x: 0.75, y: 0.85 }, { x: 1, y: 1 },
      ] },
      { type: 'noise', params: { noiseType: 0, blend: 0, scale: 6, amplitude: 6, octaves: 6, alpha: 0.5, seedOffset: 3 } },
      { type: 'fluvial', params: { passes: 2, strength: 1.2 } },
      {
        type: 'blur', params: { radius: 2, strength: 0.8 },
        mask: { mode: 2, lo: 0, hi: 0.15, invert: false }, // soften only the flats
      },
      { type: 'thermal', params: { passes: 8, talusAngle: 36, rate: 0.5 } },
    ],
  },
  {
    name: 'Erupting Stratovolcano',
    tagline: 'A breached cone pouring lava down to the sea',
    params: {
      worldSize: 128,
      scale: 2.2, heightMultiplier: 30, octaves: 6, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.3, warpStrength: 0.5,
      noiseType: 'standard', biome: 'volcanic',
    },
    stack: [
      // A radial ramp first: the island drains outward, which is what lets the
      // flows run all the way off the map instead of ponding at the cone's foot.
      { type: 'gradient', params: { kind: 1, op: 0, angle: 0, height: 26 } },
      // Wide and comparatively low: a cone whose height rivals its radius
      // reads as a spire, not a volcano. Real stratovolcanoes are far
      // broader than they are tall.
      { type: 'volcano', params: { x: 0.5, y: 0.5, radius: 0.36, height: 62 } },
      { type: 'thermal', params: { passes: 6, talusAngle: 36, rate: 0.4 } },
      { type: 'lava', params: { steps: 900, eruptionRate: 2.0, viscosity: 0.3 } },
    ],
  },
  {
    name: 'Volcanic Twins',
    tagline: 'Two vents whose flows collide and divert each other',
    params: {
      worldSize: 128,
      scale: 2.0, heightMultiplier: 26, octaves: 6, persistence: 0.5,
      lacunarity: 2.0, exponent: 1.2, warpStrength: 0.6,
      noiseType: 'standard', biome: 'volcanic',
    },
    stack: [
      { type: 'gradient', params: { kind: 1, op: 0, angle: 0, height: 28 } },
      { type: 'volcano', params: { x: 0.33, y: 0.42, radius: 0.26, height: 54, variant: 1, breachAngle: 25 } },
      { type: 'volcano', params: { x: 0.67, y: 0.60, radius: 0.22, height: 44, variant: 2, breachAngle: 205 } },
      { type: 'lava', params: { steps: 800, eruptionRate: 1.6, viscosity: 0.35 } },
    ],
  },
  {
    name: 'Volcanic Shield',
    tagline: 'A flattened caldera dome with radial gullies',
    params: {
      worldSize: 128,
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
    if (entry.mask) layer.mask = { ...entry.mask };
    if (entry.curve) layer.curve = entry.curve.map(p => ({ ...p }));
    return layer;
  });
  return { params: preset.params, stack };
}
