# Titan — working notes for Claude

Deterministic procedural terrain. One C++ engine, three hosts. Everything
lives under `titan-terrain-engine/`.

## Current state — 2026-08-03

Pre-launch. Engine C API is at v0.11, `.titan` format at v4.

**Working:** TitanLab has the node graph, curve editor, DEM import, tiled
export and undo/redo. The web lab mirrors it minus the graph. The Unreal
plugin builds on UE 5.8 and 5.5 with mesh and Landscape output, `.titan`
import (v2/v3; refuses v4), and World Partition streaming proxies.

**Known open — check these before assuming something is a new bug:**

- The Unreal plugin calls **19 of the engine's 95** exported functions. Its
  `.titan` stack import understands only `fluvial`, `hydraulic` and `thermal`;
  everything else is named in `ImportReport` rather than dropped silently.
- Its default material is an engine stand-in and ignores the biome colour the
  engine writes into vertex colours.
- Nothing conforms imported terrain to ground already in a level; it is placed
  at the actor.
- **UE 5.6 is untested** — the directory here is a stub. 5.5 is verified only
  through the synchronous suites, so its async delivery path is unproven.
- `titan_version()` still reports 0.5.0, years behind the actual API level.
- The `h + (x - h) * MaskAt(i)` rounding that broke snow idempotence also sits
  in terrace, clamp, blur, sharpen and curve. None is idempotence-tested and
  fixing them would move goldens.
- The erosion fix (droplets reading their own batch's deposits) changed what
  existing `.titan` projects render. Deliberate, and unversioned.

**Keep this section honest.** It is read at the start of every session and
acted on with confidence, so a stale entry is worse than a missing one. Update
it in the same commit as the change it describes, and delete anything that has
stopped being true rather than letting it accumulate.

## Commits

**Do not add a `Co-Authored-By:` trailer.** Ralph asked for it removed and the
history was rewritten to strip it, so re-adding it would quietly undo that.
Commits are authored by him alone.

## Layout

| Path | What |
|---|---|
| `cpp/libTitanCore/` | **The engine. The single source of truth.** C++20, C ABI (`TitanCAPI.h`). |
| `cpp/tests/` | `test_core` (behaviour), `test_golden` (cross-platform hashes), `test_bridge_contract` (what the Unreal plugin relies on) |
| `src/` | Web lab (Vite/React) — runs the engine as WASM from `src/wasm/titan_core.js` |
| `macos/` | TitanLab, the product. SwiftUI + Metal, links `libTitanCore.a` directly. |
| `unreal/TitanBridge/` | Fab plugin. Mesh + Landscape output, `.titan` import. |
| `docs/titan-file-format.md` | `.titan` schema. Written by `EngineModel.serializeProject()`. |

## Verifying a change

```bash
cd titan-terrain-engine
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release && cmake --build cpp/build --config Release
cpp/build/titan_tests && cpp/build/titan_golden --require-reference && cpp/build/titan_bridge_contract
cpp/tools/mutation_test.sh          # asserts the golden harness can still catch its bugs
macos/build_app.sh --smoke          # builds TitanLab + headless smoke
npm run lint && npm run build       # web lab
unreal/tests/run_editor_test.sh ["/Users/Shared/Epic Games/UE_5.5"]   # needs a real UE; slow
```

CI runs everything except the Unreal editor suites (no engine on a runner).

## Invariants that bite

**Touching `cpp/` invalidates three checked-in artifacts.** Rebuild and commit
them or CI fails:
- `src/wasm/titan_core.js` — `source ~/emsdk/emsdk_env.sh` first; emsdk is
  **pinned in `cpp/emsdk-version.txt`** and a different version emits different
  bytes. `build_wasm.sh` refuses to run on the wrong one.
- `unreal/.../ThirdParty/TitanCore/lib/Mac/libTitanCore.a`
- `unreal/.../ThirdParty/TitanCore/include/*.h` — copies of `cpp/.../include/`

**Determinism is a product promise, not a nicety.** `-ffp-contract=off` and
`-fno-fast-math` are load-bearing. Golden hashes are exact on macOS arm64 (the
reference config) and toleranced elsewhere because libm is not portable. If a
change moves a golden, that is a decision to state out loud, not a number to
update quietly.

**One definition, everywhere.** Seed hashing, the curve evaluator and mask
banding each used to exist in C++, Swift and TypeScript at once, and each
disagreed. New shared logic goes in the engine and the hosts call it.

## Hosts

The macOS app is the product; the web lab is the free tier and mirrors it. The
node graph is macOS-only and the sanctioned exception. The Unreal plugin is a
**bridge, not a second authoring tool** — the leverage is `.titan` import, not
mirroring the C API into Blueprint.

Only **UE 5.5 and 5.8** are really installed here; the 5.6 and 5.3 directories
are ~100 KB stubs. 5.5 cannot spawn actors from Python under `-ExecCmds`, which
is why the synchronous test suites exist.

## Before saying it works

Build green is not verification for anything with a viewport. Launch it and
look — a fully green suite has twice hidden a feature that was unusable or
terrain in the wrong place.
