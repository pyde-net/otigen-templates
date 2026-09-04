// The `asc` entry module.
//
// Deliberately tiny. It has exactly two jobs:
//
//   1. Re-export the generated `() -> ()` entry shims, which is what
//      turns them into the contract's public wasm exports.
//   2. Host the abort handler that asconfig.json's
//      `use: ["abort=assembly/index/abort"]` resolves by module path.
//
// Your contract logic lives in `contract.ts`. Dispatch is generated
// into `pyde.generated.ts` from otigen.toml's `[functions.*]` on every
// `otigen build`. Neither belongs here.

import { abort as sdkAbort } from "@pyde-net/host/assembly/abort";

// The generated entry points.
export * from "./generated/pyde.generated";

// Local, non-exported abort shim so asconfig's `--use abort=...`
// resolves to the SDK handler, which routes AssemblyScript's overflow
// and bounds traps into `pyde::revert` instead of emitting an
// `env.abort` import (which Pyde rejects at deploy time,
// HOST_FN_ABI_SPEC §9.1).
//
// NB: deliberately NOT exported. `--use` finds it by module path
// regardless, and otigen rejects any exported-but-undeclared function
// as "ExportedButNotDeclared".
function abort(
  message: string | null = null,
  fileName: string | null = null,
  line: u32 = 0,
  column: u32 = 0,
): void {
  sdkAbort(message, fileName, line, column);
}
