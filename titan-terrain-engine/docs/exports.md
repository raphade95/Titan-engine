# Export Formats & Import Guides

All heightmap exporters live in the C++ core, so every Titan product
produces byte-identical files for the same terrain.

## Format table

| Format | Contents | Best for |
|---|---|---|
| `.r16` | RAW uint16, little-endian, normalized 0–65535 | **Unreal** Landscape import |
| `.png` (16-bit) | 16-bit grayscale PNG, normalized | **Unity**, general DCC tools |
| `.exr` | Uncompressed float32 RGB, absolute heights | **Blender** displacement, Nuke, Houdini |
| `.r32` | RAW float32, little-endian, absolute heights | Custom pipelines |
| `.obj` | Full mesh: positions, normals, UVs | **Blender** direct import, quick previews |
| Splatmap `.png` | R=rock, G=height, B=flow/wetness | Material masks in any engine |
| `.titan` | JSON project (seed + params + stack) | Reproducing/sharing the terrain itself |

Normalized formats stretch the height range to full precision; the
min/max heights are what you set with the Height slider (multiply on import).

## Unreal Engine

1. **Landscape mode → New → Import from File** and select the `.r16`.
2. Set the section/component layout Unreal recommends for your resolution
   (e.g., 505×505 or 253×253 quads per component for common sizes).
3. Set the **Z scale**: Titan's `.r16` is normalized, so
   `Z scale = heightRange_cm / 512` (Unreal's 100% Z maps 512 cm per full range).
   Example: a 200 m tall terrain → 20000 cm / 512 ≈ **39**.
4. Import the splatmap as a texture (uncheck sRGB) and drive your landscape
   material's layer blend with its channels.

Or skip files entirely: use the **TitanBridge plugin** and generate in-editor.

## Unity

1. Terrain object → Terrain Settings → **Import Raw** (use `.r16`,
   byte order: Windows/little-endian) or use the 16-bit PNG with a
   heightmap-import tool.
2. Set Terrain Height to your intended height range.

## Blender

- **Displacement route**: add a subdivided plane, add a Displace modifier,
  load the `.exr` (Non-Color), set strength. Absolute heights come through —
  no manual scaling needed if your scene uses the same units.
- **Mesh route**: File → Import → Wavefront (.obj). Normals and UVs are
  included; Y-up is handled by the importer's default axis settings
  (set "Up: Y" if your build asks).

## Notes

- Exports reflect the terrain as currently simulated, including erosion.
- For seamless multi-tile worlds, keep the same seed and step the tile
  origin by `size × cellSize` per tile (world-space noise guarantees
  matching edges). Tiled export UI is on the roadmap.
