# The .titan Project File

A `.titan` file is a small JSON document that fully describes a terrain.
Because the engine is deterministic (same seed → bit-identical output,
including all erosion), this recipe *is* the terrain — no baked data needed.

## Schema (version 1 — the core, unchanged since)

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

## Versions

A file is stamped with the **lowest** version that can reproduce it faithfully,
not the newest the writing build knows. A project using nothing newer than v1
features is written as v1 and opens in every build ever shipped. Only a project
that an older reader would silently get *wrong* is stamped higher and refused
there. Reproduction is the point of the format, so a reader that cannot honour
a file says so rather than approximating it.

| Version | Adds | Stamped when |
| --- | --- | --- |
| 1 | The schema above | always readable |
| 2 | `params.worldSize` — world extent split from sample count | `worldSize != size` |
| 3 | `stack[].curve` — arbitrary curve control points, replacing five fixed `y0..y4` | a curve layer has control points |
| 4 | `graph` — the node graph (TitanLab only) | the graph is what makes the terrain |

A v1 file has no `worldSize`, because back then the extent *was* the sample
count; readers substitute `worldSize = size`, which reproduces it exactly. A v2
curve layer's `y0..y4` are lifted onto the x positions they always implied.

## The node graph (v4)

The layer stack is a straight line. The node graph can fork and rejoin, so a
graph that does is not expressible as a stack and the file says so by requiring
v4. A graph that is *not* driving the terrain is still saved, but does not raise
the version — an older reader can ignore it and still reproduce the file from
the stack.

```json
"graph": {
  "mode": true,
  "preview": null,
  "nodes": [
    { "id": "…UUID…", "kind": "terrain", "x": 80, "y": 240, "enabled": true,
      "params": { "scale": 3, "height": 60, "octaves": 6 } },
    { "id": "…UUID…", "kind": "blur", "x": 320, "y": 140, "enabled": true,
      "params": { "radius": 3, "strength": 1 } },
    { "id": "…UUID…", "kind": "combine", "x": 560, "y": 240, "enabled": true,
      "params": { "blend": 3, "strength": 1, "alpha": 0.5 } },
    { "id": "…UUID…", "kind": "output", "x": 800, "y": 240, "enabled": true, "params": {} }
  ],
  "edges": [
    { "from": "…UUID…", "to": "…UUID…", "port": 0 }
  ]
}
```

- `kind` names the operation. Where a kind shares its name with a stack layer
  (`hydraulic`, `blur`, `curve`, `volcano`, …) it runs the identical engine
  call with the identical parameters — the graph is a different order of
  execution, not a different set of operations.
- `port` is the input index on the destination: `0` and `1` for fields, and
  `-1` for the mask input. An input holds at most one edge.
- `mode` records whether the graph or the stack was making the terrain.
- Unknown node kinds are skipped on load, and edges are re-added through the
  same cycle check the editor uses, so a hand-edited file cannot produce a
  graph the editor would have refused to draw.

## Guarantees

- Loading a file in any product that accepts its version reproduces the
  terrain exactly (the engine's determinism tests enforce this).
- Files are safe to diff, version-control, and share — they contain only
  parameters, never user paths or machine data.
