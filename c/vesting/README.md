# vesting (C)

Linear-with-cliff token vesting in C — the canonical Pyde time-locked
allocation example. Configure once at deploy with
`(beneficiary, total, start, cliff, duration)`, fund the contract with at least
`total` PYDE, then anyone can call `release()` over time to forward the
vested-but-unreleased portion to the beneficiary. The schedule is evaluated
against `wave_timestamp()`.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`) — see [`otigen-templates/rust/vesting/`](../../rust/vesting/).
> C gets the same ergonomics through **generated code**: from `otigen.toml`,
> `otigen build` writes `pyde_gen.h` with the borsh runtime, typed storage
> accessors, event emitters, and the `() -> ()` entry-dispatch shims. Your
> `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## Vesting schedule

```
t = wave_timestamp()
if t < start + cliff:      vested = 0
if t >= start + duration:  vested = total
else:                      vested = total * (t - start) / duration
```

After the cliff is crossed, vesting is **linear from `start`** — not from the
cliff. The cliff just delays the *first* release; once past, you can release
`(cliff / duration) * total` in one shot, then linearly thereafter. (Matches
OpenZeppelin VestingWallet's reference semantics.)

---

## Project layout

```
vesting/
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # metadata + [state] + [events] + [functions]
├── main.c                 # YOUR CONTRACT CODE — the __<fn>_impl bodies
├── pyde_gen.h             # GENERATED (committed) — regenerated each build
├── include/pyde/host.h    # the raw pyde::* host-fn declarations
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

`pyde_gen.h` is generated from `otigen.toml` on every `otigen build` and
committed like a `.pb.go` — never edit it by hand; edit the manifest or your
`__<fn>_impl` bodies.

---

## The seam

`otigen build` reads `otigen.toml` and generates, into `pyde_gen.h`:

- **typed storage** — `<field>_get/set` for each scalar (`beneficiary`,
  `total_amount`, `start_time`, `cliff_seconds`, `duration_seconds`,
  `released`, `configured`). The host derives each slot as
  `Poseidon2(self || field)`; you never hash a slot.
- **events** — `emit_Configured(...)`, `emit_Released(...)`, `emit_Funded(...)`.
  topic-0 is `Blake3(signature)` (via the host `hash_blake3`, so it matches
  Rust/Go byte-for-byte), indexed fields become 32-byte topics, the rest are
  borsh-encoded data.
- **entry dispatch** — one `() -> ()` export per `[functions.<name>]` that
  borsh-decodes the calldata tuple, calls your `__<name>_impl`, and
  borsh-encodes the result back through `pyde::return`.
- **forward declarations** of every `__<fn>_impl`. A signature in `main.c`
  that stops matching the manifest is a hard C "conflicting types" error — the
  compiler is the drift guard.

You write only the logic. Time is read straight from the raw host fn
`wave_timestamp()` (declared in `include/pyde/host.h`, in scope via
`pyde_gen.h`); the value transfer to the beneficiary goes through the raw
`transfer()` host fn.

### The one non-obvious bit: 128-bit math under `-nostdlib`

The vesting math multiplies and divides `__uint128_t` values, which clang
lowers to the compiler-rt builtins `__multi3` / `__udivti3`. `-nostdlib` does
not link compiler-rt, so `main.c` provides small freestanding implementations
of those (plus `__umodti3`) at the top — pure integer helpers that touch no
host state. This is the C analogue of the u128 arithmetic Rust/Go get for free
from their standard libraries.

---

## Prerequisites

C needs a clang with the **wasm32** backend AND **wasm-ld**. Apple's bundled
clang lacks both. Install LLVM + lld via Homebrew:

```bash
brew install llvm lld
# Add to your shell profile (~/.zshrc or similar):
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
```

On Linux: `apt install clang lld` (or your distro's equivalent) ships a
wasm32-capable clang + wasm-ld out of the box. `make check-tools` verifies it.

---

## Quick start

```bash
make build       # otigen build → generates pyde_gen.h, clang → ./artifacts/
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make help        # list all targets
```

---

## Test framework cheatsheet

`tests/contract.test.toml` is the Foundry-shape behaviour test — identical to
the Rust/Go `vesting` template's suite. Time-travel is per-test via
`[tests.cheats].now`, which the runner injects as `wave_timestamp()`'s return.
Run with `otigen test` (or `make test`). Verbosity: `-v` gas+duration, `-vv`
events, `-vvv` call traces, `-vvvv` storage diffs.

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
