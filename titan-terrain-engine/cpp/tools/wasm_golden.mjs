// Golden checks against the WASM module the web lab actually ships.
//
// The native titan_golden binary cannot see src/wasm/titan_core.js, which is
// a checked-in build artifact — so without this, the one engine build that
// end users on the free tier actually run is the only one never verified.
// This re-runs the layer-1 (integer, strict) checks plus a terrain
// fingerprint through the WASM C API.
//
//     node cpp/tools/wasm_golden.mjs
//
// Constants below are the same ones in tests/test_golden.cpp. Layer 1 must
// match exactly — it is pure integer arithmetic. The terrain fingerprint is
// toleranced, because Emscripten's libm is a fourth implementation alongside
// Apple's, glibc's, and MSVC's (see the header comment in test_golden.cpp).

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const modulePath = resolve(here, '../../src/wasm/titan_core.js');

const FNV_OFFSET = 0xcbf29ce484222325n;
const FNV_PRIME = 0x100000001b3n;
const MASK64 = (1n << 64n) - 1n;

const hashU32 = (v, h) => {
  for (let i = 0; i < 4; i++) {
    h ^= BigInt((v >>> (8 * i)) & 0xff);
    h = (h * FNV_PRIME) & MASK64;
  }
  return h;
};

const hashU64 = (v, h) => {
  for (let i = 0; i < 8; i++) {
    h ^= (v >> BigInt(8 * i)) & 0xffn;
    h = (h * FNV_PRIME) & MASK64;
  }
  return h;
};

// --- expected values -------------------------------------------------------
//
// Parsed straight out of tests/test_golden.cpp rather than copied. Two
// hand-maintained copies of the same constant is precisely how this file went
// wrong the first time: it carried the correct FNV-1a offset basis while the
// C++ carried one with a dropped digit, and the disagreement read as an engine
// bug until both were traced back. One definition, one place.

const goldenSrc = readFileSync(resolve(here, '../tests/test_golden.cpp'), 'utf8');

const parseHash = (name) => {
  const m = goldenSrc.match(new RegExp(`constexpr uint64_t ${name}\\s*=\\s*0x([0-9a-fA-F]+)ull`));
  if (!m) throw new Error(`could not find ${name} in test_golden.cpp`);
  return BigInt('0x' + m[1]);
};

// First three fingerprint entries of a Recipe are mean, min, max.
const parseFingerprint = (recipe) => {
  const m = goldenSrc.match(
    new RegExp(`const Recipe ${recipe} = \\{[^}]*?\\{([^}]*)\\}`, 's'));
  if (!m) throw new Error(`could not find ${recipe} in test_golden.cpp`);
  const nums = m[1].split(',').map((v) => Number(v.trim()));
  return { mean: nums[0], min: nums[1], max: nums[2] };
};

const parseTolerance = (recipe) => {
  const m = goldenSrc.match(
    new RegExp(`const Recipe ${recipe} = \\{.*?\\},\\s*\\n\\s*([0-9.eE+-]+),`, 's'));
  if (!m) throw new Error(`could not find tolerance for ${recipe}`);
  return Number(m[1]);
};

const EXPECTED_SEED_HASH = parseHash('kG_SeedHash');
const EXPECTED_BASE = parseFingerprint('kR_BaseNoise');
const TOLERANCE = parseTolerance('kR_BaseNoise');

let failures = 0;
const check = (ok, label, detail = '') => {
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${label}${detail ? '  ' + detail : ''}`);
  if (!ok) failures++;
};

const createTitanModule = (await import(modulePath)).default;
const m = await createTitanModule();

console.log('=== WASM golden checks ===');
console.log(`module: ${m.UTF8ToString(m._titan_version())}, api v${m._titan_api_version()}\n`);

// --- API surface -----------------------------------------------------------
// A stale checked-in titan_core.js is the failure mode this catches: the web
// lab would call a function the module does not export and die at runtime.
console.log('Exported C API surface:');
const required = [
  '_titan_create', '_titan_destroy', '_titan_configure', '_titan_generate',
  '_titan_hash_seed', '_titan_api_version', '_titan_apply_clamp',
  '_titan_apply_curve', '_titan_apply_blur', '_titan_apply_sharpen',
  '_titan_apply_transform', '_titan_apply_heightfield', '_titan_mask_by_feature',
  '_titan_noise_to_mask', '_titan_set_mask_from_scratch', '_titan_apply_noise',
  '_titan_apply_stamp', '_titan_export_png16', '_titan_export_r16',
  '_titan_export_normal_png', '_titan_export_ao_png', '_titan_build_mesh',
  '_titan_height_range', '_titan_mass_balance', '_titan_band_scratch',
  '_titan_export_splat_png', '_titan_build_mesh_lod', '_titan_last_error',
];
const missing = required.filter((fn) => typeof m[fn] !== 'function');
check(missing.length === 0, 'all required exports present',
      missing.length ? `missing: ${missing.join(', ')}` : `(${required.length} checked)`);

// Runtime helpers the TS wrapper depends on.
const helpers = ['stringToUTF8', 'lengthBytesUTF8', 'UTF8ToString', 'HEAPF32', 'HEAPU32', 'HEAPU8'];
const missingHelpers = helpers.filter((h) => m[h] === undefined);
check(missingHelpers.length === 0, 'runtime helpers exported',
      missingHelpers.length ? `missing: ${missingHelpers.join(', ')}` : '');

// --- Layer 1: seed hashing (strict) ---------------------------------------
console.log('\nLayer 1 - integer determinism (strict):');
const hashSeed = (s) => {
  const bytes = m.lengthBytesUTF8(s) + 1;
  const ptr = m._malloc(bytes);
  try {
    m.stringToUTF8(s, ptr, bytes);
    return m._titan_hash_seed(ptr) >>> 0;
  } finally {
    m._free(ptr);
  }
};

{
  const seeds = ['', 'a', 'titan', 'alpine-7', '0123456789',
                 'café', '山', '\u{1f3d4}', 'señor-montaña'];
  let h = FNV_OFFSET;
  for (const s of seeds) h = hashU32(hashSeed(s), h);
  h = hashU32(m._titan_hash_seed(0) >>> 0, h); // NULL
  check(h === EXPECTED_SEED_HASH, 'titan_hash_seed (utf-8)',
        `${h.toString(16).padStart(16, '0')}` +
        (h === EXPECTED_SEED_HASH ? '' : ` want ${EXPECTED_SEED_HASH.toString(16)}`));
}

// --- Layer 2: terrain fingerprint (toleranced) ----------------------------
console.log('\nLayer 2 - terrain fingerprint (toleranced):');
{
  const handle = m._titan_create();
  const size = 128;
  let sum = 0, min = Infinity, max = -Infinity;

  for (let type = 0; type <= 8; type++) {
    m._titan_configure(handle, size, 1.0, 2.0, 40.0, 2024 + type, 6,
                       0.5, 2.0, 1.2, type, 0.6, 1.0, 2.0, 0.0, 0.0);
    m._titan_generate(handle);
    const bed = m._titan_bedrock_ptr(handle) >> 2;
    const sed = m._titan_sediment_ptr(handle) >> 2;
    for (let i = 0; i < size * size; i++) {
      const h = m.HEAPF32[bed + i] + m.HEAPF32[sed + i];
      sum += h;
      if (h < min) min = h;
      if (h > max) max = h;
    }
  }
  m._titan_destroy(handle);

  const mean = sum / (9 * size * size);
  const rel = (a, b) => Math.abs(a - b) / Math.max(1, Math.abs(a), Math.abs(b));
  const drift = Math.max(rel(mean, EXPECTED_BASE.mean),
                         rel(min, EXPECTED_BASE.min),
                         rel(max, EXPECTED_BASE.max));
  check(drift <= TOLERANCE, 'base fields, all 9 noise types',
        `drift ${drift.toExponential(2)} (bound ${TOLERANCE.toExponential(0)})`);
}

console.log(`\n${failures === 0 ? 'WASM GOLDEN CHECKS PASSED' : 'WASM GOLDEN CHECKS FAILED'} ` +
            `(${failures} failure${failures === 1 ? '' : 's'})`);
process.exit(failures === 0 ? 0 : 1);
