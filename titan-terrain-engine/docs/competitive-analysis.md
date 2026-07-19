# Competitive Analysis → Titan v0.4 Plan

Research date: 2026-07-19. Sources: QuadSpinner Gaea docs/blog (1.2 → 2.2),
World Machine device reference + "Mt Rainier" release notes, SideFX Houdini
heightfield documentation.

## What the leaders actually ship

### Gaea (QuadSpinner) — the benchmark for *simulation depth*
- **Erosion family**: Erosion2 combines hydraulic + thermal + gravitational
  flows with volume preservation; **selective processing** (mask inputs),
  orographic controls, selective precipitation.
- **Snow/ice family**: Snow (accumulation by slope/altitude with melt
  dynamics), Snowfield, Dusting, Glacier, IceFloe.
- **Water family**: Rivers, Lakes, Sea, WaterColor.
- **Surface**: Debris (physics rock fragments), Scree, Vegetation
  distribution, Anastomosis, Crumble, Distress.
- **Primitives/LookDev**: Shape node + geological primitives (Badlands,
  Ridge, Canyonizer, Fold, Shatter, Stacks…).
- **Workflow**: masks/portals everywhere, Accumulator (global snow/water/
  debris masks), build vs preview resolution, caching.

### World Machine — the benchmark for *composability*
- **Generators**: multi-layer Advanced Perlin, Voronoi, Gradient, Radial
  Gradient, Constant, File Input, **Layout Generator (draw shapes —
  circles, polygons — directly as terrain input)**.
- **Combiners**: blend two heightfields (add/subtract/multiply/max/min/
  average by strength), Chooser (mask-driven selection).
- **Selectors**: Height / Slope / Angle / Convexity → masks that gate any
  other device.
- **Filters**: Terrace, Curves, Clamp, Blur, Expander, Bias/Gain.
- **Natural filters**: Erosion, Thermal, Coastal Erosion, Snow.
- **Workflow**: edit history + snapshots, progressive preview, resolution
  strategies (preview res ≠ build res), tiled builds.

### Houdini heightfields — the benchmark for *masked iteration*
- Every heightfield node takes a **mask input**; HF Mask by Feature builds
  masks from elevation/slope/facing.
- Erosion is **stacked**: multiple HF Erode passes with different masks and
  feature sizes, not one monolithic pass.
- Erosion outputs **layers**: sediment, debris, flow, flowdir — reused as
  masks downstream.

## Where Titan stands today

Titan already has: deterministic engine, layer stack with reorderable
multi-instance erosion (fluvial/hydraulic/thermal — multiple passes DO
work), domain warp, terrace/plateau, two-layer bedrock/sediment model,
flow/sediment data as splat channels, exports, three frontends.

**The structural gap, in one sentence: Titan has layers but no *masks*,
one noise per terrain, and no direct placement tools.** Everything the
user feels as "demo-like" traces to those three missing systems, plus
snow/water simulation and hidden erosion parameters.

## v0.4 feature plan (priority order)

### 1. Mask system (the unlock for everything else)
Engine: every layer gets an optional per-cell mask (0–1) multiplying its
effect. Mask sources: **Height / Slope / Flow selectors** (with min/max +
falloff), **painted masks** (brush in viewport), **shape masks** (see #3),
and combinations (multiply/add/invert).
→ Matches Gaea selective erosion, WM selectors, Houdini mask inputs.
→ Directly answers "snow on a particular peak" and "erode only this area".

### 2. Noise stacking
Replace single base noise with **Noise layers** in the stack, each with its
own type/seed offset/frequency/amplitude and a **blend mode**
(add, subtract, multiply, max, min, blend%). fBm ridged mountains + low-freq
simplex continents + Voronoi cells become one stack.
→ Matches WM Advanced Perlin + Combiner; Gaea primitive graphs.
Also add **Voronoi/cellular noise** and **gradient/radial** generators.

### 3. Shape stamps (direct placement)
New **Stamp layer**: primitive shapes — circle, rectangle, ridge line,
crater, cone/dome — with position, size, rotation, height, falloff curve,
and operation (raise / lower / set / blend). **Click-to-place and drag in
the viewport.** Shapes can also emit *masks* instead of height (feeding #1).
→ Matches WM Layout Generator and Gaea Shape; answers the square/circle
request and "custom terrain in that particular section".

### 4. Snow simulation layer
Accumulation by altitude + slope with settle iterations (reuse thermal
machinery on a snow depth field) and melt curve; outputs a snow-depth layer
rendered white in the viewport and exported as a mask channel.
→ Gaea's Snow node, WM's Snow device. Combined with #1: snow on one peak.

### 5. Advanced erosion controls (already in the engine, hidden)
Expose per-layer: sediment capacity, dissolve/deposit speeds, evaporation,
inertia, droplet lifetime, erosion radius, bedrock hardness influence
(hydraulic); talus rate + bedrock breakdown (thermal); deposit ratio, area/
slope exponents, max step (fluvial). "Simple / Advanced" disclosure per
layer so the default UI stays approachable.

### 6. Lakes & visible rivers
The fluvial pass already computes the priority-flood filled surface — lake
depth = filled − terrain, nearly free. Output water depth as a layer;
render lakes/rivers as real water in the viewport; export water mask.
→ Gaea Lakes/Rivers, WM Coastal direction.

### 7. Workflow depth (from WM "Mt Rainier" notes)
- Preview vs **build resolution** (design at 256, export at 1024+).
- Named **snapshots** on top of undo history.
- Per-layer **solo/preview** (see one layer's contribution).

### Later (v0.5+): debris/scree scatter, vegetation distribution maps,
coastal erosion, glaciers, curves editor, tiled/infinite builds, node view.

## Mac app parity

TitanLab currently exposes a fixed pipeline — the "demo feel" is real. The
plan: once #1–#5 land in the engine + .titan format, port the full layer
stack UI to SwiftUI (list + reorder + per-layer disclosure), and TitanLab
reads the same project files as the web lab. The engine work is shared;
the Mac UI is a focused port.
