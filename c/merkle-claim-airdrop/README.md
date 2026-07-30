# merkle-claim-airdrop (C)

A Merkle-tree airdrop claim contract in C. An off-chain process precomputes a
tree whose leaves are `Blake3("PYDE_LEAF" ‖ claimant ‖ amount_be)`, the admin
commits the 32-byte root once, and claimants then prove inclusion with their
sibling path to pull their allocation from the contract's balance. The whole
allocation costs ONE 32-byte root on-chain; verification is `O(log n)` per
claim.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `declare_events!`) — see [`rust/merkle-claim-airdrop/`](../../rust/merkle-claim-airdrop/).
> C gets the same ergonomics through **generated code**: from `otigen.toml`,
> `otigen build` writes `pyde_gen.h` with the borsh runtime, typed storage
> accessors, event emitters, and the `() -> ()` entry-dispatch shims. Your
> `main.c` includes it and defines the `__<fn>_impl` bodies.

For depth (full ABI, error codes, gas table, threat model), see the protocol
book at <https://book.pyde.network>.

---

## Tree encoding (match this off-chain when you build the tree)

```
Leaf:  Blake3("PYDE_LEAF" ‖ claimant_addr_32 ‖ amount_u128_be_16)
Node:  Blake3("PYDE_NODE" ‖ left_hash_32 ‖ right_hash_32)
Proof: [position_byte ‖ sibling_hash_32] repeated per level.
       position 0 ⇒ current hash is the LEFT child (sibling on the right);
       position 1 ⇒ current hash is the RIGHT child (sibling on the left).
```

Amount is **big-endian u128** (16 bytes) — the standard Solidity-style airdrop
encoding, so community tree generators interoperate. Domain-separation tags
(`PYDE_LEAF`, `PYDE_NODE`) keep a leaf hash from ever colliding with an
internal node hash. A C contract, a Rust contract, and a Go contract with this
manifest are byte-for-byte interchangeable — the same tree verifies against all
three.

---

## Project layout

```
merkle-claim-airdrop/
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

## Entry points

- `init()` — constructor; records the caller as admin (gates `set_root`).
- `fund()` — payable; accept PYDE into the contract, emit `Funded`.
- `set_root(bytes32)` — admin-only, one-shot; commit the merkle root.
- `claim(uint128 amount, bytes proof)` — verify inclusion, transfer the
  proven amount to the caller, emit `Claim`.
- `is_claimed(address) -> bool` — per-account claim flag (view).
- `total_claimed() -> uint128` — cumulative claimed (view).
- `merkle_root_first_byte() -> int64` — `-1` if unset, else first root byte
  (view probe).

The hashing helpers (`leaf_hash`, `node_hash`, `walk_proof`) call the raw
`hash_blake3` host fn declared in `include/pyde/host.h`; `pay()` uses the raw
`transfer` host fn with a little-endian 16-byte amount.

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

`otigen build` runs the generator and then invokes clang itself, so you don't
compile `main.c` ahead of it (that would try to include a `pyde_gen.h` that
doesn't exist yet). The `clang-build` target is an escape hatch for when the
header already exists.

---

## Where to find more

- **Otigen Toolchain Guide**: <https://book.pyde.network/otigen>
- **Host Function ABI v1.0**: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- **Otigen Test Framework Spec**: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- **Examples**: <https://github.com/pyde-net/otigen/tree/main/examples>
