# Titan — working notes for Claude

Deterministic procedural terrain. One C++ engine, three hosts. Everything
lives under `titan-terrain-engine/`.

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
