# pyde-template

A Pyde contract scaffolded by `otigen init --lang c`. This README is
your project-local cheatsheet — common commands, the storage model,
host function quick reference, and the gotchas you'll hit early.

For depth (full ABI, error codes, gas table, threat model, parachain
design), see the protocol book at <https://book.pyde.network>.

---

## Project layout

```
pyde-template/
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # contract metadata + state schema + network
├── main.c                 # YOUR CONTRACT CODE (start here)
├── include/
│   └── pyde/
│       └── host.h         # vendored copy of every pyde::* host fn declaration
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

---

## Prerequisites

C is the bare-metal language: you need a clang with the **wasm32**
backend AND **wasm-ld** (LLVM's WASM linker). Apple's bundled clang
lacks both. Install LLVM + lld via Homebrew:

```bash
brew install llvm lld

# Add to your shell profile (~/.zshrc or similar):
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
```

On Linux: `apt install clang lld` (or your distro's equivalent) ships
a wasm32-capable clang + wasm-ld out of the box.

`make check-tools` verifies everything's present.

---

## Quick start

```bash
make build       # clang → build/contract.wasm ('otigen build' bundles it → ./artifacts/pyde-template.bundle/)
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

**Storage is variable-length**. The contract derives the slot itself
via a `derive_slot` helper (see `main.c`), then reads or writes the
exact byte width it needs through `sload` / `sstore` / `sdelete`
(declared in `include/pyde/host.h`).

Inside a slot, byte conventions:

- Numbers (uint64, uint128): little-endian, exact width (no 32-byte padding).
- Addresses: raw 32-byte hash.
- Booleans: single byte; 0 = false, anything else = true.

---

## Host functions (where to look)

Every `pyde::*` host fn is declared in `include/pyde/host.h` with a
short comment + gas cost. Categories:

| Category | Section | What you use it for |
|---|---|---|
| Storage | §7.1 | `sload` / `sstore` / `sdelete` (call `derive_slot(field, key)` first) |
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

## Common operations (C idioms)

### Read a uint64 scalar slot

```c
static const uint8_t FIELD_COUNTER[7] = {'c','o','u','n','t','e','r'};

static uint64_t read_counter(void) {
    uint8_t slot[32];
    derive_slot(FIELD_COUNTER, sizeof FIELD_COUNTER, (const uint8_t*)0, 0, slot);

    uint8_t buf[8];
    int32_t actual = sload(slot, buf, 8);
    if (actual <= 0) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | (uint64_t)buf[i];
    }
    return v;
}
```

### Write a uint64 scalar slot

```c
static void write_counter(uint64_t value) {
    uint8_t slot[32];
    derive_slot(FIELD_COUNTER, sizeof FIELD_COUNTER, (const uint8_t*)0, 0, slot);

    uint8_t buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(value & 0xff);
        value >>= 8;
    }
    sstore(slot, buf, 8);
}
```

### Read a mapping slot (`balances[owner]`)

```c
static const uint8_t FIELD_BALANCES[8] = {'b','a','l','a','n','c','e','s'};

static uint64_t read_balance(const uint8_t owner[32]) {
    uint8_t slot[32];
    derive_slot(FIELD_BALANCES, sizeof FIELD_BALANCES, owner, 32, slot);

    uint8_t buf[8];
    int32_t actual = sload(slot, buf, 8);
    if (actual <= 0) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)buf[i];
    return v;
}
```

### Read the caller's address

```c
static void current_caller(uint8_t out[32]) {
    caller(out);
}
```

### Emit an event

```c
uint8_t topics[64]; // topic-0 + indexed[0]
__builtin_memcpy(topics, TOPIC_0, 32);
__builtin_memcpy(topics + 32, who, 32);

uint8_t data[16]; // u128 LE
// ... write amount as LE u128 ...

emit_event(topics, 2, data, sizeof(data));
```

### Revert with a reason

```c
static const char MSG[] = "InsufficientBalance";
revert((const uint8_t*)MSG, sizeof(MSG) - 1);
// `revert` is declared noreturn — control flow stops here.
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
expect.gas_max      = "200_000"   # regression guard
```

Final-state assertions:

```toml
[tests.expect]
storage.counter = "3"
```

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## Common gotchas

- **You renamed the project.** No code change needed in `main.c` —
  `derive_slot` pulls `self_address` at runtime, so
  renaming Just Works. Just remember to update `[contract].name` in
  `otigen.toml`.
- **`wasm-ld` not found.** clang's wasm32 backend needs LLVM's wasm
  linker. Apple's bundled clang has neither; `brew install llvm lld`
  + PATH setup (see Prerequisites) fixes it.
- **You included `<stdio.h>` and got a wall of errors.** `-nostdlib`
  means no libc. Use freestanding headers only: `<stdint.h>`,
  `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`. For `memcpy`/`memset`,
  use Clang's `__builtin_memcpy` / `__builtin_memset` (lowered inline,
  no libc dep).
- **`__builtin_trap()` for unrecoverable errors.** Compiles to WASM
  `unreachable`. Same effect as `revert` without a reason.
- **Function-pointer tables.** clang's wasm output uses indirect-call
  tables for function pointers. Pyde's deploy validator allows them
  but the export surface stays clean.
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
