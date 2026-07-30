# nft-token (C)

The **PTS-N reference NFT** (`pts-n/1`, PIP-0005 §12) in C. Per-id owner slots,
settle-then-notify `transfer_call`, on-chain token URIs, role-gated mint. You
write only typed inner functions; `otigen build` generates the rest.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`) — see [`otigen-templates/rust/nft-token/`](../../rust/nft-token/).
> C gets the same ergonomics through **generated code**: from `otigen.toml`,
> `otigen build` writes `pyde_gen.h` with the borsh runtime, typed storage
> accessors, event emitters, and the `() -> ()` entry-dispatch shims. Your
> `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## Project layout

```
nft-token/
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

- **typed storage** — `<field>_get/set` for a scalar, `<field>_get/set/delete`
  for a map (multi-key: `operator_approvals_get(owner, operator)`). The host
  derives each slot as `Poseidon2(self || field || keys...)` from the field
  name and borsh-encoded keys; you never hash a slot.
- **events** — `emit_<Name>(fields...)`. topic-0 is `Blake3(signature)`
  (computed via the host `hash_blake3`, so it matches Rust/Go byte-for-byte),
  indexed fields become 32-byte topics, the rest are borsh-encoded data.
- **entry dispatch** — one `() -> ()` export per `[functions.<name>]` that
  borsh-decodes the calldata tuple, calls your `__<name>_impl`, and
  borsh-encodes the result back through `pyde::return`.
- **forward declarations** of every `__<fn>_impl`. A signature in `main.c`
  that stops matching the manifest is a hard C "conflicting types" error —
  the compiler is the drift guard.

You write only the logic — the `pts-n/1` surface: `init`, the views
(`owner_of`, `balance_of`, `token_uri`, …), `transfer_from`/`transfer_call`,
`approve`/`set_approval_for_all`, and `mint`/`burn`/`set_minter`/`set_manager`.

---

## Reaching further — `transfer_call`

`transfer_call` is the one entry the generated accessors don't fully cover: it
settles the move, emits `Transfer`, then notifies the recipient via the raw
`cross_call` host fn (declared in `include/pyde/host.h`, which `pyde_gen.h`
includes). It borsh-marshals `(operator, from, token_id, data)` with the
generated `pyde_enc` helpers, invokes `on_nft_received`, and reverts
`token:bad_receiver` unless the receiver returns `ACK_NFT`. Two C-isms carried
from Rust: `checked_add` → a manual `== UINT64_MAX` overflow check + revert;
`Vec<T>` returns → `__pyde_alloc`.

Full ABI: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>.

---

## Prerequisites

C needs a clang with the **wasm32** backend AND **wasm-ld**. Apple's bundled
clang lacks both. Install LLVM + lld via Homebrew:

```bash
brew install llvm lld
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
```

On Linux: `apt install clang lld` ships a wasm32-capable clang + wasm-ld out of
the box. `make check-tools` verifies it.

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

`tests/contract.test.toml` pins the `pts-n/1` surface: canonical `token:*`
codes, the zero-sentinel `Transfer` family, monotonic never-recycled ids, the
three authorization paths (owner / per-id approval / operator), atomic approval
clearing on transfer, role gating, and the recipient guards (including the
token's own address via `__contract__`). `transfer_call` is exercised
end-to-end in the token-vault receiver suite — the toml runner cannot yet
encode `vec(uint8)` arguments.

```toml
[[tests.calls]]
function = "mint"
from     = "alice"
args     = ["bob", "ipfs://relic-1"]
expect.return_value = "1"
expect.events = [ { name = "Transfer", to = "bob" } ]
```

Full spec: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>.

---

## License

Pick one and edit this section. Pyde itself has no preference.
