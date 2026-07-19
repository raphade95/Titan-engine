# The .titan Project File

A `.titan` file is a small JSON document that fully describes a terrain.
Because the engine is deterministic (same seed → bit-identical output,
including all erosion), this recipe *is* the terrain — no baked data needed.

## Schema (version 1)

```json
{
  "version": 1,
  "params": {
    "size": 256,
    "scale": 2.5,
    "heightMultiplier": 70,
    "seed": "alpine-ridge-4",
    "octaves": 8,
    "persistence": 0.5,
    "lacunarity": 2.0,
    "exponent": 1.1,
    "warpStrength": 0.6,
    "noiseType": "ridged",
    "biome": "temperate"
  },
  "stack": [
    { "type": "fluvial",   "enabled": true, "params": { "passes": 3, "strength": 1.2 } },
    { "type": "hydraulic", "enabled": true, "params": { "iterations": 65536, "spawnMode": 1 } },
    { "type": "thermal",   "enabled": true, "params": { "passes": 12, "talusAngle": 35, "rate": 0.5 } }
  ]
}
```

## Field notes

- `seed` is a free string, hashed with **FNV-1a 32-bit** in every product,
  so files are portable between the web lab, TitanLab, and TitanBridge.
- `noiseType`: `none` | `standard` | `ridged` | `billow`.
- `biome` affects rendering only, never the heightfield.
- Layer `type`: `fluvial` | `hydraulic` | `thermal` | `terrace` | `plateau`.
  Layers run in array order. Unknown types are skipped on load (forward
  compatibility); out-of-range params are clamped.
- `hydraulic.iterations` rounds up to multiples of 2048 (the engine's
  determinism batch); UIs use steps of 16384.

## Guarantees

- Loading a v1 file in any current or future Titan product reproduces the
  terrain exactly (the engine's determinism tests enforce this).
- Files are safe to diff, version-control, and share — they contain only
  parameters, never user paths or machine data.
