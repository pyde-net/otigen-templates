# simple-multisig

3-signer FALCON-512 multisig — the canonical Pyde example of **in-contract post-quantum signature verification**. Demonstrates `pyde::falcon_verify`, `bytes` typed-args for the variable-length pubkeys + sigs, and the `@pubkey:NAME` / `@sig:NAME:args.IDX` test-side DSL.

> This example predates the macro substrate (otigen #116–#133). It
> exercises the raw `pyde::*` host-fn pattern + manual entry-point
> declarations rather than `#[pyde::entry]` / `declare_storage!()`.
> For a multisig contract on the current substrate, see the
> acceptance-suite [`multisig-wallet`](../multisig-wallet/) — it
> ships the M-of-N + value-forwarding cross-call shape on the
> macro substrate. `simple-multisig` is kept as the canonical
> reference for the **in-contract FALCON verification** pattern,
> which `multisig-wallet` doesn't cover (it uses on-chain approval
> bookkeeping instead of off-chain signed claims).

## What it does

| Function | Purpose |
|---|---|
| `init(threshold, h1, h2, h3)` | Deploy-time: stores three signer-ID hashes (`Poseidon2(pubkey)`) + the required threshold (1..=3). |
| `execute(target, amount, action_hash, pk1, sig1, pk2, sig2, pk3, sig3)` | Verify each non-empty `(pubkey, signature)` pair; once `threshold` distinct signers' sigs verify against `action_hash`, transfer + mark the hash as used. |
| `get_threshold()` | View — current required threshold. |
| `is_used(action_hash)` | View — has this `action_hash` already executed? |

Events:
- `Executed(target indexed, amount, action_hash indexed)` — emitted once per successful execution.

Reverts (six paths):
- `BadThreshold` — `init` called with threshold ∉ 1..=3.
- `AlreadyUsed` — `action_hash` was already executed (anti-replay).
- `UnknownSigner` — a provided pubkey's `Poseidon2` doesn't match any registered signer ID.
- `DuplicateSigner` — the same registered signer was counted twice in one call.
- `BadSignature` — `falcon_verify` rejected (tampered message, wrong sig, malformed bytes).
- `InsufficientApprovals` — valid distinct sigs < `threshold`.

## Build + test

```bash
make build       # cargo --release + otigen build
make test        # otigen test
make test-vvvv   # full Foundry-style trace
```

9 tests cover: happy paths (2-of-3, 3-of-3), each revert path, anti-replay.

## What this example demonstrates

### 1. In-contract FALCON-512 verification

The contract imports `pyde::falcon_verify` and calls it per-(pubkey, signature) pair. The test runner mocks it via `pyde_crypto::falcon::falcon_verify` — the real PQ verifier the chain itself uses, so a signature that passes `otigen test` will pass on-chain.

### 2. FALCON keypairs declared in tests

`[accounts]` accepts `keypair = "falcon512"`:

```toml
[accounts]
alice = { keypair = "falcon512" }
```

The runner generates a fresh 897-byte pubkey + 1281-byte secret at plan time and caches them for the test run. Tests reference the pubkey or produce signatures via DSL prefixes:

```toml
args = [
  "recipient",                       # 0: address
  "500",                             # 1: uint128
  "0xdead...",                       # 2: bytes32 — client-computed action hash
  "@pubkey:alice", "@sig:alice:args.2",   # alice's pubkey + sig over arg 2
  "@pubkey:bob",   "@sig:bob:args.2",
  "0x", "0x",                        # carol slot unused (empty bytes)
]
```

### 3. Variable-length `bytes` typed-args

Each `bytes` declared input in `otigen.toml` expands to **two** wasm i32 params (`ptr`, `len`). FALCON pubkeys (897 B) and signatures (~660-690 B) flow through this — no calldata-blob hackery, no fixed-array workarounds.

### 4. `Poseidon2(pubkey)` as the on-chain signer ID

Storing the full 897-byte pubkey on-chain would be wasteful. Instead the contract stores `Poseidon2(pubkey_bytes)` (32 bytes) per signer. Callers provide the full pubkey at execute time; the contract recomputes the hash and matches against its registered set.

The test DSL has a matching `@pubkey_hash:NAME` form for the `init` call:

```toml
args = ["2", "@pubkey_hash:alice", "@pubkey_hash:bob", "@pubkey_hash:carol"]
```

### 5. `action_hash` as the canonical replay key

Real multisigs (Gnosis Safe, etc.) sign a domain-separated digest of the full intent. The off-chain wallet UI assembles this digest and feeds it to each signer. The on-chain contract verifies sigs over the digest and uses it as both the message-to-verify AND the replay-protection key (refuses to execute the same hash twice).

We do the same: `action_hash` is a 32-byte client-provided digest. The contract has no opinion about what's inside — that's the off-chain wallet's job.

## Sample output (`make test-vvvv` on `two_of_three_valid_sigs_executes`)

```text
✓ two_of_three_valid_sigs_executes (0.67 ms)
  Events:
    [0] topic0=0xa43d...6528  topics=3  data=16 bytes    ← Executed
  Calls:
    [0] init(2, 0x..., 0x..., 0x...) -> 0
    [1] execute(recipient, 500, 0x4141..., 897 B, 666 B, 897 B, 666 B, 0 B, 0 B) -> 0
    [2] is_used(0x4141...) -> 1
  Storage diff:
    0x...: <unset> → 0x02     (threshold = 2)
    0x...: <unset> → 0x...    (signers[0] = Poseidon2(alice_pk))
    0x...: <unset> → 0x...    (signers[1])
    0x...: <unset> → 0x...    (signers[2])
    0x...: <unset> → 0x01     (used[action_hash] = true)
```

## Extending this contract

Common additions, in rough order of complexity:

1. **Variable signer set** — store `signer_count` in a slot, signers in `signers[0..signer_count]`. Add `add_signer` / `remove_signer` requiring threshold approval (the multisig governs its own membership).
2. **Threshold change** — `change_threshold(new)` requiring threshold approval.
3. **Generic actions** — accept `(target, calldata)` instead of `(target, amount)` and use `cross_call` so the multisig can govern any contract.
4. **Time-bounded signatures** — include a `deadline: u64` field in the message; reject if `wave_timestamp() > deadline`. Prevents stale sigs being replayed against future state.

## Architecture references

- `HOST_FN_ABI_SPEC §7.7` — `falcon_verify` signature + gas: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>
- `OTIGEN_TEST_SPEC §6.3` — runner mocks including FALCON: <https://book.pyde.network/companion/OTIGEN_TEST_SPEC>
- `WASM_AUTHOR_GUIDE §7` (field-keyed storage) and §12 (FALCON-verify pattern): <https://book.pyde.network/companion/WASM_AUTHOR_GUIDE>
- `SDK_AUTHOR_GUIDE` — porting a contract-side SDK to another language: <https://book.pyde.network/companion/SDK_AUTHOR_GUIDE>
- `examples/multisig-wallet/` — M-of-N multisig on the macro substrate (on-chain approval bookkeeping; complements this contract's off-chain-signed pattern)

## License

Public domain. Use as a starting point for your own multisig; the patterns here generalise to most threshold-signature contracts.
