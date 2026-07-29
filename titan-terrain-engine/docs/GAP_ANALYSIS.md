# Titan Engine — Feature Gap Analysis

Competitive gaps identified by comparing Titan's current feature set against World Machine 2, Gaea 2.0 Early Access, Houdini HeightField (H16–H17), and Terragen Classic at their earliest public releases.

**Status (v0.8):** every High and Medium item below is implemented in libTitanCore
and exposed in both the web lab and the macOS app. Three items remain open, all
Low priority: surface-detail nodes (#11), tiled export (#20), and DEM/GeoTIFF
decoding (the remaining half of #5).

This list is a *competitive floor*, not a roadmap — it was drawn up against what
rivals shipped in their earliest public releases. The engine has since gone well
past it, and the work below is not on the table at all:

| Since this list | What landed |
|---|---|
| v0.6 | Snow and Lakes promoted from engine-only to layers in both UIs |
| v0.7 | **Volcanism** — stratovolcano edifice stamping plus a cellular lava flow with yield strength, self-channelizing streams, and chilled lava folded back into the terrain. Drag-to-place in both viewports |
| v0.8 | **Viewport overhaul** — linear-space lighting, horizon-traced AO shared with the exporter, hemisphere ambient, curvature cavity shading, ACES tone mapping, camera-relative aerial perspective, and simulated snow/lakes actually rendered |
| throughout | LOD preview meshing (decouples simulation resolution from preview cost, so grids can exceed 512), parameter sanitization, exception-safe C boundary with `titan_last_error`, canonical engine-side seed hashing and band curves, mass-balance and height-range introspection |

Test coverage grew from ~17 checks to 47 test functions — 185 assertions —
across `titan_tests` and `titan_golden`, plus the WASM golden harness and the
TitanLab smoke test.

**The one open architectural question** is not on this list either: every
competitor is a node graph, Titan is a linear layer stack. That is a deliberate
product position, not a gap — it should be revisited as a decision, not closed
as a to-do.

---

## Noise & Terrain Generation

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 1 | Voronoi / Cellular noise | WM2, Houdini (12 variants), Gaea (Cellular, Cellular3D) | High | ✅ v0.4 core; v0.5 exposed in both UIs (Cells, Walls) |
| 2 | Worley / distance-based noise | Houdini (F1, F2-F1, Manhattan, Chebyshev variants) | Medium | ✅ v0.5 — F1/F2-F1 + Manhattan/Chebyshev metrics (noise types 6, 7) |
| 3 | Gradient / radial gradient generators | WM2 (Gradient, Radial Grad), Gaea (LinearGradient, RadialGradient) | Medium | ✅ v0.5 — stamp shapes 4/5; Gradient layer (web), Gradient Generator (macOS) |
| 4 | Additional fractal modes (Terrain, Hybrid Terrain) | Houdini (4 fractal modes beyond standard fBm) | Medium | ✅ v0.5 — Musgrave hybrid multifractal (noise type 8) |
| 5 | Heightfield file import (PNG, RAW, DEM, GeoTIFF) | All four competitors | High | ✅ v0.5 — .png/.r16/.r32 via `titan_apply_heightfield` (engine-side resampling); DEM/GeoTIFF post-launch |

## Filters & Shaping

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 6 | Clamp / clip heights | WM2 (Clamp), Houdini (Clip), Gaea (Clamp, Clip) | Medium | ✅ v0.5 — `titan_apply_clamp` |
| 7 | Curves / height remap (custom transfer function) | WM2 (Curves), Houdini (Remap), Gaea (Curve, Recurve, Shaper) | High | ✅ v0.5 — monotone-cubic control points; 5-point layer (web), presets (macOS) |
| 8 | Blur / smooth | Houdini (Blur), Gaea (Blur, Median, SlopeBlur, VariableBlur) | High | ✅ v0.5 — `titan_apply_blur` (masked = SlopeBlur-style workflows) |
| 9 | Sharpen | Gaea (Sharpen) | Medium | ✅ v0.5 — `titan_apply_sharpen` (unsharp mask) |
| 10 | Simple transform (scale vertical, offset, invert) | WM2 (Simple Transform), Gaea (Adjust, Autolevel, Flip) | High | ✅ v0.5 — `titan_apply_transform` |
| 11 | Surface detail nodes (Roughen, Sandstone, Rockscape) | Gaea (20+ detail nodes) | Low | ⏳ partially covered by masked high-frequency noise layers |

## Masking System

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 12 | Mask by feature (slope, height, curvature) | Houdini (Mask by Feature — 5 criteria), Gaea (Slope, Height, Curvature, Angle) | High | ✅ v0.5 — `titan_mask_by_feature` (soft band + invert) |
| 13 | Noise-based mask generation | Houdini (Mask Noise) | Medium | ✅ v0.5 — `titan_noise_to_mask` |
| 14 | Per-layer mask support (mask per pipeline operation) | WM2 (mask input port per device), Houdini (second input), Gaea (mask ports) | High | ✅ v0.5 — every web layer has a mask block; macOS masks the sim/adjustment stack |

## Combiners & Blending

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 15 | General combiner (add, subtract, multiply, max, min, blend two heightfields) | WM2 (Combiner), Houdini (Layer), Gaea (Combine, Mixer, Layers) | High | ✅ v0.5 — `titan_apply_heightfield` blend modes + Combine Import layer |

## Selectors & Derived Maps

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 16 | Full slope map (grid-wide, not just point probe) | Houdini, Gaea (Slope), Terragen (surface slope constraints) | Medium | ✅ v0.5 — `titan_compute_slope_map` |
| 17 | Curvature map | Houdini (Mask by Feature curvature), Gaea (Curvature) | Medium | ✅ v0.5 — `titan_compute_curvature_map` (Laplacian) |
| 18 | Normal map export | Gaea (Normals), Houdini | Medium | ✅ v0.5 — `titan_export_normal_png` (RGB8, world-space) |
| 19 | Occlusion / AO map | Gaea (Occlusion, AO), Houdini (Mask by Feature occlusion) | Low | ✅ v0.5 export; v0.8 `titan_compute_ao` shares one field with the viewport shading |

## Export & Output

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 20 | Tiled export (for large worlds) | WM2 Pro (Tiled Output), Houdini (Tile Split/Splice), Gaea (Build Swarm) | Low | ⏳ world-space origins already make tiles seamless; batch UI post-launch |

## Workflow & UI

| # | Feature | Present In | Priority | Status |
|---|---------|-----------|----------|--------|
| 21 | 2D top-down view | WM2, Terragen, Gaea | Medium | ✅ v0.5 — hypsometric + hillshade overlay in both UIs |

---

## Summary by Priority

### High (8 items — competitive table stakes) — all shipped
1. ✅ Voronoi / Cellular noise
2. ✅ Heightfield file import
3. ✅ Curves / height remap
4. ✅ Blur / smooth filter
5. ✅ Simple transform (scale, offset, invert)
6. ✅ Mask by feature (slope, height, curvature)
7. ✅ Per-layer mask support
8. ✅ General combiner (blend two heightfields)

### Medium (9 items — expected by power users) — all shipped
9. ✅ Worley / distance-based noise
10. ✅ Gradient / radial gradient generators
11. ✅ Additional fractal modes (hybrid multifractal)
12. ✅ Clamp / clip heights
13. ✅ Sharpen
14. ✅ Noise-based mask generation
15. ✅ Full slope map
16. ✅ Curvature map
17. ✅ Normal map export

### Low (4 items — nice to have, not launch blockers)
18. ⏳ Surface detail nodes (partially covered by masked noise layers)
19. ✅ Occlusion / AO map (exported *and* used for viewport shading since v0.8)
20. ⏳ Tiled export
21. ✅ 2D top-down view
