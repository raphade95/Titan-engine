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
  resolves more detail on the same landform, and the simulation passes measure
  in world units too — hydraulic, fluvial, blur and thermal all agree across a
  doubling of the grid. Thermal creep is one cell per pass by construction, so
  the pass count is scaled by the cell size to cover the same world distance;
  the multiplier is capped at 16x so a fine grid cannot silently turn a cheap
  layer into a minutes-long one.
- World-space noise sampling: adjacent tiles are seamless (chunking-ready).
- **Cellular noise family**: voronoi F1/F2-F1 plus Worley manhattan/chebyshev
  metrics, and a Musgrave hybrid multifractal "terrain" mode.
- **Filters**: clamp, curves, blur, sharpen, vertical transform
  (scale/offset/invert) — all mask-aware.
- **Curve editor**: a draggable monotone-cubic height remap with up to 12
  control points, in both apps. The preview is drawn from the engine's own
  sampler (`titan_sample_curve`) rather than a spline reimplemented per host, so
  it cannot disagree with the result — verified to 1.2e-07 against the applied
  remap.
- **Node graph (TitanLab)**: the layer stack, but able to fork and rejoin —
  two treatments of the same base terrain merged by a Combine node, or any
  node's output used as another's mask. Nodes run the *same* engine calls the
  stack's layers do; a graph that is still a straight line converts back into
  a stack. Opens in a drawer under the viewport (⌥⌘G).
- **Masking**: per-operation masks generated from height/slope/curvature bands
  or fractal noise (`titan_mask_by_feature`, `titan_noise_to_mask`).
- **Combiner / import**: blend an external heightfield (add/sub/mul/max/min/mix)
  with engine-side resampling — also powers .png/.r16/.r32 heightmap import.
- **Real-world elevation import**: GeoTIFF/TIFF (uint8/16/32, int16/32, float32/64,
  strips or tiles, LZW / DEFLATE / PackBits, horizontal predictor) and SRTM `.hgt`,
  decoded in the engine so every host reads a given file identically. The true
  elevation range is reported alongside the normalized field, so you can set an
  importing tool's Z scale from real metres.
- Gradient generators (linear/radial stamps), full slope & curvature maps.
- Exports: 16-bit RAW heightmap (`.r16`), 16-bit PNG, float32 RAW/EXR, OBJ
  mesh, RGBA splatmap PNG, world-space normal map PNG, ambient-occlusion PNG.
  The normalizing formats trim outlier sediment towers rather than spending most
  of the 16-bit depth on the gap between the terrain and a few pixels — an
  unsettled hydraulic pass went from 36% usable range to 81%, while terrain
  whose maximum is a real summit exports untouched.
- **Tiled export** for large worlds (Unreal World Partition): an N×N grid of
  heightmaps sliced from one simulation and sharing one height range, so the set
  reassembles into the single-file export bit for bit. Generating tiles
  independently would seam — world-space noise lines up exactly, but erosion is
  not local, and separately eroded tiles disagree by up to 4.2% of the relief
  along their shared edge.
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
npm run test:project  # .titan round-trip fidelity and version migration
```

Run the native tests before shipping any engine change — determinism and mass
conservation are product guarantees, not implementation details.

If `titan_golden` fails after an intentional engine change, that is the signal
working. Confirm the change is what you meant, then regenerate the constants
on the reference configuration (macOS arm64) with `./build/titan_golden
--print`. See [docs/determinism.md](docs/determinism.md).
