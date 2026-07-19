# Titan Roadmap — from v0.2.0 to shipping product

## Where we are (July 2026)

**Done:**
- `libTitanCore` (C++20): seeded simplex/fBm/ridged/billow noise, domain
  warping, droplet hydraulic erosion, mass-conserving thermal weathering,
  stream-power fluvial erosion, mesh generation, carve brush, flat C API.
- Determinism, mass-conservation, and tile-seam guarantees enforced by a
  17-check native test harness (`npm run test:core`).
- Web lab (React/Three.js) running the engine as WASM: flat-canvas start,
  fresh-seed/lock-seed workflow, hydraulic + thermal + fluvial UI,
  `.r16` / 8-bit PNG / splatmap exports.

**Not started:** native Mac app, Unreal plugin, GPU compute, multithreading,
presets, undo, layer stack, high-bit-depth/mesh exports, Windows build,
licensing, website.

---

## Phase 0 — Engine hardening (foundation for everything)

Goal: the core is fast, tunable, and API-stable enough to build two frontends on.

1. **Multithreading.** Thread-pool droplet batches with private delta buffers,
   merged single-threaded (the lock-free plan from the design doc). Keep
   `std::jthread`/standard C++ only — no GCD in the core (portability).
   Target: 1024², 200k droplets + fluvial in **< 1 s** on an M-series Mac.
2. **Benchmark harness** alongside the tests so perf regressions are visible.
3. **Visual tuning pass.** Splat thresholds (terrain currently reads too
   rocky), fluvial constants, hardness curve, better shipped defaults.
   Tune in the web lab — it's the fastest loop.
4. **Droplet spawn distributions.** Uniform / elevation-weighted /
   precipitation-map (opens the door to rain-shadow effects later).
5. **Shaping modifiers.** Terrace, plateau/clamp, strata displacement —
   cheap wins that fill out the "toolbox" feel.
6. **Export formats in core.** 16-bit PNG, 32-bit EXR (tinyexr), raw float32;
   OBJ/glTF mesh export. Blender story depends on this.
7. **Freeze the C API as v1** (versioned; param structs instead of long
   argument lists). Swift and Unreal both consume it — breaking it later is
   expensive.
8. **Repo hygiene: `git init` + GitHub + CI** (macOS/Linux/Windows matrix
   running the test harness). Windows CI matters *now* — most Unreal
   customers are on Windows, and MSVC determinism must be proven early,
   not discovered broken at plugin time.

**Exit criteria:** perf target met; tests green on all three OSes; C API tagged v1.

## Phase 1 — Web lab as flagship demo + UX proving ground

Goal: the browser app becomes the free tier / marketing funnel, and the place
the product UX gets decided.

1. **Move the engine into a Web Worker** with progress callbacks — no UI
   freeze at high iteration counts; progress bars during erosion.
2. **Layer stack v1** (the product's core UX bet: simpler than node graphs).
   Ordered stack: Base Noise → Warp → Fluvial → Hydraulic → Thermal → …,
   each layer with params + enable toggle; deterministic full-stack rebuild.
   The stack (params + seed) *is* the project file: small JSON, perfectly
   reproducible → save/load/share.
3. **Preset gallery** (Alpine, Islands, Canyon, Dunes, Volcanic) as the
   first-run experience on the flat canvas.
4. **Undo/redo** over the layer stack (param history, not heightmap
   snapshots — determinism makes replay free).
5. Surface the new exports (PNG-16/EXR/OBJ/glTF) with an "import into
   UE5 / Unity / Blender" help panel.

**Exit criteria:** a stranger can open the site, make a decent terrain from a
preset in five minutes, and import it into Blender/UE without reading docs.

## Phase 2 — TitanLab, the native macOS app (the paid product)

Goal: a real Mac app that feels Mac-native and outruns the browser.

1. Xcode project: SwiftUI shell + `MTKView`; Objective-C++ bridge
   (`TitanBridge.mm`) linking `libTitanCore.a` (universal arm64+x86_64).
2. Metal renderer: heightfield mesh, splat-driven shading (port the web
   fragment shader to MSL), sky/fog, orbit + WASD fly camera, triple
   buffering per the design doc.
3. Port the **layer-stack UI** from the web lab (same JSON project format —
   `.titan` files open in both).
4. Generation on a background queue; deferred regeneration while sliders
   drag (regenerate on release), progress reporting.
5. Exports + drag-and-drop out; `.titan` file association.
6. Distribution: Developer ID signing + notarization + DMG (skip Mac App
   Store initially — sandboxing friction, 30% cut). Auto-update via Sparkle.
7. Private beta with a handful of environment artists; iterate.

**Exit criteria:** notarized DMG a beta tester can download, use, and export
from without help; 1024² interactive workflow feels instant.

## Phase 3 — TitanBridge, the Unreal plugin (Fab marketplace)

Goal: Titan terrain inside the UE editor on Mac **and Windows**.

1. Plugin skeleton; `TitanBridge.Build.cs` linking the static lib per
   platform (Mac `.a`, Windows `.lib` — built by the Phase-0 CI).
2. `ATitanTerrainActor`: `UProceduralMeshComponent` preview, params as
   `UPROPERTY`s, `CallInEditor` Generate button, async generation via the
   task graph, collision deferred behind a "Finalize" button.
3. **Landscape bake path**: write the heightmap + weightmaps into a real
   `ULandscape` — that's what production users actually want for big worlds;
   the PMC path is for instant preview.
4. Auto-generated landscape material from splat weights (RVT optional, later).
5. Load `.titan` project files exported from TitanLab/web — the cross-sell
   story: design in TitanLab, bake in Unreal.
6. Fab submission: demo map, video, docs, versioning against UE 5.x.

**Exit criteria:** plugin passes Fab review; a UE user on Windows who has
never seen TitanLab can generate and bake a landscape in one session.

## Phase 4 — Launch & business

1. Pricing decision (current lean): **web lab free** (capped resolution) →
   **TitanLab paid, cheap** (Gumroad/Paddle handles payments + license keys)
   → **TitanBridge paid on Fab** (Fab handles licensing). `.titan`
   interchange makes each tier sell the next.
2. Website + docs + three short tutorial videos (flat canvas → preset →
   erode → export to UE/Blender).
3. Opt-in crash reporting; support channel; EULA + third-party license file.
4. Launch: Fab listing, r/proceduralgeneration, Mac gamedev communities,
   "Gaea for Mac" positioning.

## Deliberately after v1 (don't block launch)

- **GPU erosion** (Metal compute, pipe-model) for 4k–8k terrains — biggest
  post-launch perf win; CPU path stays as the deterministic reference.
- Tiled/infinite worlds (the world-space noise groundwork is already done).
- Node graph view for power users, biome/climate simulation, vegetation
  export (scatter data), Unity native plugin (heightmap import already works).

## Sequencing logic & rough effort (solo, part-time)

Phases 0–1 are weeks-scale. Phase 2 is the long pole (Metal + Swift bridge,
likely a couple of months of evenings). Phase 3 is smaller than it looks
*if* Phase 0's CI proved the Windows build. Phase 4 runs partly in parallel.

Order matters because: the web lab is the cheapest place to decide UX (layer
stack) before rebuilding it natively; the C API freeze must precede two
frontends consuming it; and Windows CI must precede any Fab commitment.
