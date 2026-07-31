// .titan project format: round-trip fidelity and version migration.
//
// The format is the product's reproducibility promise — a file has to rebuild
// the same terrain, on either app, however old it is. That makes the migration
// path worth a test of its own: v2 split world extent from sample density, and
// a v1 file has to keep reproducing what its author saw.
//
//   npm run test:project
import {
  serializeProject, deserializeProject, TITAN_PROJECT_VERSION, makeLayer,
  requiredVersion,
} from '../src/core/pipeline';
import type { TerrainParams } from '../src/core/types';

console.log('=== .titan project format ===');

let fails = 0;
const check = (ok, label) => { console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label}`); if (!ok) fails++; };

const params: TerrainParams = {
  size: 1024, worldSize: 4096, scale: 3.1, heightMultiplier: 250, seed: 'héllo-🌋',
  octaves: 9, persistence: 0.55, lacunarity: 2.1, exponent: 1.4, warpStrength: 0.8,
  noiseType: 'ridged', biome: 'volcanic',
};
const stack = [makeLayer('volcano'), makeLayer('lava')];

// v2 round trip
// Stamped with the *minimum* version needed to read it, not the newest this
// build knows: this stack uses no custom curve, so it stays a valid v2 and
// still opens in a build that predates them.
const json = serializeProject({ version: 3, params, stack });
check(JSON.parse(json).version === 2,
      'a project using nothing newer is stamped v2, not the current version');
const back = deserializeProject(json);
for (const k of Object.keys(params)) {
  check(back.params[k] === params[k], `round-trips ${k} (${params[k]} -> ${back.params[k]})`);
}
check(back.stack.length === 2 && back.stack[0].type === 'volcano', 'round-trips the volcano stack');

// v1 migration: no worldSize in the file, extent was the sample count.
const v1 = JSON.stringify({
  version: 1,
  params: { size: 256, scale: 2, heightMultiplier: 40, seed: 'old', octaves: 6,
            persistence: 0.5, lacunarity: 2, exponent: 1.2, warpStrength: 0.5,
            noiseType: 'standard', biome: 'temperate' },
  stack: [],
});
const migrated = deserializeProject(v1);
check(migrated.params.worldSize === 256,
      `v1 gets worldSize = size (${migrated.params.worldSize}) so it reproduces exactly`);
check(migrated.version === TITAN_PROJECT_VERSION, 'v1 normalizes to the current version');

// A v1 file whose author had set a big resolution must not be downgraded.
const v1big = JSON.parse(v1); v1big.params.size = 1024;
const mb = deserializeProject(JSON.stringify(v1big));
check(mb.params.size === 1024, `v1 at 1024 stays 1024 (was clamped to 512)`);

// Unknown future version must be rejected, not silently misread.
// A file from a future build must be refused rather than mis-reproduced.
try { deserializeProject(JSON.stringify({ version: TITAN_PROJECT_VERSION + 1, params: {}, stack: [] })); check(false, 'rejects a newer version'); }
catch (e) { check(/newer than this build/.test((e as Error).message), `rejects a newer version: "${(e as Error).message}"`); }
try { deserializeProject('{"nope":1}'); check(false, 'rejects junk'); }
catch { check(true, 'rejects junk'); }

// --- v3: arbitrary curve control points --------------------------------
{
  const curved = makeLayer('curve');
  curved.curve = [
    { x: 0, y: 0 }, { x: 0.2, y: 0.55 }, { x: 0.6, y: 0.62 }, { x: 1, y: 1 },
  ];
  const proj = { version: 3 as const, params, stack: [curved] };

  check(JSON.parse(serializeProject(proj)).version === 3,
        'a project with a custom curve is stamped v3');
  // A project that uses nothing newer stays readable by older builds.
  check(requiredVersion({ version: 3, params, stack: [makeLayer('blur')] }) === 2,
        'a project without a custom curve is still stamped v2');

  const back = deserializeProject(serializeProject(proj));
  const got = back.stack[0].curve!;
  check(got.length === 4, `curve round-trips its point count (${got.length})`);
  check(got.every((p, i) => Math.abs(p.x - curved.curve![i].x) < 1e-9 &&
                            Math.abs(p.y - curved.curve![i].y) < 1e-9),
        'curve round-trips every control point exactly');

  // A v2 file predates control points: its five fixed y-values must be lifted
  // onto the x positions they always used, reproducing the same curve.
  const v2 = JSON.stringify({
    version: 2,
    params: { ...params, size: 128, worldSize: 128 },
    stack: [{ type: 'curve', enabled: true,
              params: { y0: 0, y1: 0.15, y2: 0.55, y3: 0.85, y4: 1 },
              mask: { mode: 0, lo: 0, hi: 1, invert: false } }],
  });
  const migrated = deserializeProject(v2).stack[0].curve!;
  const expected = [0, 0.15, 0.55, 0.85, 1];
  check(migrated.length === 5 &&
        migrated.every((p, i) => Math.abs(p.x - i * 0.25) < 1e-9 &&
                                 Math.abs(p.y - expected[i]) < 1e-9),
        'a v2 curve layer migrates to the same shape as points');

  // Out-of-order or out-of-range input must not reach the engine, whose
  // spline assumes strictly increasing x.
  const messy = JSON.parse(serializeProject(proj));
  messy.stack[0].curve = [{ x: 0.9, y: 2 }, { x: 0.1, y: -1 }, { x: 0.9, y: 0.5 }];
  const fixed = deserializeProject(JSON.stringify(messy)).stack[0].curve!;
  const sorted = fixed.every((p, i) => i === 0 || p.x > fixed[i - 1].x);
  const inRange = fixed.every(p => p.y >= 0 && p.y <= 1 && p.x >= 0 && p.x <= 1);
  check(sorted && inRange && fixed[0].x === 0 && fixed[fixed.length - 1].x === 1,
        'a malformed curve is sorted, clamped and pinned to the full range');
}

console.log(fails === 0 ? '\nALL PASSED' : `\n${fails} FAILED`);
process.exit(fails ? 1 : 0);
