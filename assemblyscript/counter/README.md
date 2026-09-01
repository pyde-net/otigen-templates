# pyde-template

A Pyde contract scaffolded by `otigen init --lang as`. This README is
your project-local cheatsheet — common commands, the storage model,
host function quick reference, and the gotchas you'll hit early.

For depth (full ABI, error codes, gas table, threat model, parachain
design), see the protocol book at <https://book.pyde.network>.

---

## Project layout

```
pyde-template/
├── package.json           # npm deps (assemblyscript + @pyde-net/host) + build script
├── asconfig.json          # `asc` compiler config (release target)
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # contract metadata + state schema + network
├── assembly/
│   ├── contract.ts        # YOUR CONTRACT CODE (start here)
│   └── index.ts           # asc entry: re-exports the generated shims
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

Host-fn declarations (`sload`, `sstore`, `caller`, …) live in the
`@pyde-net/host` npm package — no `host_fns.ts` is vendored into the
project. `npm install` pulls the package; imports resolve straight to
it so the ABI stays in lockstep with the toolchain.

---

## Prerequisites

```
node --version    # 18+
npm install       # installs assemblyscript + @pyde-net/host
npm ls asc        # ^0.28
otigen --version  # https://github.com/pyde-net/otigen/releases
```

`make check-tools` verifies everything's present.

---

## Quick start

```bash
npm install      # one-time: install assemblyscript + @pyde-net/host
make build       # asc build + otigen build → ./artifacts/pyde-template.bundle/
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make inspect     # otigen inspect pyde-template --network devnet
make verify      # otigen verify pyde-template --network devnet
make help        # list all targets
```

---

## Storage model (one-paragraph version)

Pyde storage is a global Jellyfish Merkle Tree (JMT). Slot keys are
32-byte hashes. Two contracts can't collide on a slot because the
canonical derivation includes the contract's address:

```
slot = Poseidon2(contract_address || field_name [|| key_bytes])
```

**Storage is variable-length**. Declare fields in `otigen.toml`'s
`[state]` and `otigen build` generates a typed accessor into
`assembly/pyde.storage.generated.ts`:

```ts
import { storage } from "./pyde.storage.generated";

storage.counter.read();          // u64
storage.counter.write(next);
storage.counter.delete();
storage.balances.read(who);      // map: keys first, in declaration order
```

The slot is derived host-side from the field name, so the contract
never hashes one, and the width is checked against the declared type
rather than trusted. Underneath the accessor calls `sload_scalar` /
`sstore_scalar` (and the `*_map1..3` family for keyed maps) from
`@pyde-net/host/assembly/raw` — you can call those directly when you
need a shape the accessor does not cover, and both land in the same
keyspace.

Inside a slot, byte conventions:

- Numbers (u64, u128): little-endian, exact width (no 32-byte padding).
- Addresses: raw 32-byte hash.
- Booleans: single byte; 0 = false, anything else = true.

---

## Host functions (where to look)

Every `pyde::*` host fn is declared in the `@pyde-net/host` npm
package with a short comment + gas cost. Import the symbols you need
directly:

```ts
import { sstore_scalar, sload_scalar } from "@pyde-net/host/assembly/raw";
```

Categories:

| Category | Section | What you use it for |
|---|---|---|
| Storage | §7.1 | `sstore_scalar` / `sload_scalar` (+ `*_map1..3`); the host derives the typed slot from the field name |
| Balance | §7.2 | `balance` / `transfer` (native PYDE) |
| Context | §7.3 | `caller` / `origin` / `self_address` / `wave_timestamp` / `chain_id` |
| Tx context | §7.4 | `tx_hash` / `tx_value` / `calldata_size` / `calldata_copy` |
| Events | §7.5 | `emit_event` |
| Hashing | §7.6 | `hash_blake3` / `hash_poseidon2` / `hash_keccak256` |
| PQ crypto | §7.7 | `falcon_verify` |
| Cross-contract | §7.8 | `cross_call` / `cross_call_static` / `delegate_call` |
| Halt | §7.9 | `pyde_return` (wire name `return`) / `revert` |
| Gas | §7.10 | `consume_gas` |
| Randomness | §7.11 | `beacon_get` |

Full spec: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>.

---

## Common operations (AssemblyScript idioms)

### Read a scalar slot

```ts
import { sload } from "@pyde-net/host/assembly";

const FIELD_COUNTER: StaticArray<u8> = [0x63, 0x6f, 0x75, 0x6e, 0x74, 0x65, 0x72]; // "counter"

const slot = deriveSlot(FIELD_COUNTER, null);
const buf = new StaticArray<u8>(8);
const actual = sload(changetype<usize>(slot), changetype<usize>(buf), 8);
if (actual <= 0) {
  // slot empty → default to zero
}
```

`changetype<usize>(...)` is AS's way of extracting the linear-memory
offset of an object's backing buffer.

### Read a mapping slot (`balances[owner]`)

```ts
const FIELD_BALANCES: StaticArray<u8> = [
  0x62, 0x61, 0x6c, 0x61, 0x6e, 0x63, 0x65, 0x73,  // "balances"
];

function readBalance(owner: StaticArray<u8>): u64 {
  const slot = deriveSlot(FIELD_BALANCES, owner);
  const buf = new StaticArray<u8>(8);
  const actual = sload(changetype<usize>(slot), changetype<usize>(buf), 8);
  if (actual <= 0) return 0;
  let v: u64 = 0;
  for (let i = 7; i >= 0; i--) v = (v << 8) | u64(buf[i]);
  return v;
}
```

### Read the caller's address

```ts
import { caller } from "@pyde-net/host/assembly";

function currentCaller(): StaticArray<u8> {
  const buf = new StaticArray<u8>(32);
  caller(changetype<usize>(buf));
  return buf;
}
```

### Manual little-endian u64 encoding

AssemblyScript has no built-in LE codec for u64. Hand-roll (matching
`index.ts` — `buf[offset]` holds the least-significant byte):

```ts
function writeU64LE(buf: StaticArray<u8>, offset: i32, value: u64): void {
  for (let i = 0; i < 8; i++) {
    buf[offset + i] = u8(value & 0xff);
    value = value >> 8;
  }
}

function readU64LE(buf: StaticArray<u8>, offset: i32): u64 {
  let v: u64 = 0;
  for (let i = 7; i >= 0; i--) {
    v = (v << 8) | u64(buf[offset + i]);
  }
  return v;
}
```

### Revert with a reason

```ts
import { revert } from "@pyde-net/host/assembly";

function fail(msg: string): void {
  const bytes = String.UTF8.encode(msg, false);
  revert(
    changetype<usize>(bytes),
    bytes.byteLength,
  );
  unreachable(); // AS doesn't know revert never returns
}
```

---

## Test framework cheatsheet

`tests/contract.test.toml` is the Foundry-shape behaviour test. Run
with `otigen test` (or `make test`). Verbosity levels:

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
```

Final-state assertions:

```toml
[tests.expect]
storage.counter = "3"
```

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## Common gotchas

- **You renamed the project.** No code change needed in
  `assembly/index.ts` — `deriveSlot` pulls `self_address` at runtime,
  so renaming Just Works.
  Just remember to update `[contract].name` in `otigen.toml`.
- **`asc` not on PATH after `npm install -g`.** Make sure your global
  npm prefix is in `$PATH` (`echo $(npm config get prefix)/bin`).
  Alternative: rely on `npm run build` (uses the local install).
- **`runtime: "stub"` means no GC.** AS allocates with a bump
  pointer that's never freed. Fine for contracts (instance is torn
  down per-tx). Don't use `--runtime incremental` — its
  `__pin`/`__unpin` exports would clutter your contract's surface.
- **`changetype<usize>(...)` looks unsafe but is the standard pattern
  for passing AS objects to host fns.** AssemblyScript's type system
  doesn't have a Rust-style raw-pointer type — `changetype` is the
  bridge.
- **`expect.gas_max` keeps failing after a refactor.** Gas costs are
  wasmtime-version-sensitive. Adjust the ceiling rather than fighting
  the runtime; the assertion is a regression guard, not a contract.

---

## Where to find more

- **Otigen Toolchain Guide** (linear walk-through):
  <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**:
  <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**:
  <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- **WASM Contract Author Guide**:
  <https://book.pyde.network/companion/WASM_AUTHOR_GUIDE>
- **Examples**:
  <https://github.com/pyde-net/otigen/tree/main/examples>

---

## License

Pick one and edit this section. Pyde itself has no preference.
