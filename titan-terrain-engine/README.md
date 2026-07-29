# Titan Terrain Engine

A cross-platform procedural terrain engine built to close the macOS gap left
by Gaea and World Machine, with a simpler learning curve than Houdini.

## Architecture

All terrain math lives in **one C++ library** compiled to multiple targets:

| Component | What it is | Status |
|---|---|---|
| `libTitanCore` (cpp/) | Pure C++20 engine: noise, erosion, exporters, mesh generation. Deterministic and multithreaded. | **Active** |
| **Web Lab** (src/) | React + Three.js app running the engine as WASM: layer stack, presets, .titan projects. Free tier. | **Active** |
| **TitanLab (macOS)** (macos/) | SwiftUI + Metal native app linking libTitanCore directly. `./macos/build_app.sh --smoke` | **Active** |
| **TitanBridge** (unreal/) | Unreal Engine plugin (Fab): in-editor generation via `ATitanTerrainActor`. | **Active** |
| Launch kit (docs/, site/) | Docs, marketing site, pricing/EULA drafts, launch checklists. | Drafted |

The viewer contains **zero terrain math** — [src/core/TitanCore.ts](src/core/TitanCore.ts)
only marshals parameters in and copies buffers out across the C API
([cpp/libTitanCore/include/TitanCAPI.h](cpp/libTitanCore/include/TitanCAPI.h)).

## Engine features

- **Deterministic by contract**: same seed → the same terrain, including after
  all erosion passes. Bit-identical for a given platform and architecture,
  regardless of thread count, chunking, or optimization level; visually
  identical across platforms, to within a fraction of a rendering pixel on base
  terrain. The one caveat is that `libm` (`pow`, `sin`, `exp`, …) is not
  bit-portable, so exports are not byte-identical across architectures —
  [docs/determinism.md](docs/determinism.md) states exactly what holds, how it
  is measured, and what it would take to close the gap. Enforced in CI by
  `titan_golden`, which is itself mutation-tested.
- Seeded 2D simplex noise with fBm, billow, and true Musgrave **ridged
  multifractal** (octave feedback weighting).
- **Domain warping** for tectonic-looking landforms.
- Two-layer ground model (bedrock + sediment) with strata-based hardness.
- **Hydraulic erosion** — Lagrangian droplet simulation.
- **Thermal weathering** — angle-of-repose talus creep, double-buffered and
  exactly mass-conserving.
- **Fluvial erosion** — priority-flood sink filling, D8 flow accumulation,
  stream-power law (`E = K·Aᵐ·Sⁿ`) for connected river networks.
- **Volcanoes** — a stratovolcano edifice with concave-up flanks, a jagged
  summit crater, radial barranca gullies, and a rim breached on one bearing.
  Flanks union with the terrain while the crater cuts into it, so a cone
  dropped on a mountainside merges with it. Place as many as you like; each
  registers an eruption vent.
- **Lava flow** — a cellular flow whose yield strength rises as it cools. That
  one rule is what makes it lava rather than water: it pools in craters until
  it finds the breach, piles into lobes, freezes levees along its chilled
  margins, and self-channelizes into streams that run to the map edge. Chilled
  lava becomes bedrock, so it diverts later flows and lands in every export.
- **World size and resolution are separate.** `worldSize` is how far the terrain
  spans; `size` is how finely it is sampled. Both apps used to pin the engine's
  `cellSize` to 1.0, which made the extent equal to the pixel count — the same
  seed came out 6.7x flatter at 1024 than at 128 because raising "Resolution"
  silently widened the world while Height stayed absolute. Refining the grid now
  resolves more detail on the same landform.
- World-space noise sampling: adjacent tiles are seamless (chunking-ready).
- **Cellular noise family**: voronoi F1/F2-F1 plus Worley manhattan/chebyshev
  metrics, and a Musgrave hybrid multifractal "terrain" mode.
- **Filters**: clamp, curves (monotone-cubic height remap), blur, sharpen,
  vertical transform (scale/offset/invert) — all mask-aware.
- **Masking**: per-operation masks generated from height/slope/curvature bands
  or fractal noise (`titan_mask_by_feature`, `titan_noise_to_mask`).
- **Combiner / import**: blend an external heightfield (add/sub/mul/max/min/mix)
  with engine-side resampling — also powers .png/.r16/.r32 heightmap import.
- Gradient generators (linear/radial stamps), full slope & curvature maps.
- Exports: 16-bit RAW heightmap (`.r16`), 16-bit PNG, float32 RAW/EXR, OBJ
  mesh, RGBA splatmap PNG, world-space normal map PNG, ambient-occlusion PNG.
- 2D top-down hypsometric map view in both the web lab and TitanLab (macOS).
- Drop-a-volcano placement in both viewports: press on the terrain to place a
  cone, drag to position it. Lava is rendered from a per-vertex attribute the
  engine fills — incandescence by temperature, a flow-aligned crust that drifts
  downhill, and emission that lights the rock the stream runs past.

## Running the web lab

Prerequisites: Node.js. (The WASM module is prebuilt and checked in.)

```bash
npm install
npm run dev        # http://localhost:3000
```

## Rebuilding the engine

Prerequisites: CMake + a C++20 compiler; Emscripten for the WASM target.

```bash
npm run test:core     # behavioural harness (determinism, mass conservation, seams)
npm run test:golden   # checked-in golden hashes — the cross-platform contract
npm run test:mutation # proves the golden harness can still catch regressions
npm run build:wasm    # rebuild src/wasm/titan_core.js for the viewer
npm run test:wasm     # golden checks against the WASM the web lab ships
```

Run the native tests before shipping any engine change — determinism and mass
conservation are product guarantees, not implementation details.

If `titan_golden` fails after an intentional engine change, that is the signal
working. Confirm the change is what you meant, then regenerate the constants
on the reference configuration (macOS arm64) with `./build/titan_golden
--print`. See [docs/determinism.md](docs/determinism.md).
