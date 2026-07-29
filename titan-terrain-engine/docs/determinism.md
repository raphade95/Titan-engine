# Determinism: what Titan guarantees, and what it does not

Determinism is the product's central claim — a seed is a reproduction recipe,
and a `.titan` project is only meaningful if that recipe resolves the same way
for everyone. This document states precisely how far that holds, because the
honest answer is more nuanced than "same seed, same terrain".

## The guarantee

| Scope | Guarantee | Enforced by |
|---|---|---|
| Same build, same machine | **Bit-identical**, including after every erosion pass | `test_core.cpp` (`TestDeterminism`) |
| Same build, any thread count | **Bit-identical** — droplet batching merges in fixed order | `test_core.cpp` (`TestChunkedErosionEquivalence`) |
| Same build, any optimization level | **Bit-identical** | verified `-O0`/`-O2`/`-O3` |
| Same architecture, different compiler | **Bit-identical** for integer paths; expected bit-identical for terrain | `titan_golden` layer 1 (strict), layer 2 (exact on reference) |
| **Different architecture or platform** | **Visually identical, not bit-identical** | `titan_golden` layer 2 (toleranced) |

Seeds, RNG streams, and permutation tables are bit-identical **everywhere**,
without qualification. It is the floating-point terrain fields that carry the
caveat below.

## Why terrain is not bit-identical across platforms

IEEE-754 pins down `+ - * /` and `sqrt` to a single correctly-rounded result,
and the build pins fast-math and FP contraction off (see below), so ordinary
arithmetic is portable. `libm` is not. `pow`, `sin`, `tan`, `exp`, `log`, and
`tanh` are permitted to differ in the last unit in the last place between
implementations, and they do — measured on one machine, one compiler, one set
of flags, varying only the target architecture:

```
powf    arm64 50a532a50c8a1dd5    x86_64 55d2dfcb015fdf8b
sinf    arm64 dfff5ed94a50b528    x86_64 0defb67c746feb4f
expf    arm64 f76cabea1318260e    x86_64 52c0c6c7e8852f29
logf    arm64 0cc764e52d88ecda    x86_64 2a25e618673cb4b6
tanhf   arm64 b22978cf7ad67505    x86_64 4febbba92071c63c
tanf    arm64 86d26b83f1581423    x86_64 ad842b5c786c26ea
sqrtf   identical (IEEE-mandated)
atanf   identical (in practice)
```

Height shaping calls `pow()` on every cell, so this reaches essentially every
terrain. Titan ships against at least four libm implementations — Apple's,
glibc, MSVC's CRT, and Emscripten's — so a single bit-exact expectation cannot
hold across them.

### How large is the difference in practice?

Measured arm64 vs x86_64, identical source and flags, on a 128×128 terrain
with a 25-unit height range:

| Stage | Mean \|Δh\| | Worst cell | Cells over 1% of range |
|---|---|---|---|
| Base noise only | 9.4e-09 | 0.00001 (0.00%) | 0 of 16384 |
| After hydraulic + thermal + fluvial | 6.9e-04 | 0.312 (1.23%) | 2 of 16384 |

Base generation is identical to well within a rendering pixel. Erosion is
**chaotic**: a last-ulp height difference can flip which neighbour a droplet
descends into, and that droplet's path diverges from there. The visible effect
is bounded and rare, but it is not zero.

**What this means for users:** a seed produces the same terrain everywhere in
every way a person can perceive. It does not produce byte-identical `.r16`
exports across platforms, so do not build a workflow on hashing exports for
equality across machines.

## Build flags that are load-bearing

```cmake
-fno-fast-math -ffp-contract=off      # clang / gcc
/fp:precise /fp:contract-             # MSVC
```

`-fno-fast-math` alone is **not** sufficient. It leaves floating-point
contraction enabled, so `a * b + c` may be fused into a single-rounding FMA
wherever the target has one. arm64 fuses; baseline x86_64 cannot. This was a
real defect, not a hypothetical: because `CMAKE_OSX_ARCHITECTURES` builds a
universal binary, the two slices of the *same shipping macOS app* generated
different terrain from the same seed. Both flags must stay, in
`cpp/CMakeLists.txt` and `cpp/build_wasm.sh` alike.

For the same reason, prefer explicit arithmetic over `pow` with a constant
exponent: `pow(x, 2.0f)` is strength-reduced to `x * x` at `-O2` and above but
not at `-O0`, and the two are not always bit-identical, which made results
depend on the optimization level.

## How this is tested

- **`cpp/tests/test_core.cpp`** — behavioural guarantees; a build agrees with
  itself.
- **`cpp/tests/test_golden.cpp`** (`titan_golden`) — checked-in constants; every
  build agrees with the same numbers. Layer 1 (integer) is strict everywhere.
  Layer 2 (terrain) is exact on the reference configuration and toleranced
  elsewhere, with per-recipe bounds sized from measured drift.
- **`cpp/tools/mutation_test.sh`** — reintroduces each determinism defect and
  asserts `titan_golden` catches it. Mutations that are provably output-neutral
  are declared as such, so the file also documents which orderings genuinely
  do not matter.
- **`cpp/tools/wasm_golden.mjs`** — runs the same checks against the checked-in
  `src/wasm/titan_core.js`, the engine build free-tier users actually run.

All four run in CI, on macOS, Linux, and Windows.

### Adding an engine feature

If it changes terrain output, `titan_golden` will fail on the reference
configuration. That is the intended signal. Confirm the change is what you
meant, then regenerate:

```bash
./build/titan_golden --print    # on macOS arm64
```

Never regenerate to turn a red build green without understanding why it went
red.

## Open items (for later review)

**Layer 2 is only strict on one configuration.** Exact terrain hashes are
enforced on macOS arm64; Linux and Windows run the toleranced fingerprint
instead. A genuine regression that appears *only* on Linux or Windows and
stays under the per-recipe bound would pass CI unnoticed.

Current mitigations, in descending order of strength:

- Layer 1 (integer: seeds, RNG streams, permutation tables) is strict on every
  platform, so anything touching seeding is caught everywhere.
- Bounds sit 50x or more below measured cross-architecture drift, so the
  window a regression could hide in is narrow.
- `titan_golden --require-reference` runs on the macOS CI job and fails loudly
  if that runner ever stops being Apple arm64, rather than silently
  downgrading to the toleranced path with nothing left checking bits.

The window closes entirely once the libm work below is done, at which point
every platform can enforce exact hashes. Until then, treat a Linux- or
Windows-only terrain regression as a class of bug CI can miss.

## Making terrain bit-identical everywhere

This is achievable, and it is the natural next step if byte-identical exports
across platforms become a product requirement (for example, for asset pipelines
that cache on content hashes).

It requires replacing the libm calls on the terrain path with in-house,
correctly-specified implementations. The full list of call sites is small:

| Function | Where |
|---|---|
| `pow` | height shaping (`GenerateHeightmap`, `ApplyNoise`, `NoiseToScratch`), terrace sharpness, fluvial area/slope exponents |
| `sin` | `HardnessAt` strata bands, stamp rotation |
| `cos` | stamp rotation |
| `tan` | thermal talus angle, snow repose angle |
| `exp` | crater stamp profile |
| `tanh` | `ApplyPlateau` shoulder, curvature mask |
| `atan` | slope mask, AO export |
| `log` | fluvial flow map scaling |

Most are called once per operation for setup (`tan`, `cos`, `sin` for rotation)
rather than per cell, so the per-cell surface is narrower than the table
suggests: `pow`, `sin` (strata), `tanh`, `atan`, `exp`, and `log`.

Once those are deterministic, layer 2 of `titan_golden` can be promoted from
toleranced to strict on every platform, and the guarantee table at the top
collapses to a single row.
