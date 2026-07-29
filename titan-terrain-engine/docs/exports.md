# Export Formats & Import Guides

All heightmap exporters live in the C++ core, so every Titan product produces
the same files for the same terrain. (Byte-identical within a platform and
architecture; see [determinism.md](determinism.md) for the cross-platform
caveat.)

## Format table

| Format | Contents | Best for |
|---|---|---|
| `.r16` | RAW uint16, little-endian, normalized 0–65535 | **Unreal** Landscape import |
| `.png` (16-bit) | 16-bit grayscale PNG, normalized | **Unity**, general DCC tools |
| `.exr` | Uncompressed float32 RGB, absolute heights | **Blender** displacement, Nuke, Houdini |
| `.r32` | RAW float32, little-endian, absolute heights | Custom pipelines |
| `.obj` | Full mesh: positions, normals, UVs | **Blender** direct import, quick previews |
| Splatmap `.png` | R=rock, G=height, B=flow/wetness, A=sediment | Material masks in any engine |
| AO `.png` | 8-bit grayscale horizon occlusion | Baked shadowing, cavity masks |
| `.titan` | JSON project (seed + params + stack) | Reproducing/sharing the terrain itself |

### Normalized formats and the real height range

`.r16` and 16-bit `.png` are **normalized**: they stretch the terrain's actual
minimum and maximum across the full 0–65535 range. To rebuild real elevations
on import you need that actual range.

**It is not `[0, Height slider]`.** The Height slider scales the *base noise*;
the layer stack then moves both ends. Erosion lowers peaks, fluvial deposition
piles material up, and clamp/plateau/transform reshape the extremes directly. A
128×128 terrain generated at Height 40 measures `[2.19, 35.42]` after
generation and `[1.90, 80.97]` after a hydraulic + fluvial pass — the maximum
ends up **twice** the slider value.

So read the range off the app instead of assuming it:

- **Web lab** and **TitanLab** — the Export tab shows `HEIGHT RANGE (ACTUAL)`
  with the span and a ready-computed Unreal Z scale.
- **C API** — `titan_height_range(handle, &min, &max)`.

The splatmap's **green channel follows the same range**. It used to divide by
the Height slider, which meant every cell above the slider clamped to 255 — so
on any terrain carrying a gradient, a stamp or a volcano the channel was a flat
white plateau over the whole upper landscape, and it disagreed with the height
data in the same export. It is now normalized to the actual span, matching both
`.r16`/`.png16` and what the viewport shades.

Use `.exr` or `.r32` if you would rather not think about it at all: both store
absolute float heights with no normalization.

## Unreal Engine

1. **Landscape mode → New → Import from File** and select the `.r16`.
2. Set the section/component layout Unreal recommends for your resolution
   (e.g., 505×505 or 253×253 quads per component for common sizes).
3. Set the **Z scale** from the *actual* span shown in the Export tab, not
   from the Height slider:
   `Z scale = span_cm / 512` (Unreal's 100% Z maps 512 cm per full range).
   Example: an actual span of 79.07 units at 1 unit = 1 m → 7907 cm / 512 ≈
   **15.4**. Taking the Height slider (40) instead would have given ≈ 7.8 and
   a terrain half as tall as intended.
4. Import the splatmap as a texture (uncheck sRGB) and drive your landscape
   material's layer blend with its channels.

Or skip files entirely: use the **TitanBridge plugin** and generate in-editor.

## Unity

1. Terrain object → Terrain Settings → **Import Raw** (use `.r16`,
   byte order: Windows/little-endian) or use the 16-bit PNG with a
   heightmap-import tool.
2. Set Terrain Height to the **actual span** from the Export tab, not the
   Height slider — the two differ once a layer stack has run.

## Blender

- **Displacement route**: add a subdivided plane, add a Displace modifier,
  load the `.exr` (Non-Color), set strength. Absolute heights come through —
  no manual scaling needed if your scene uses the same units.
- **Mesh route**: File → Import → Wavefront (.obj). Normals and UVs are
  included; Y-up is handled by the importer's default axis settings
  (set "Up: Y" if your build asks).

## Resolution and world extent

The resolution slider (64–2048) sets both the simulation grid **and** the world
extent: a terrain spans `size x cellSize` world units, and every host currently
fixes `cellSize` at 1. So a 1024 grid is a 1024-unit-wide world, four times
wider than a 256 one.

That means **height does not scale with resolution**. A preset tuned at 128
with Height 60 has a 60:128 relief ratio; the same preset at 1024 has 60:1024
and reads as a flat plate. Raise Height proportionally (the slider goes to 500)
when you raise resolution, or treat the presets as 128–256 recipes.

The preview mesh is decimated to at most 512 vertices per edge regardless of
resolution, so orbiting stays smooth at 2048. **Exports always use the full
simulation resolution** — what you export is not what the preview mesh
tessellates.

> Design note: most competitors treat resolution as detail density over a fixed
> world, which would mean deriving `cellSize` from `size`. Titan does not do
> that today, because `cellSize` also scales slope, talus angle, and fluvial
> distances — changing it would alter every existing terrain and every preset.
> It is a deliberate open decision, not an oversight.

## Notes

- Exports reflect the terrain as currently simulated, including erosion.
- Terrain may sit **below zero**: a Lower stamp, a negative Transform offset, or
  a negative Clamp minimum all produce sub-datum ground (sea floors, basins).
  Normalized formats handle this fine — the minimum simply maps to 0 — but
  `.exr`/`.r32` carry the negative values through, which is usually what you
  want for a scene with a sea level.
- For seamless multi-tile worlds, keep the same seed and step the tile
  origin by `size × cellSize` per tile (world-space noise guarantees
  matching edges). Tiled export UI is on the roadmap.
