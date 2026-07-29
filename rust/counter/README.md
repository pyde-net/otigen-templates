# counter-rust

A Pyde contract scaffolded by `otigen init`. This README is your
project-local cheatsheet — common commands, the storage model, the
macro substrate the toolchain ships, and the gotchas you'll hit
early.

For depth (full ABI, error codes, gas table, slashing rules, parachain
design, threat model), see the protocol book at
<https://book.pyde.network>.

---

## Project layout

```
counter-rust/
├── Cargo.toml             # crate + cdylib + release profile
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # contract metadata + state schema + network
├── src/
│   └── lib.rs             # YOUR CONTRACT CODE (start here)
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

Note: there's no longer a `src/host_fns.rs` — the `pyde-host`
crate re-exports every chain-side host fn under `pyde::*` so
contracts depend on one crate and never paste extern decls.

---

## Quick start

```bash
make build      # cargo build --release + otigen build → ./artifacts/counter-rust.bundle/
make test       # otigen test (runs tests/contract.test.toml)
make test-vvvv  # otigen test -vvvv (full verbosity: gas, events, traces, storage diffs)
make deploy     # otigen deploy --network devnet
make inspect    # otigen inspect counter-rust --network devnet
make verify     # otigen verify counter-rust --network devnet (reproducibility check)
make help       # list all targets
```

---

## The macro substrate (what makes Pyde feel like Solidity for Rust)

Pyde contracts ship with three function-like proc macros that
collapse the boilerplate every chain author hand-writes on every
other Rust contract platform:

| Macro                       | What it does                                                |
| --------------------------- | ----------------------------------------------------------- |
| `pyde::declare_storage!()`  | Reads `[state]` from `otigen.toml` → typed storage accessors |
| `pyde::declare_events!()`   | Reads `[events.*]` from `otigen.toml` → typed event structs |
| `#[pyde::entry]`            | Wraps a function in the `() -> ()` chain ABI shim           |

The `pyde::ctx::*` wrappers (`caller()`, `self_address()`,
`tx_value()`, `wave_id()`, etc.) and `pyde::call::execute<T>` for
cross-contract typed calls round out the substrate.

What this means in code: no raw `*const u8` pointers, no manual
`sload`/`sstore`, no event-topic byte arithmetic. The compiler
catches schema drift; the test runner catches behaviour drift.

---

## Storage model

`otigen.toml`:

```toml
[state]
schema = [
    { name = "counter", type = "uint64" },
]
```

`src/lib.rs`:

```rust
pyde::declare_storage!();

#[pyde::entry]
fn increment() -> u64 {
    let next = storage::counter().read().wrapping_add(1);
    storage::counter().write(next);
    next
}
```

The macro expands `storage::counter()` into a typed accessor that
calls the chain's `sstore_scalar` / `sload_scalar` host fns. The
chain derives the slot internally as
`Poseidon2(self_address || field_name)` — contracts never see slot
bytes, contracts can't collide on slots derived from another
contract's address (the chain enforces it), and there's no
`derive_slot` helper to copy-paste.

### Field type vocabulary

| Token                         | Rust type                  | Width                                        |
| ----------------------------- | -------------------------- | -------------------------------------------- |
| `u8` / `u16` / `u32` / `u64` / `u128` | `u8`..`u128`         | 1 / 2 / 4 / 8 / 16 bytes LE                  |
| `i8` / `i16` / `i32` / `i64` / `i128` | `i8`..`i128`         | 1 / 2 / 4 / 8 / 16 bytes LE                  |
| `bool`                        | `bool`                     | 1 byte                                       |
| `address` / `hash32`          | `[u8; 32]`                 | 32 bytes                                     |
| `bytes`                       | `Vec<u8>`                  | variable (u32 len + bytes)                   |
| `string`                      | `String`                   | variable (u32 len + utf-8)                   |
| `vec(<inner>)`                | `Vec<inner>`               | variable; inner must be fixed-width          |
| `struct(<Name>)`              | `Name` (borsh-codable)     | variable; borsh round-trip                   |

### Map shape

```toml
{ name = "balances", type = "map", keys = ["address"], value = "uint128" }
```

```rust
storage::balances().read(&owner);
storage::balances().write(&owner, 100);
storage::balances().delete(&owner);
```

Up to 3 keys are supported. Keys can be any fixed-width scalar
(addresses, hashes, primitives) or `bytes`/`string`. `vec(...)`
and `struct(...)` keys are rejected up-front to avoid slot
collisions on variable-length encodings.

---

## Common operations

### Read the caller's address

```rust
let who = pyde::ctx::caller();   // EOA for top-level tx, contract for cross_call
```

### Emit an event

`otigen.toml`:

```toml
[events.Transfer]
fields = [
    { name = "from",   type = "address", indexed = true },
    { name = "to",     type = "address", indexed = true },
    { name = "amount", type = "uint128" },
]
```

`src/lib.rs`:

```rust
pyde::declare_events!();

events::Transfer { from, to, amount }.emit();
```

The macro computes `Blake3(canonical_signature)` for topic-0 at
expansion time and packs indexed fields LE-padded-to-32 per
Pyde's event-encoding convention.

### Revert with a reason

```rust
pyde::revert("insufficient balance");   // diverges, never returns
```

The chain captures the bytes as `receipt.return_data` so callers
and the wallet UI can surface a human-readable failure.

### Cross-contract call (typed)

```rust
let balance: u128 = pyde::call::execute(&other_contract, "balance_of", (alice,))
    .expect("balance_of failed");
```

`pyde::call::execute<T>` borsh-decodes the callee's return into `T`
and surfaces categorized failures via `CallError::{Reverted,
InvalidFunction, NonPayable, InsufficientBalance,
ReentrancyBlocked, DecodeError, ReturnDataTooLarge}`.

---

## Test framework cheatsheet

`tests/contract.test.toml` is the Foundry-shape behaviour test.
Run with `otigen test` (or `make test`). Verbosity levels:

| Level | What you see |
|---|---|
| default | Pass/fail per test |
| `-v` | + gas-used + duration |
| `-vv` | + emitted events (topic-0 + count + data size) |
| `-vvv` | + per-call traces (function args return gas) |
| `-vvvv` | + storage diffs (slot → before / after) |

Per-call assertions:

```toml
[[tests.calls]]
function = "increment"
expect.return_value = "1"
expect.gas_max      = "200000"   # regression guard
# expect.events     = [{ name = "Counter", value = "1" }]
# expect.revert     = "InsufficientBalance"
# expect.no_revert  = true
```

Final-state assertions:

```toml
[tests.expect]
storage.counter           = "3"
balances.alice            = "0x...100"
events_total              = 5
```

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## Common gotchas

- **You renamed the project.** No code change needed — slot
  derivation reads the contract's own address from `self_address`
  at runtime, so renaming Just Works. Just remember to update
  `[contract].name` in `otigen.toml` to match the new project
  directory name.
- **You called `std::*` and got a confusing error.** Pyde
  contracts are `#![no_std]`. Use `extern crate alloc` for `Vec`
  / `String` / `Box` (already wired by `pyde-host`'s feature
  flags) — `dlmalloc` is the default global allocator.
- **You changed a function signature and got `BuildRejected:
  AbiMismatch` at deploy time.** Update `[functions.<name>]` in
  `otigen.toml` to match the new signature. `#[pyde::entry]`
  reads the schema at expansion; mismatches fail at `cargo
  build`, not at deploy — unless the schema drifted and you
  rebuilt against the new one without redeploying.
- **A struct-typed storage slot returns `Default::default()`
  after you change the struct shape.** The macro borsh-decodes
  the stored bytes; a layout change means old bytes don't
  decode, and the macro falls back to `Default`. Migrate by
  reading the old shape via a versioned wrapper or by clearing
  the slot before redeploying.
- **A test fails with `unknown export`.** Either the contract
  doesn't actually expose the function (check `#[pyde::entry]`)
  or the function signature is wrong in `otigen.toml`.

---

## Where to find more

- **fungible-token** — the PTS-F reference token (pts-f/1) with
  events + cross-call patterns: `examples/fungible-token/`
- **storage-stress** — every storage type / arity / shape
  exercised: `examples/storage-stress/`
- **dao-governance** + **nft-marketplace** + **multisig-wallet** —
  idiomatic patterns for cross-call dispatch + value forwarding
- **Host Function ABI v1.0**:
  <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**:
  <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- **WASM Contract Author Guide**:
  <https://book.pyde.network/companion/WASM_AUTHOR_GUIDE>
- **SDK Author Guide** (porting a contract-side SDK to another
  language): <https://book.pyde.network/companion/SDK_AUTHOR_GUIDE>

---

## License

Pick one and edit this section. Pyde itself has no preference.
