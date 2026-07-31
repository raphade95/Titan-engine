// Prints the C API surface of the checked-in WASM module, one symbol a line.
//
// This is the drift check that does not care which compiler built the module.
// Byte-comparing the artifact against a rebuild only works when CI and the
// developer run bit-identical toolchains; this asks the weaker but far more
// stable question — does the module the web lab ships still expose everything
// the engine exports? That is the failure that actually reached users: the
// node graph shipped titan_read_height/titan_set_height in the C++ and the
// checked-in module never got rebuilt, so the web lab had no way to evaluate
// a graph at all.
//
//     node cpp/tools/wasm_exports.mjs [path-to-titan_core.js]

import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const modulePath = process.argv[2]
  ? resolve(process.argv[2])
  : resolve(here, '../../src/wasm/titan_core.js');

const { default: createTitanModule } = await import(modulePath);
const m = await createTitanModule();

const exports = Object.keys(m)
  .filter((k) => k.startsWith('_titan_'))
  .sort();

if (exports.length === 0) {
  console.error('no _titan_* exports found — is this a TitanCore module?');
  process.exit(1);
}

console.log(exports.join('\n'));
