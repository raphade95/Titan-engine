# Getting Started with Titan

Titan generates game-ready terrain in three steps: **pick a preset, tune the
stack, export**. Everything is deterministic — a seed plus your settings is
the entire terrain, reproducible forever, in every Titan product.

## 1. The flat canvas

Titan opens with a clean, flat plane. Nothing is pre-baked; the terrain you
make is yours from the first click. Start either way:

- **Preset** (recommended first time): pick Alpine Peaks, Island Chain,
  Canyonlands, Rolling Dunes, or Volcanic Shield. Each is a complete recipe —
  base noise plus a simulation stack — with a fresh random seed.
- **From scratch**: choose a Noise Structure (Simplex / Ridged / Billow) and
  shape it with the Base sliders.

## 2. Base terrain controls

| Control | What it does |
|---|---|
| Seed | The identity of your terrain. Lock it to keep a landscape while tuning; Randomize to explore. |
| Resolution | Grid size (128–512 in the lab). Higher = more detail, slower simulation. |
| Scale | How many noise features span the terrain — small values = one big landmass. |
| Height | Vertical scale of the landscape. |
| Octaves | Layers of detail. 4 = smooth, 8+ = craggy. |
| Exponent | Shaping curve: >1 flattens lowlands and sharpens peaks. |
| Domain Warp | Bends the noise through a second noise field — the difference between "blobs" and tectonic-looking landforms. |

## 3. The simulation stack

Layers run **top to bottom** on every rebuild. Reorder, toggle, or remove
them freely — the whole stack re-runs deterministically.

- **River Networks** — routes rainfall across the entire map and carves
  connected, branching drainage (stream-power erosion). The "make it look
  real" layer.
- **Hydraulic Erosion** — droplet simulation; adds riverbeds, sediment fans,
  and fine flow detail. "Highlands" rainfall concentrates rain on peaks.
- **Thermal Weathering** — settles loose material to the angle of repose;
  softens cliffs into talus slopes.
- **Terrace** — geological stepping for mesas and rice-paddy looks.
- **Plateau** — compresses peaks toward a ceiling with a rounded shoulder.

A good default order: **Terrace/Plateau → River Networks → Hydraulic →
Thermal** (shape first, drainage second, detail third, settling last).

### When a stack is not enough (TitanLab)

The stack applies each layer to the result of the one above it. That is the
right shape for most work, and it is where to start. What it cannot do is treat
one base terrain two different ways and merge the results — eroded valleys
combined with the *unweathered* ridgeline, say, or a mask built from the
terrain's own slope before erosion softened it.

**Open Node Graph** (toolbar, the Stack tab, or ⌥⌘G) opens the same operations
as a graph in a drawer beneath the viewport. It starts as your current stack
laid out left to right, because a stack *is* a graph that happens to be a line.

- Drag from a node's right-hand port to another node's left to wire them up;
  drop the wire on empty canvas to pick its destination from a search palette.
- Double-click the canvas to add a node. Drag an unconnected node onto a wire
  to splice it into the chain.
- The **eye** on any node shows that node's result in the 3D viewport, without
  rewiring anything. Only the nodes feeding it are evaluated.
- The **purple port** on the bottom-left of a node is a Mask input: whatever
  field you wire into it limits that operation, the same as a layer's mask.
- **Bake to Stack** converts back, when the graph is still a straight line.
  Once it forks, it cannot be a stack, and the drawer says so rather than
  quietly dropping a branch.

Graphs are saved in the `.titan` file. A project whose graph drives the terrain
is a v4 file and will not open in the web lab, which has no graph to open it
into — see [titan-file-format.md](titan-file-format.md).

## 4. Export

See [exports.md](exports.md) for the format table and per-app import steps
(Unreal, Unity, Blender).

## 5. Save your work

**Save .titan** writes a small JSON project file — seed, base parameters,
and the full stack. Loading it reproduces the terrain exactly, in the web
lab, TitanLab for macOS, and the Unreal plugin.
