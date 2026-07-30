// simple-multisig — 2-of-3-style FALCON-512 multisig. Canonical Pyde example
// of in-contract post-quantum signature verification via the `falcon_verify`
// host fn, written on the C codegen seam (`#include "pyde_gen.h"` + typed
// `__<fn>_impl` bodies).
//
// ## Concept
//
// Three signers are registered at deploy time. Each is identified on-chain by
// Poseidon2(falcon_pubkey) — a 32-byte hash of the signer's FALCON-512 public
// key (897 bytes). Storing the hash instead of the full pubkey keeps state
// small; callers provide the full pubkey at execute time.
//
// To execute a transfer, anyone may submit one call passing up to three
// (pubkey, signature) pairs and a strictly-increasing `nonce`. Each pair is
// verified:
//
//   - The pubkey's Poseidon2 hash must equal one of the registered signer IDs
//     (UnknownSigner reverts).
//   - The same signer must not be counted twice (DuplicateSigner).
//   - falcon_verify(pk, action_digest, sig) must return 0 (BadSignature).
//
// Once the number of valid distinct sigs reaches `threshold`, the contract
// calls transfer(target, amount) and marks the `action_digest` as used
// (anti-replay).
//
// ## Domain-separated digest binding
//
// The `action_digest` is computed on-chain as:
//
//     action_digest = Poseidon2(
//         DOMAIN_TAG ||      // b"PYDE-MULTISIG-V1"
//         self_address ||    // 32 bytes — this contract's address
//         chain_id ||        // 8 bytes  — little-endian u64
//         target ||          // 32 bytes — recipient
//         amount ||          // 16 bytes — little-endian u128
//         nonce              // 16 bytes — little-endian u128
//     )
//
// binding every signature to (this contract, this chain, this intent, this
// nonce). Off-chain wallets query the same digest before signing via the
// `action_digest(target, amount, nonce)` view function.

#include "pyde_gen.h"

// Fixed number of registered signers. Three keeps the contract small + lets us
// hard-code the per-signer storage layout.
#define N_SIGNERS 3

// Domain-separation tag for the FALCON-signed digest. Bumping the suffix forces
// existing signatures to no longer validate, which is the intended migration
// path if the digest scheme changes. b"PYDE-MULTISIG-V1" — 16 bytes.
static const uint8_t DOMAIN_TAG[16] = {
    'P', 'Y', 'D', 'E', '-', 'M', 'U', 'L',
    'T', 'I', 'S', 'I', 'G', '-', 'V', '1',
};

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

// b32_eq compares two 32-byte hashes for equality.
static bool b32_eq(pyde_bytes32 a, pyde_bytes32 b) {
    for (int i = 0; i < 32; i++) {
        if (a.bytes[i] != b.bytes[i]) {
            return false;
        }
    }
    return true;
}

// addr_is_zero reports whether `a` is the all-zero (ZERO_ADDRESS) sentinel. A
// transfer to this address would burn funds, so `execute()` rejects it up-front.
static bool addr_is_zero(pyde_address a) {
    for (int i = 0; i < 32; i++) {
        if (a.bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// pubkey_hash returns Poseidon2(pubkey_bytes) — the on-chain "signer ID". 32
// bytes. Matches the test runner's @pubkey_hash:NAME DSL exactly.
static pyde_bytes32 pubkey_hash(pyde_bytes pk) {
    pyde_bytes32 h;
    hash_poseidon2(pk.ptr, (int32_t)pk.len, h.bytes);
    return h;
}

// pay pushes `amount` PYDE from this contract's balance to `to`. Reverts the
// whole frame on a non-zero return code.
static void pay(pyde_address to, __uint128_t amount) {
    uint8_t amount_bytes[16];
    for (int i = 0; i < 16; i++) {
        amount_bytes[i] = (uint8_t)(amount >> (8 * i));
    }
    int32_t rc = transfer(to.bytes, amount_bytes);
    if (rc != 0) {
        pyde_revert_str("TransferFailed");
    }
}

// ms_falcon_verify verifies a FALCON-512 signature via the chain's host fn.
static bool ms_falcon_verify(pyde_bytes pk, const uint8_t *msg, int32_t msg_len, pyde_bytes sig) {
    int32_t rc = falcon_verify(pk.ptr, msg, msg_len, sig.ptr, (int32_t)sig.len);
    return rc == 0;
}

// compute_action_digest recomputes the canonical action digest. See the file
// doc-comment for the encoding. The digest serves two roles simultaneously: the
// FALCON message every signer signs over, and the replay-protection storage key.
//
// Pre-allocated length: 16 (DOMAIN) + 32 (self) + 8 (chain) + 32 (target) + 16
// (amount) + 16 (nonce) = 120 bytes. We size the buffer exactly so the contract
// avoids any reallocation on the happy path.
static pyde_bytes32 compute_action_digest(pyde_address target, __uint128_t amount, __uint128_t nonce) {
    pyde_address self_addr = pyde_self();
    uint64_t chain_id_v = (uint64_t)chain_id();
    uint8_t buf[120];
    uint32_t o = 0;
    for (int i = 0; i < 16; i++) {
        buf[o++] = DOMAIN_TAG[i];
    }
    for (int i = 0; i < 32; i++) {
        buf[o++] = self_addr.bytes[i];
    }
    for (int i = 0; i < 8; i++) {
        buf[o++] = (uint8_t)(chain_id_v >> (8 * i));
    }
    for (int i = 0; i < 32; i++) {
        buf[o++] = target.bytes[i];
    }
    for (int i = 0; i < 16; i++) {
        buf[o++] = (uint8_t)(amount >> (8 * i));
    }
    for (int i = 0; i < 16; i++) {
        buf[o++] = (uint8_t)(nonce >> (8 * i));
    }
    pyde_bytes32 out;
    hash_poseidon2(buf, 120, out.bytes);
    return out;
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

// init configures the multisig once at deploy time.
//
// Reverts on duplicate signer hashes — a multisig registered with
// (alice, alice, bob) would silently degrade threshold = 2 to a 1-of-2 (alice
// alone covers two slots). That's not a multisig.
void __init_impl(uint8_t threshold, pyde_bytes32 signer0, pyde_bytes32 signer1, pyde_bytes32 signer2) {
    if (threshold < 1 || threshold > N_SIGNERS) {
        pyde_revert_str("BadThreshold");
    }
    if (b32_eq(signer0, signer1) || b32_eq(signer0, signer2) || b32_eq(signer1, signer2)) {
        pyde_revert_str("DuplicateSignerInInit");
    }
    threshold_set(threshold);
    signers_set(0, signer0);
    signers_set(1, signer1);
    signers_set(2, signer2);
}

// execute performs a transfer once `threshold` valid signatures are presented.
//
// Each signer slot consists of a (pubkey, signature) pair. An empty pubkey
// (len == 0) means "this slot is unused" — the caller may pass any combination
// as long as enough distinct signers verify.
//
// The on-chain `action_digest` binds every signature to (self_address,
// chain_id, target, amount, nonce).
void __execute_impl(
    pyde_address target,
    __uint128_t amount,
    __uint128_t nonce,
    pyde_bytes pk1,
    pyde_bytes sig1,
    pyde_bytes pk2,
    pyde_bytes sig2,
    pyde_bytes pk3,
    pyde_bytes sig3
) {
    if (addr_is_zero(target)) {
        pyde_revert_str("multisig: execute to zero address");
    }

    // Recompute the digest on-chain so signatures are bound to (this contract,
    // this chain, this intent, this nonce). The caller never supplies the
    // digest — there's nothing for them to lie about.
    pyde_bytes32 action_digest = compute_action_digest(target, amount, nonce);

    if (used_get(action_digest)) {
        pyde_revert_str("AlreadyUsed");
    }

    uint8_t threshold = threshold_get();
    pyde_bytes32 signers[3] = {
        signers_get(0),
        signers_get(1),
        signers_get(2),
    };

    pyde_bytes slot_pk[3] = {pk1, pk2, pk3};
    pyde_bytes slot_sig[3] = {sig1, sig2, sig3};

    // Bitmap of which signer indices have already counted — prevents
    // double-counting the same signer across slots.
    uint8_t seen_signers = 0;
    uint8_t valid = 0;

    for (int s = 0; s < 3; s++) {
        pyde_bytes pk = slot_pk[s];
        pyde_bytes sig = slot_sig[s];
        if (pk.len == 0) {
            continue;
        }
        pyde_bytes32 pk_hash = pubkey_hash(pk);
        int signer_idx = -1;
        for (int i = 0; i < N_SIGNERS; i++) {
            if (b32_eq(signers[i], pk_hash)) {
                signer_idx = i;
                break;
            }
        }
        if (signer_idx < 0) {
            pyde_revert_str("UnknownSigner");
        }

        uint8_t bit = (uint8_t)(1u << signer_idx);
        if (seen_signers & bit) {
            pyde_revert_str("DuplicateSigner");
        }
        seen_signers |= bit;

        if (!ms_falcon_verify(pk, action_digest.bytes, 32, sig)) {
            pyde_revert_str("BadSignature");
        }
        valid++;
    }

    if (valid < threshold) {
        pyde_revert_str("InsufficientApprovals");
    }

    // Anti-replay: mark BEFORE the transfer so a re-entrant transfer can't
    // double-spend by re-calling execute().
    used_set(action_digest, true);

    pay(target, amount);

    emit_Executed(target, amount, action_digest);
}

// get_threshold is a view: threshold required for execution.
uint8_t __get_threshold_impl(void) {
    return threshold_get();
}

// is_used is a view: is this action digest already executed?
bool __is_used_impl(pyde_bytes32 action_digest) {
    return used_get(action_digest);
}

// action_digest is a view: the canonical digest the multisig will recompute +
// bind sigs to for execute(target, amount, nonce). Off-chain wallets call this
// before collecting signatures so every signer signs the exact same bytes the
// chain will reject otherwise.
pyde_bytes32 __action_digest_impl(pyde_address target, __uint128_t amount, __uint128_t nonce) {
    return compute_action_digest(target, amount, nonce);
}
