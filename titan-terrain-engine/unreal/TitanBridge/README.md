# TitanBridge — Titan Terrain for Unreal Engine

Deterministic procedural terrain inside the Unreal editor, powered by the
same libTitanCore engine as TitanLab. Same seed + same settings = identical
terrain, on Mac and Windows, in every Titan product.

## Install

1. Copy the `TitanBridge` folder into your project's `Plugins/` directory
   (create it next to your `.uproject` if it doesn't exist).
2. Ensure the platform library exists:
   - Mac: `ThirdParty/TitanCore/lib/Mac/libTitanCore.a` (bundled)
   - Windows: `ThirdParty/TitanCore/lib/Win64/TitanCore.lib`
     (see the README in that folder to build it)
3. Open the project. When prompted to rebuild the plugin, accept.
4. Enable **Edit → Plugins → Procedural → Titan Terrain Bridge** if needed.

## Use

1. Place **Titan Terrain Actor** from the Place Actors panel.
2. Tweak Base + Erosion settings in the Details panel
   (`Titan|Base`, `Titan|Erosion` categories).
3. Click **Generate Terrain**. Generation runs on a background thread —
   the editor stays responsive. **Randomize Seed** rolls a new landscape.
4. Leave **Generate Collision** off while iterating; click
   **Finalize Collision** when the terrain is final.

Vertex colors carry the splat data (R = rock/slope, G = height,
B = flow/wetness, A = sediment depth) — drive your landscape material's
layer blending from them, or from an exported splatmap.

## Big worlds: heightmap route

For production-scale worlds, use a real Landscape instead of the preview
mesh: export a `.r16` from TitanLab (or the web lab) and import it via
**Landscape mode → Import from File**. The `.titan` project file reproduces
the exact same terrain in every tool, so the preview actor and the imported
Landscape match.

## Determinism contract

- Seeds are FNV-1a hashed strings — `"alpine-4"` produces the same terrain
  here, in TitanLab, and in the web lab.
- Erosion is bit-identical across thread counts and platforms
  (no fast-math; fixed-batch parallel droplets).

## Versions

Built and tested against UE 5.8 (Mac). Source-compatible with 5.3+.
