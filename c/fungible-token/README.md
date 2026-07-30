# fungible-token (C)

The PTS-F reference token (`pts-f/1`, PIP-0005) in its default extension
configuration — expiring delta-only allowances, settle-then-notify
`transfer_call`, role-gated supply — written in C against the otigen C seam.

> This is the same contract as
> [`rust/fungible-token`](../../rust/fungible-token/) and
> [`go/fungible-token`](../../go/fungible-token/), byte-for-byte
> interchangeable on-chain. Rust authors get the macro substrate
> (`#[pyde::entry]`, `declare_storage!`, `declare_events!`); C gets the same
> ergonomics through **generated code**. From `otigen.toml`, `otigen build`
> writes `pyde_gen.h` with the borsh runtime, typed storage accessors, event
> emitters, custom-type + vec codecs, and the `() -> ()` entry-dispatch shims.
> Your `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## Project layout

```
fungible-token/
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # metadata + [state] + [events] + [functions] + [types]
├── main.c                 # YOUR CONTRACT CODE — the __<fn>_impl bodies
├── pyde_gen.h             # GENERATED (committed) — regenerated each build
├── include/pyde/host.h    # the raw pyde::* host-fn declarations
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

`pyde_gen.h` is generated from `otigen.toml` on every `otigen build` and
committed like a `.pb.go` — never edit it by hand; edit the manifest or your
`__<fn>_impl` bodies. The `[state]`, `[events]`, `[functions]`, and `[types]`
tables are byte-identical to the Rust manifest — only `[contract.lang]`
differs, so the two contracts share the same conformance test suite.

---

## The seam

`otigen build` reads `otigen.toml` and generates, into `pyde_gen.h`:

- **typed storage** — `<field>_get/set` for a scalar, `<field>_get/set/delete`
  for a map. The host derives each slot as `Poseidon2(self || field || keys...)`
  from the field name and borsh-encoded keys; you never hash a slot. This
  contract's two-key allowance maps become `allowance_amounts_get(owner,
  spender)` / `allowance_expiries_set(owner, spender, v)`.
- **custom types** — `[types.TokenInfo]` becomes a C `struct TokenInfo` plus
  its borsh `TokenInfo_encode` / `TokenInfo_decode` pair.
- **vec codecs** — `vec(address)` → `pyde_vec_Address`, `vec(uint128)` →
  `pyde_vec_U128`, `vec(uint8)` → `pyde_vec_U8`, each with an
  `__encodeVec<Tag>` / `__decodeVec<Tag>` helper.
- **events** — `emit_<Name>(fields...)`. topic-0 is `Blake3(signature)`,
  indexed fields become 32-byte topics, the rest are borsh-encoded data.
- **entry dispatch** — one `() -> ()` export per `[functions.<name>]` that
  borsh-decodes the calldata tuple, calls your `__<name>_impl`, and
  borsh-encodes the result back through `pyde::return`.

You write only the logic. Two C-isms are the only departures from a literal
translation of the Rust reference:

- Rust `checked_add` / `saturating_sub` become explicit unsigned
  overflow/underflow checks (`sum < a` → revert `token:overflow`).
- Returning a `Vec<T>` allocates through the generated `__pyde_alloc`
  (`balance_of_batch` fills a `pyde_vec_U128`).

The one thing the codegen doesn't wrap is the outbound
`on_token_received` notification in `transfer_call` — that is a raw
`cross_call` (declared in `include/pyde/host.h`), with args marshalled via the
generated `pyde_enc` / `pyde_dec` borsh helpers.

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

`transfer_call`'s happy path needs a real receiver contract (the token-vault
example is the reference receiver); this suite pins its failure paths. Every
other surface — the `token:*` revert codes, the zero-sentinel `Transfer`
family, delta-only expiring allowances, the CAS primitive, role gating, and
the recipient guards — is exercised here.

---

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- **PTS-F standard (PIP-0005)**: <https://book.pyde.network>

---

## License

Pick one and edit this section. Pyde itself has no preference.
