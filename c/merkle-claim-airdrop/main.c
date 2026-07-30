// merkle-claim-airdrop — Merkle-tree-based claim contract. Canonical Pyde
// example of off-chain commitment + on-chain verification: an off-chain
// process precomputes a tree where each leaf is
// Blake3(LEAF_TAG ‖ claimant_addr ‖ amount_be), the constructor commits the
// root via `set_root(root)` (after `init()` records the admin), and claimants
// then prove inclusion with their leaf + sibling-path to claim their
// allocation. Successful claims transfer the proven `amount` of PYDE from the
// contract's balance to the claimant.
//
// ## Tree encoding
//
//   Leaf:  Blake3("PYDE_LEAF" ‖ claimant_addr_32 ‖ amount_u128_be_16)
//   Node:  Blake3("PYDE_NODE" ‖ left_hash_32 ‖ right_hash_32)
//   Proof: [position_byte ‖ sibling_hash_32] repeated per level.
//          position 0 ⇒ current hash is the LEFT child (sibling on the right);
//          position 1 ⇒ current hash is the RIGHT child (sibling on the left).
//   Amount is BIG-ENDIAN u128 (16 bytes) — the standard Solidity-style
//   airdrop encoding, so community tree generators interoperate.
//
// Domain-separation tags (LEAF_TAG, NODE_TAG) prevent a leaf hash from
// colliding with an internal node hash.
//
// You write only the typed inner functions (`__<fn>_impl`) plus otigen.toml.
// `otigen build` reads [functions.*] / [state] / [events] and generates
// `pyde_gen.h`: the () -> () export shims, the typed storage accessors, and the
// event emitters. Every __<fn>_impl below is forward-declared in that header,
// so a signature drift is a hard C compile error rather than a mis-decoded
// contract.

#include "pyde_gen.h"

// ────────────────────────────────────────────────────────────────
// Constants
// ────────────────────────────────────────────────────────────────

// Domain-separation tag for leaf hashing. Prepended to
// `(addr ‖ amount_be)` so a leaf hash can't collide with any internal node
// hash (which uses NODE_TAG).
static const uint8_t LEAF_TAG[9] = {'P', 'Y', 'D', 'E', '_', 'L', 'E', 'A', 'F'};

// Domain-separation tag for internal node hashing.
static const uint8_t NODE_TAG[9] = {'P', 'Y', 'D', 'E', '_', 'N', 'O', 'D', 'E'};

// 32-byte all-zero address. Used as the sentinel for "admin not yet
// recorded" — `init()` rejects a second call by checking whether the admin
// slot is still zero.
static const pyde_address ZERO_ADDRESS = {{0}};

// Cap at 32 levels × 33 bytes = 1056 — supports trees up to 2^32 leaves, far
// beyond any realistic airdrop.
#define MAX_PROOF_BYTES (33u * 32u)

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

// addr_is_zero mirrors `admin == ZERO_ADDRESS`.
static bool addr_is_zero(pyde_address a) {
    return pyde_addr_eq(a, ZERO_ADDRESS);
}

// bytes32_eq is a constant-32 compare — the `computed_root != stored_root`
// check (there's no generated helper for bytes32 equality).
static bool bytes32_eq(pyde_bytes32 a, pyde_bytes32 b) {
    for (int i = 0; i < 32; i++) {
        if (a.bytes[i] != b.bytes[i]) {
            return false;
        }
    }
    return true;
}

// tx_value_u128 reads the 16-byte little-endian value attached to this call
// into a __uint128_t (the raw `tx_value` host fn writes bytes, not a scalar).
static __uint128_t tx_value_u128(void) {
    uint8_t buf[16];
    tx_value(buf);
    __uint128_t v = 0;
    for (int i = 15; i >= 0; i--) {
        v = (__uint128_t)((v << 8) | (__uint128_t)buf[i]);
    }
    return v;
}

// pay pushes `amount` PYDE from this contract's balance to `to`. Reverts the
// whole frame on a non-zero return code — `claim()` relies on this to
// propagate "contract is underfunded" failures cleanly so the storage-side
// claim flag rolls back atomically and the claimant can retry after a top-up.
static void pay(pyde_address to, __uint128_t amount) {
    uint8_t amount_bytes[16];
    __uint128_t v = amount;
    for (int i = 0; i < 16; i++) {
        amount_bytes[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    int32_t rc = transfer(to.bytes, amount_bytes);
    if (rc != 0) {
        pyde_revert_str("merkle: TransferFailed");
    }
}

// ────────────────────────────────────────────────────────────────
// Merkle leaf + node hashing
// ────────────────────────────────────────────────────────────────

// leaf_hash computes a leaf hash from `(claimant, amount)`.
static pyde_bytes32 leaf_hash(pyde_address claimant, __uint128_t amount) {
    // 9 (tag) + 32 (addr) + 16 (amount) = 57 bytes
    uint8_t buf[57];
    for (int i = 0; i < 9; i++) {
        buf[i] = LEAF_TAG[i];
    }
    for (int i = 0; i < 32; i++) {
        buf[9 + i] = claimant.bytes[i];
    }
    // amount as big-endian u128 (most-significant byte first).
    __uint128_t v = amount;
    for (int i = 15; i >= 0; i--) {
        buf[41 + i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    pyde_bytes32 out;
    hash_blake3(buf, 57, out.bytes);
    return out;
}

// node_hash computes an internal node hash from its two children.
static pyde_bytes32 node_hash(pyde_bytes32 left, pyde_bytes32 right) {
    // 9 (tag) + 32 + 32 = 73 bytes
    uint8_t buf[73];
    for (int i = 0; i < 9; i++) {
        buf[i] = NODE_TAG[i];
    }
    for (int i = 0; i < 32; i++) {
        buf[9 + i] = left.bytes[i];
    }
    for (int i = 0; i < 32; i++) {
        buf[41 + i] = right.bytes[i];
    }
    pyde_bytes32 out;
    hash_blake3(buf, 73, out.bytes);
    return out;
}

// walk_proof folds `leaf` upward through each proof step, applying each step's
// sibling according to its position byte. Returns the computed root.
//
// `proof.len` must be a multiple of 33; this is checked by the caller before
// invocation.
static pyde_bytes32 walk_proof(pyde_bytes32 leaf, pyde_bytes proof) {
    pyde_bytes32 hash = leaf;
    uint32_t i = 0;
    while (i < proof.len) {
        uint8_t position = proof.ptr[i];
        pyde_bytes32 sibling;
        for (int j = 0; j < 32; j++) {
            sibling.bytes[j] = proof.ptr[i + 1 + j];
        }
        if (position == 0) {
            // Current hash is on the left.
            hash = node_hash(hash, sibling);
        } else {
            // Current hash is on the right.
            hash = node_hash(sibling, hash);
        }
        i += 33;
    }
    return hash;
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

// init is the constructor — records the caller as the contract admin. The
// admin is the only address allowed to call `set_root` later. Reverts if
// called a second time (`admin` slot already populated).
//
// Tagged ["constructor"] in the manifest so the chain rejects any post-deploy
// invocation. The in-source guard is defence in depth + lets the test runner
// exercise the second-call revert path.
void __init_impl(void) {
    if (!addr_is_zero(admin_get())) {
        pyde_revert_str("merkle: already initialized");
    }
    admin_set(pyde_caller());
}

// fund is the payable funding entry — accept PYDE into the contract's own
// balance. Anyone can fund. Emits Funded(from, amount) for off-chain
// accounting. Refuses zero-value calls so accidental empty calls are caught
// loudly instead of emitting a meaningless event.
void __fund_impl(void) {
    __uint128_t amount = tx_value_u128();
    if (amount == 0) {
        pyde_revert_str("merkle: fund requires non-zero value");
    }
    pyde_address from = pyde_caller();
    emit_Funded(from, amount);
}

// set_root commits the merkle root. Admin-only — only the address recorded by
// `init()` may set the root. Subsequent calls (even by the admin) revert with
// RootAlreadySet, so the root is locked once committed.
void __set_root_impl(pyde_bytes32 root) {
    pyde_address admin = admin_get();
    if (addr_is_zero(admin)) {
        pyde_revert_str("merkle: not initialized");
    }
    if (!pyde_addr_eq(pyde_caller(), admin)) {
        pyde_revert_str("merkle: caller is not admin");
    }
    if (root_set_get()) {
        pyde_revert_str("RootAlreadySet");
    }
    merkle_root_set(root);
    root_set_set(true);
}

// claim proves (caller, amount) ∈ tree by recomputing the root from
// leaf_hash(caller, amount) plus the supplied path, then transfers `amount`
// PYDE from the contract's balance to the claimant.
//
// State + event happen BEFORE the value transfer (checks-effects-
// interactions). Pyde has no implicit reentrancy — only cross_call can
// re-enter — and pay() resolves through the native transfer host fn rather
// than cross_call, so re-entry is impossible.
//
// Reverts:
//   - RootNotSet — set_root hasn't been called yet.
//   - AlreadyClaimed — caller has already claimed.
//   - MalformedProof — proof.len % 33 != 0.
//   - ProofTooLong — cap at 32 levels.
//   - InvalidProof — recomputed root doesn't match the stored root.
//   - "merkle: TransferFailed" — the contract's balance is short of the
//     proven amount. State changes are rolled back atomically with the revert,
//     so the `claimed` flag does not stick and the claimant can retry once the
//     contract has been topped up.
void __claim_impl(__uint128_t amount, pyde_bytes proof) {
    if (!root_set_get()) {
        pyde_revert_str("RootNotSet");
    }

    pyde_address claimant = pyde_caller();
    if (claimed_get(claimant)) {
        pyde_revert_str("AlreadyClaimed");
    }

    // Proof length must be a multiple of 33 (each step = 1B position + 32B
    // sibling). Empty proofs are allowed — degenerate single-leaf trees verify
    // directly against the leaf hash.
    if (proof.len % 33u != 0u) {
        pyde_revert_str("MalformedProof");
    }

    if (proof.len > MAX_PROOF_BYTES) {
        pyde_revert_str("ProofTooLong");
    }

    pyde_bytes32 leaf = leaf_hash(claimant, amount);
    pyde_bytes32 computed_root = walk_proof(leaf, proof);
    pyde_bytes32 stored_root = merkle_root_get();

    if (!bytes32_eq(computed_root, stored_root)) {
        pyde_revert_str("InvalidProof");
    }

    claimed_set(claimant, true);
    __uint128_t prev_total = total_claimed_get();
    __uint128_t new_total = prev_total + amount;
    if (new_total < prev_total) {
        pyde_revert_str("merkle: total claimed overflow");
    }
    total_claimed_set(new_total);

    emit_Claim(claimant, amount);

    // Actual PYDE transfer to the claimant. If the contract is underfunded the
    // host fn returns non-zero and pay() reverts the whole frame —
    // `claimed` rolls back with it, so the user can retry after a top-up.
    pay(claimant, amount);
}

// is_claimed is a view: false if unclaimed, true if claimed.
bool __is_claimed_impl(pyde_address addr) {
    return claimed_get(addr);
}

// total_claimed is a view: cumulative tokens claimed.
__uint128_t __total_claimed_impl(void) {
    return total_claimed_get();
}

// merkle_root_first_byte is a view: -1 if root unset, else the first byte of
// the stored root. A cheap probe tests use to confirm set_root landed.
int64_t __merkle_root_first_byte_impl(void) {
    if (!root_set_get()) {
        return -1;
    }
    pyde_bytes32 root = merkle_root_get();
    return (int64_t)root.bytes[0];
}
