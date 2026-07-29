#!/usr/bin/env bash
# simple-multisig live e2e — smoke test for the wire-level path.
#
# Verifies:
#   - the contract deploys cleanly with the
#     `init(threshold, signer0_hash, signer1_hash, signer2_hash)`
#     constructor,
#   - `get_threshold()` reads back the configured 2-of-3 threshold,
#   - the `action_digest(target, amount, nonce)` view function
#     returns a non-zero canonical digest (the byte string
#     off-chain wallets are supposed to FALCON-sign before calling
#     `execute`),
#   - resolve-by-name works for the `[contract].name = "simple-multisig"`
#     entry in the on-chain registry.
#
# Full FALCON-512 verification + dedup guards + replay protection +
# digest-binding regression coverage lives in
# `examples/simple-multisig/tests/contract.test.toml` — 14 cases via
# the in-process sandbox. The Tier 1 path skips raw FALCON sigs
# because constructing them from bash would require a Python wrapper
# around `pyde_crypto::falcon` and that complexity belongs in the
# Tier 2 path which already runs the FALCON host fn directly.
#
# Run from the otigen repo root:
#   bash examples/simple-multisig/tests/run_e2e.sh

set -euo pipefail
# shellcheck source=../../_e2e_lib.sh
source examples/_e2e_lib.sh

start_devnet
import_devnet_wallets
export_devnet_addresses

echo "→ building + bundling simple-multisig"
build_bundle examples/simple-multisig/otigen.toml

# Constructor args: threshold=2, three distinct 32-byte signer hashes.
# We use 0x01..., 0x02..., 0x03... as the signer IDs — the contract
# stores them verbatim; FALCON-pubkey-hash construction lives in the
# Tier 2 sandbox path.
INIT_ARG_HEX=$(python3 - <<'PY'
import sys
threshold = (2).to_bytes(1, "little")
s0 = b"\x01" * 32
s1 = b"\x02" * 32
s2 = b"\x03" * 32
sys.stdout.write((threshold + s0 + s1 + s2).hex())
PY
)

echo "→ deploying with init args"
deploy_contract_with_init_args examples/simple-multisig/otigen.toml devnet-0 "$INIT_ARG_HEX"
echo "  addr: $DEPLOY_ADDR"

echo "→ smoke checks"

# Threshold round-trips through the view. `otigen call` view output is
# `  Return:   <decoded>` (renderer in crates/otigen-cli/src/commands/
# call.rs::render_view_return) — match that, not the pre-PR-#234 word.
# `--config` is required alongside `--rpc-url` so the call command sees
# the function's declared outputs in otigen.toml and decodes the return
# blob (without it the renderer falls back to raw hex `0x02` which the
# integer regex misreads as `0`).
CFG=examples/simple-multisig/otigen.toml
threshold=$("${OTIGEN[@]}" call --config "$CFG" "$DEPLOY_ADDR" get_threshold \
    --rpc-url "$RPC" 2>&1 | grep -oE 'Return:[[:space:]]+[0-9]+' | awk '{print $2}')
if [[ "$threshold" != "2" ]]; then
    echo "ERROR: get_threshold returned '$threshold' (expected 2)" >&2
    exit 1
fi
echo "  ✓ get_threshold = 2"

# `action_digest(devnet-1, 1_000_000_000, 1)` returns a 32-byte hash.
# We don't assert the exact bytes here (chain-id + contract-address
# both feed in and we'd duplicate the on-chain hashing scheme); we
# just verify the call succeeds and returns 32 bytes.
ACTION_DIGEST_ARGS_HEX=$(python3 - <<PY
import sys
addr = bytes.fromhex("${DEVNET_ADDR_1#0x}")
amount = (1_000_000_000).to_bytes(16, "little")
nonce = (1).to_bytes(16, "little")
sys.stdout.write((addr + amount + nonce).hex())
PY
)

digest_call=$("${OTIGEN[@]}" call --config "$CFG" "$DEPLOY_ADDR" action_digest \
    --args "0x${ACTION_DIGEST_ARGS_HEX}" \
    --rpc-url "$RPC" 2>&1)
# Engine returns a 32-byte hex blob on the `Return:` line. The call
# header also echoes the contract address (`Call … on 0x…`, `Target:
# 0x…`) so we filter to the Return line first; otherwise the grep
# would pick up the contract address and the digest check would
# silently pass on the wrong bytes.
digest=$(grep -E '^[[:space:]]*Return:' <<<"$digest_call" | grep -oE '0x[0-9a-f]{64}' | head -1)
if [[ -z "$digest" || "$digest" == "0x$(printf '0%.0s' {1..64})" ]]; then
    echo "ERROR: action_digest returned a zero / unparseable digest" >&2
    echo "$digest_call" >&2
    exit 1
fi
echo "  ✓ action_digest = $digest"

# Resolve-by-name reaches the same deployed address.
inspect_out=$("${OTIGEN[@]}" inspect --config "$CFG" simple-multisig --rpc-url "$RPC" 2>&1 | head -3)
if ! grep -q "$DEPLOY_ADDR" <<<"$inspect_out"; then
    echo "ERROR: inspect-by-name didn't resolve to $DEPLOY_ADDR" >&2
    echo "$inspect_out" >&2
    exit 1
fi
echo "  ✓ inspect by-name resolves to $DEPLOY_ADDR"

echo "→ all simple-multisig smoke checks passed"
