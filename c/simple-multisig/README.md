# simple-multisig (C)

A 3-signer FALCON-512 multisig in C — the canonical Pyde example of
in-contract post-quantum signature verification. You write only typed inner
functions; `otigen build` generates the rest.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`) — see [`rust/simple-multisig/`](../../rust/simple-multisig/).
> C gets the same ergonomics through **generated code**: from `otigen.toml`,
> `otigen build` writes `pyde_gen.h` with the borsh runtime, typed storage
> accessors, event emitters, and the `() -> ()` entry-dispatch shims. Your
> `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## What it does

Three signers are registered at deploy time, each identified on-chain by
`Poseidon2(falcon_pubkey)` — a 32-byte hash of the signer's FALCON-512 public
key. To move funds, a caller submits up to three `(pubkey, signature)` pairs
and a strictly-increasing `nonce`. Each pair is checked:

- the pubkey's Poseidon2 hash must be one of the registered signer IDs
  (`UnknownSigner`),
- the same signer must not be counted twice (`DuplicateSigner`),
- `falcon_verify(pk, action_digest, sig)` must succeed (`BadSignature`).

Once `threshold` distinct signatures verify, the contract pays
`transfer(target, amount)` and records the `action_digest` so it can never
replay (`AlreadyUsed`).

### Domain-separated digest binding

The `action_digest` is computed **on-chain**, never supplied by the caller:

```text
action_digest = Poseidon2(
    b"PYDE-MULTISIG-V1" ||   // domain tag
    self_address        ||   // 32 bytes — this contract
    chain_id            ||   // 8 bytes  — LE u64
    target              ||   // 32 bytes — recipient
    amount              ||   // 16 bytes — LE u128
    nonce                    // 16 bytes — LE u128
)
```

This binds every signature to `(this contract, this chain, this intent, this
nonce)`, so a signature over one transfer can't be redirected to another,
replayed across chains, or reused against a different multisig. Off-chain
wallets read the canonical digest via the `action_digest(target, amount,
nonce)` view before collecting signatures.

---

## Project layout

```
simple-multisig/
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
  for a map. Here: `threshold_get/set`, `signers_get/set` (keyed by `uint8`
  slot), `used_get/set` (keyed by the 32-byte `action_digest`). The host
  derives each slot as `Poseidon2(self || field || keys...)`; you never hash.
- **events** — `emit_Executed(target, amount, action_digest)`. topic-0 is
  `Blake3(signature)`, indexed fields (`target`, `action_digest`) become
  32-byte topics, the rest is borsh-encoded data.
- **entry dispatch** — one `() -> ()` export per `[functions.<name>]` that
  borsh-decodes the calldata tuple, calls your `__<name>_impl`, and
  borsh-encodes the result back.
- **forward declarations** of every `__<fn>_impl`, so a signature drift from
  the manifest is a hard C "conflicting types" error.

Anything the generator doesn't cover — `transfer`, `falcon_verify`,
`hash_poseidon2`, `chain_id`, `self_address` — is a raw host fn declared in
`include/pyde/host.h` (which `pyde_gen.h` includes), called directly with the
byte pointers from your `pyde_bytes` / `pyde_address` / `pyde_bytes32` values.

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
make build       # otigen build → generates pyde_gen.h, clang → ./artifacts/…
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make help        # list all targets
```

---

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- **Examples**: <https://github.com/pyde-net/otigen/tree/main/examples>

---

## License

Pick one and edit this section. Pyde itself has no preference.
