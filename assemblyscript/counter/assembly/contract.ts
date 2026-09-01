// Your contract. This is the file to edit.
//
// Pyde requires every chain-facing export to have the WASM signature
// `() -> ()`: arguments arrive as borsh calldata, results leave through
// `pyde::return`. You do not write any of that. For each
// `[functions.<name>]` in otigen.toml you write one ordinary typed
// function marked `@entry`, and `otigen build` generates the export
// into `pyde.generated.ts`.
//
// Storage works the same way. `[state].schema` generates a typed
// accessor into `pyde.storage.generated.ts`, and the slot is derived
// HOST-side as Poseidon2(self_address || field || keys…) — the contract
// never hashes a slot itself. `[events.*]` generates typed emitters the
// same way.
//
// Change a type in otigen.toml and the generated substrate changes with
// it; if a signature here stops agreeing, the build fails naming both
// sides rather than shipping a contract that decodes calldata one way
// and reads it another.

import { storage } from "./pyde.storage.generated";

// The `@view` / `@mutating` / `@payable` markers state what an entry
// DOES, and are checked against `[functions.*]` by the
// `@pyde-net/host/transform` plugin wired in asconfig.json: mark a
// function `@view` while the manifest says it mutates, or the reverse,
// and the build fails naming both sides. They add nothing to the wasm —
// the ABI comes from otigen.toml either way.

// increment() -> uint64 — advance the counter, return the new value.
@entry
@mutating
export function increment(): u64 {
  const next = storage.counter.read() + 1;
  storage.counter.write(next);
  return next;
}

// get() -> uint64 — read the counter without mutating. Declared `view`
// in otigen.toml, so the engine rejects any state write from this frame.
@entry
@view
export function get(): u64 {
  return storage.counter.read();
}
