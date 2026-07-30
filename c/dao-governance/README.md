# dao-governance (C)

A minimal proposal-and-vote DAO in C, wired through the otigen C codegen seam.
Members create proposals that carry a `(target, function, calldata)` payload,
peers vote yes/no within a deadline, and once voting closes any caller can fire
`execute_proposal` to cross-call the target with the recorded payload — provided
the yes votes cleared the quorum threshold snapshotted at proposal creation.
Membership itself is fully member-driven: `add_member` / `remove_member` are
self-call only, reachable exclusively through a passed proposal that targets the
DAO, so no single key can grow or prune the member set.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`). C gets the same ergonomics through **generated code**:
> from `otigen.toml`, `otigen build` writes `pyde_gen.h` with the borsh runtime,
> typed storage accessors, event emitters, the `vec(address)` codec, and the
> `() -> ()` entry-dispatch shims. Your `main.c` includes it and defines the
> `__<fn>_impl` bodies — the compiler is the drift guard: a body whose signature
> stops matching the manifest is a hard "conflicting types" error.

## Project layout

```
dao-governance/
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

## Quick start

```bash
make build       # otigen build → generates pyde_gen.h, clang → ./artifacts/
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make help        # list all targets
```

`otigen build` runs the generator and then invokes clang itself, so you don't
compile `main.c` ahead of it (that would try to include a `pyde_gen.h` that
doesn't exist yet). The `clang-build` target is an escape hatch for when the
header already exists.

## Reaching further

`execute_proposal` reaches past the generated surface: it drives the raw
`cross_call` host fn (declared in `include/pyde/host.h`) with the proposal's
recorded target / function / calldata, then maps the ABI status code back onto
the same revert strings the Rust and Go ports use (`-13` → "target doesn't
expose that function", `-3/-9/-12` → "target call failed", any other negative →
the callee's UTF-8 revert payload, or "dao: target reverted"). The DAO ignores
the return value; only the success/revert boundary matters.

`main.c` also supplies a freestanding `__multi3` (128-bit multiply): under `-O3`
clang lowers the quorum ceiling's constant `/ 100` and the u64 overflow checks
to that compiler-rt builtin, which `-nostdlib` does not link. It's a pure
integer helper — the C analogue of the u64/u128 arithmetic Rust/Go get for free
from their standard libraries.

## Prerequisites

C needs a clang with the **wasm32** backend AND **wasm-ld**. Apple's bundled
clang lacks both. Install LLVM + lld via Homebrew:

```bash
brew install llvm lld
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
```

On Linux: `apt install clang lld` ships a wasm32-capable clang + wasm-ld out of
the box. `make check-tools` verifies it.

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
