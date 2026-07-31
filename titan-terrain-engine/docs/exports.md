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

### Terrain size vs. resolution

A terrain's real-world extent is **World Size**, not Resolution. Resolution only
sets how many samples span it, so a 1024-sample export and a 128-sample export of
the same project describe the same landscape at different fidelities. One world
unit is `worldSize / size` — the apps show this as the cell size under the
Resolution slider, and it is what an Unreal or Unity import needs for horizontal
scale.

This split arrived with `.titan` **version 2**. Version 1 files predate it and
load unchanged: their extent was implicitly their sample count, so the loader
sets `worldSize = size`, reproducing exactly the terrain the file described.

**Version 3** adds arbitrary curve control points (a v2 curve layer carried five
fixed values, which the loader lifts onto the same x positions they always
used). Files are stamped with the *minimum* version needed to read them
correctly, not the newest the writing build knows — a project using no custom
curve is still a valid v2 and still opens in an older build. A reader refuses a
file newer than it understands rather than approximating it, because
reproduction is the whole point of the format.

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

- **Web lab** and **TitanLab** — the Export tab shows `EXPORT HEIGHT RANGE`
  with the span and a ready-computed Unreal Z scale.
- **C API** — `titan_export_height_range(handle, &min, &max)` for what the file
  encodes, `titan_height_range` for the true surface range. See below for when
  the two differ.

### When the export range is not the terrain range

Droplet erosion can leave a handful of single-cell sediment towers standing far
above everything around them. Normalizing to the true maximum then spends most
of the 16-bit depth on the empty gap between the landscape and a few pixels — a
196k-droplet pass measured a 99.9th percentile of 35 against a maximum of 92, so
the terrain occupied **36%** of the range and lost more than a bit and a half of
precision everywhere.

`.r16` and `.png16` therefore normalize to an **export range** that trims a
genuine outlier tail, clamping the few cells above it. The test is conservative:
a bound is only trimmed when it sits more than a quarter of the robust span
beyond the 99.9th percentile. Across realistic stacks, ordinary terrain measures
1.03–1.07x its p99.9 and exports completely untouched, while an unsettled
hydraulic pass measures 2.0–2.6x and is trimmed — recovering 36–47% usable range
to about 81%.

Both apps show the export range and flag when it has been trimmed.
`titan_export_height_range` returns it; `titan_height_range` still reports the
true surface range. **Set an importing tool's Z scale from the export range**,
since that is what the file encodes.

A **Thermal Weathering** layer settles the towers out properly — which is why
the preset stacks, which end in one, never trigger any trimming.

The splatmap's **green channel follows the same range**. It used to divide by
the Height slider, which meant every cell above the slider clamped to 255 — so
on any terrain carrying a gradient, a stamp or a volcano the channel was a flat
white plateau over the whole upper landscape, and it disagreed with the height
data in the same export. It is now normalized to the actual span, matching both
`.r16`/`.png16` and what the viewport shades.

Use `.exr` or `.r32` if you would rather not think about it at all: both store
absolute float heights with no normalization.

## Tiled export

Large worlds import as a grid of heightmaps rather than one file. Pick a tile
count in the Export tab; the web lab downloads a `.zip`, TitanLab writes the
files into a folder you choose. Names follow `titan_<seed>_x<i>_y<j>.<ext>`,
the layout Unreal's tiled landscape import expects.

Two properties make the set assemble cleanly:

**Tiles are sliced from one simulation, not generated per tile.** World-space
noise sampling does make independently generated tiles line up exactly — the
measured seam error on raw terrain is 0.0 — but erosion is not a local
operation. Droplets do not cross a tile boundary, drainage networks terminate
at it, and talus creep has nothing to slump onto beyond it. Tiles eroded
separately disagree along their shared edge by up to 4.2% of the relief, which
is a visible ridge. Slicing avoids the question entirely: the tile set
reassembles into the single-file export bit for bit.

**Every tile shares one height range.** Normalizing each tile to its own
extremes would give every tile a different vertical scale and step at every
seam. All tiles use the whole terrain's export range, so the Z scale shown in
the Export tab is correct for all of them.

`Shared edge row` adds one sample to each tile's far edge so neighbours share a
row of vertices, which is what landscape importers expect; the last tile in
each axis clamps at the grid edge and repeats its final row. Turn it off for an
exact partition — every sample in exactly one tile, nothing duplicated.

The tile count must divide the resolution (a 1024 grid tiles 1/2/4/8/16 ways;
the UI greys out the rest). Each tile covers `worldSize / tiles` world units.

## Importing real-world elevation

Both apps read DEM (digital elevation model) files directly, so you can start
from a real place and then run Titan's layers over it:

| Format | What it is |
| --- | --- |
| `.tif` / `.tiff` | GeoTIFF or plain TIFF — uint8/16/32, int16/32, float32/64; strips or tiles; uncompressed, LZW, DEFLATE or PackBits; horizontal predictor supported |
| `.hgt` | SRTM tiles — raw big-endian int16, side inferred from file size (1201, 3601, …) |
| `.dem` | Read as `.hgt` when the layout matches |

Decoding lives in the engine (`titan_decode_dem`), not in either host, so the
web lab, TitanLab and Unreal all resolve a given file to the same samples.

Three things worth knowing:

- **Voids are filled, not left as holes.** SRTM's `-32768` and TIFF `NaN`/`nodata`
  samples are replaced from their neighbours so erosion doesn't route water into
  a cliff of missing data.
- **The true elevation range is reported separately.** The imported field is
  normalized to 0–1 for the pipeline, and the app shows the source range in
  metres (`1201×1201 · elevation 214 → 3402`). Multiply your export's Z range by
  the real one when you want the result to stay geographically honest.
- **Non-square sources are stretched to a square.** A 3601×1801 tile is
  resampled, not cropped. Crop it beforehand if you need the aspect preserved.

Use the imported field as the base terrain, or as one input to a Combiner layer
to blend real topography with generated detail.

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

**World Size** sets the extent; **Resolution** sets the sample density. The
engine's `cellSize` is `worldSize / size`, so raising Resolution resolves finer
detail on the same landform rather than stretching it into a larger, flatter
one. Presets are recipes at any resolution.

This was not always so. Both apps used to pin `cellSize` at 1.0, which made the
extent equal to the sample count — a 1024 grid was a 1024-unit world, four times
wider than a 256 one, while Height stayed absolute. The same seed came out 6.7x
flatter at 1024 than at 128. The simulation passes were cell-denominated too, so
refining the grid rewrote the physics on top of that.

Both are fixed. The passes measure in world units, and hydraulic, fluvial, blur
and thermal all converge across a doubling of the grid — mean deviation 0.0% and
p99 1.2% for thermal, under 0.2% for the rest. One caveat worth knowing:

- **The default is coarse.** 128 samples over a 128-unit world is one world unit
  per droplet step, which is under-resolved for the droplet model — it leaves
  larger depositional spikes than a finer grid does. The value is preserved for
  compatibility, not because it is the converged answer.

The preview mesh is decimated to at most 512 vertices per edge regardless of
resolution, so orbiting stays smooth at 2048. **Exports always use the full
simulation resolution** — what you export is not what the preview mesh
tessellates.

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
