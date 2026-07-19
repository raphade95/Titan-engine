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

- **Deterministic by contract**: same seed → bit-identical terrain, on every
  platform, including after all erosion passes (PCG32 streams, no fast-math).
- Seeded 2D simplex noise with fBm, billow, and true Musgrave **ridged
  multifractal** (octave feedback weighting).
- **Domain warping** for tectonic-looking landforms.
- Two-layer ground model (bedrock + sediment) with strata-based hardness.
- **Hydraulic erosion** — Lagrangian droplet simulation.
- **Thermal weathering** — angle-of-repose talus creep, double-buffered and
  exactly mass-conserving.
- **Fluvial erosion** — priority-flood sink filling, D8 flow accumulation,
  stream-power law (`E = K·Aᵐ·Sⁿ`) for connected river networks.
- World-space noise sampling: adjacent tiles are seamless (chunking-ready).
- Exports: 16-bit RAW heightmap (`.r16`), 8-bit PNG, RGBA splatmap PNG.

## Running the web lab

Prerequisites: Node.js. (The WASM module is prebuilt and checked in.)

```bash
npm install
npm run dev        # http://localhost:3000
```

## Rebuilding the engine

Prerequisites: CMake + a C++20 compiler; Emscripten for the WASM target.

```bash
npm run test:core    # native build + test harness (determinism, mass conservation, seams)
npm run build:wasm   # rebuild src/wasm/titan_core.js for the viewer
```

Run the native tests before shipping any engine change — determinism and
mass conservation are product guarantees, not implementation details.
