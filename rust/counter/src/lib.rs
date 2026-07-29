//! `counter-rust` — minimal Pyde contract scaffolded by `otigen init --lang rust`.
//!
//! Demonstrates the four fundamentals every Pyde contract needs:
//!   1. `#![no_std]` + a panic handler (Pyde runs `wasm32-unknown-
//!      unknown` — there is no OS, no std, no stdout).
//!   2. Host-fn imports via the `pyde-host` crate.
//!   3. Schema-typed storage via `pyde::declare_storage!()`. The
//!      macro reads `otigen.toml` at compile time, parses the
//!      `[state]` schema, and emits one typed accessor per field
//!      under `mod storage`. Each accessor delegates to the chain's
//!      typed-storage host fns (`sstore_scalar`/`sload_scalar`/...)
//!      so the chain validates the field name + value type and
//!      derives the slot internally (`Poseidon2(self_address ||
//!      field_name)`). Contracts can no longer write to slots
//!      derived from another contract's address.
//!   4. `#[pyde::entry]` (from `pyde-entry-macros`) wraps each
//!      exported function in the calldata-decode + return-wrap shim
//!      required by Pyde's `() -> ()` entry ABI.
//!
//! Bigger surfaces (events, balance transfers, cross-contract calls,
//! upgrade flows) are demonstrated in the canonical examples at
//! `pyde-net/otigen/examples/`.

#![no_std]

extern crate alloc;

use core::panic::PanicInfo;
use pyde_host as pyde;

/// Pyde sandboxes every contract — a panic traps the WASM instance
/// and the engine reverts the per-tx overlay. We don't need to do
/// anything special here; just halt deterministically.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

// ─────────────────────────────────────────────────────────────────────
// Schema-typed storage
// ─────────────────────────────────────────────────────────────────────
//
// Reads `otigen.toml`'s `[state] schema` at compile time and emits one
// typed accessor per field. For
//
//   [state]
//   schema = [
//       { name = "counter", type = "uint64" },
//   ]
//
// you get `storage::counter()` returning a `CounterField` with
// `.read() -> u64`, `.write(value: u64)`, `.delete()`. Misspelling
// the field name is a compile error; supplying the wrong value type
// is a compile error. The contract never sees raw slot bytes.

pyde::declare_storage!();

// ─────────────────────────────────────────────────────────────────────
// Public entry points (declared in otigen.toml `[functions.*]`)
// ─────────────────────────────────────────────────────────────────────

/// Increment the counter; return the new value.
///
/// The `#[pyde::entry]` macro generates the `() -> ()` WASM shim:
///   - decodes calldata from the host (none here — no args)
///   - calls this inner body
///   - borsh-encodes the return value + surfaces it as
///     `receipt.return_data` via `pyde::return`
#[pyde::entry]
fn increment() -> u64 {
    let next = storage::counter().read().wrapping_add(1);
    storage::counter().write(next);
    next
}

/// Read the current counter without mutating it. The `view`
/// attribute lives in `otigen.toml` under `[functions.get]`; the
/// chain's `pyde_call` RPC sets view-mode at runtime so any
/// mutating host fn would trap with `ERR_FORBIDDEN`.
#[pyde::entry]
fn get() -> u64 {
    storage::counter().read()
}
