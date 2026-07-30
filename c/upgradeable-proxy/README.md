# upgradeable-proxy (C)

The canonical Pyde upgradeable-proxy pattern in C. A thin proxy whose logic
contract is admin-swappable: `forward(fn, calldata)` delegate-calls into the
current logic in THIS contract's frame, so the proxy's storage (in particular
`value`) survives an upgrade. State preservation is the whole point.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`) — see [`rust/upgradeable-proxy/`](../../rust/upgradeable-proxy/).
> C gets the same ergonomics through **generated code**: from `otigen.toml`,
> `otigen build` writes `pyde_gen.h` with the borsh runtime, typed storage
> accessors, event emitters, and the `() -> ()` entry-dispatch shims. Your
> `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## The delegate-call pattern

`forward(function, calldata)` runs the logic contract's code under the raw
`delegate_call` host fn:

- `self_address` stays the proxy,
- `caller` is preserved across the delegate, and
- the logic's storage accessors derive slots from the **proxy's**
  `self_address` — so the logic writes to the proxy's slots.

Privileged proxy fields are prefixed `proxy_` (`proxy_admin`, `proxy_logic`) so
a logic contract that declares its own `admin` / `logic` field can't clobber the
proxy's gating state — slots are `Poseidon2(self_address ‖ field_name)`, shared
under delegate-call. `value` stays unprefixed; it's the intended shared field.

`forward` uses the **raw** `delegate_call` (declared in `include/pyde/host.h`,
in scope via `pyde_gen.h`) rather than a typed decode — the proxy is a
type-erased forwarder that can't know the logic's return shape, so it hands the
logic's bytes back verbatim. The ABI status code is mapped exactly as the
canonical `pyde-host::call::execute_delegate_raw` does: `0` = success (return
bytes), `-13` = "proxy: logic has no such function", `-3/-9/-12` = generic
"proxy: delegate-call failed", `-10`/other = a revert whose UTF-8 payload is
forwarded straight through.

---

## Project layout

```
upgradeable-proxy/
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
make build       # otigen build → generates pyde_gen.h, clang → ./artifacts/pyde-template.bundle/
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make help        # list all targets
```

`otigen build` runs the generator and then invokes clang itself, so you don't
compile `main.c` ahead of it (that would try to include a `pyde_gen.h` that
doesn't exist yet). The `clang-build` target is an escape hatch for when the
header already exists.

---

## Tests

`tests/contract.test.toml` exercises the proxy's OWN surface (init, the
`upgrade_to` admin gate, `transfer_admin` / `renounce_admin`, the events, and
`get_value` initial state). The full `forward(fn, calldata)` → delegate-call
into a logic twin lives in the live-devnet e2e instead — `delegate_call`
cross-target is a chain-side concern, not something the wasmtime test sandbox is
the right place to exercise. Run with `otigen test` (or `make test`).

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>

---

## License

Pick one and edit this section. Pyde itself has no preference.
