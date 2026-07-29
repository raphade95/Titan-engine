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
const json = serializeProject({ version: 2, params, stack });
check(JSON.parse(json).version === TITAN_PROJECT_VERSION, `serializes as version ${TITAN_PROJECT_VERSION}`);
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
try { deserializeProject(JSON.stringify({ version: 3, params: {}, stack: [] })); check(false, 'rejects a newer version'); }
catch (e) { check(/newer than this build/.test(e.message), `rejects a newer version: "${e.message}"`); }
try { deserializeProject('{"nope":1}'); check(false, 'rejects junk'); }
catch { check(true, 'rejects junk'); }

console.log(fails === 0 ? '\nALL PASSED' : `\n${fails} FAILED`);
process.exit(fails ? 1 : 0);
