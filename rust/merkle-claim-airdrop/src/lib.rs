//! `merkle-claim-airdrop` — Merkle-tree-based claim contract.
//! Canonical Pyde example of off-chain commitment + on-chain
//! verification: an off-chain process precomputes a tree where each
//! leaf is `Blake3(LEAF_TAG ‖ claimant_addr ‖ amount_be)`, the
//! constructor commits the root via `init(root)`, and claimants then
//! prove inclusion with their leaf + sibling-path to claim their
//! allocation. Successful claims transfer the proven `amount` of PYDE
//! from the contract's balance to the claimant.
//!
//! ## Funding
//!
//! The contract has zero balance after deploy. Before any `claim`
//! will pay out, someone must fund it:
//!
//!   1. Call `fund()` (a `#[payable]` entry) with attached value.
//!      Emits `Funded(from, amount)` for off-chain accounting.
//!   2. Any unsolicited value transfer to the contract address
//!      lands in the contract's balance and is claimable later.
//!
//! `claim()` transfers PYDE from the contract's balance to the
//! claimant. If the contract is short of the proven amount the chain
//! reverts the inner transfer call and `claim()` propagates
//! `TransferFailed` — state changes (incl. the `claimed` flag) roll
//! back atomically so the claimant can retry once the contract has
//! been topped up.
//!
//! ## Why merkle (not a map of preallocations)
//!
//! A naive airdrop stores `claimed[address] = bool` for every
//! recipient at deploy time — gas + storage that scales with the
//! recipient set. Merkle commits the entire allocation in ONE 32-byte
//! root; the cost moves off-chain (tree construction) and on-chain
//! is constant per claimant (`O(log n)` hash ops to verify a proof).
//!
//! Standard trick the entire ecosystem uses.
//!
//! ## Tree encoding
//!
//! Leaf:  `Blake3(LEAF_TAG ‖ claimant_addr_32 ‖ amount_u128_be_16)`
//! Node:  `Blake3(NODE_TAG ‖ left_hash_32 ‖ right_hash_32)`
//!
//! Domain-separation tags (`LEAF_TAG`, `NODE_TAG`) prevent a leaf
//! hash from colliding with an internal node hash. Without them an
//! attacker who can guess the tree's depth could potentially craft
//! a leaf whose hash equals an internal node — and submit a "claim"
//! with the wrong amount at a higher level.
//!
//! Amount is encoded as **big-endian u128** in 16 bytes — matching
//! the standard Solidity-style merkle airdrop encoding so the tests
//! can use the same tree-construction code paths community
//! generators ship.
//!
//! Proof encoding:  `[position_byte ‖ sibling_hash_32]` for each step,
//! repeated N times. `position_byte = 0` ⇒ current hash is the LEFT
//! child of this step's parent (sibling goes on the RIGHT). `1` ⇒
//! current hash is RIGHT, sibling LEFT.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use core::panic::PanicInfo;
use pyde_host as pyde;
use pyde_host::{Address, Hash32};

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

pyde::declare_storage!();
pyde::declare_events!();

// ────────────────────────────────────────────────────────────────
// Constants
// ────────────────────────────────────────────────────────────────

/// Domain-separation tag for leaf hashing. Prepended to
/// `(addr ‖ amount_be)` so a leaf hash can't collide with any
/// internal node hash (which uses `NODE_TAG`).
const LEAF_TAG: &[u8] = b"PYDE_LEAF";

/// Domain-separation tag for internal node hashing.
const NODE_TAG: &[u8] = b"PYDE_NODE";

/// 32-byte all-zero address. Used as the sentinel for "admin not yet
/// recorded" — `init()` rejects a second call by checking whether the
/// admin slot is still zero.
const ZERO_ADDRESS: Address = [0u8; 32];

// ────────────────────────────────────────────────────────────────
// Payment helper
// ────────────────────────────────────────────────────────────────

/// Push `amount` PYDE from this contract's balance to `to`. Reverts
/// the whole frame on a non-zero return code — `claim()` relies on
/// this to propagate "contract is underfunded" failures cleanly so
/// the storage-side claim flag rolls back atomically and the
/// claimant can retry after a top-up.
fn pay(to: &Address, amount: u128) {
    let amount_bytes = amount.to_le_bytes();
    let rc = unsafe { pyde::raw::transfer(to.as_ptr(), amount_bytes.as_ptr()) };
    if rc != 0 {
        pyde::revert("merkle: TransferFailed");
    }
}

// ────────────────────────────────────────────────────────────────
// Merkle leaf + node hashing
// ────────────────────────────────────────────────────────────────

/// Compute a leaf hash from `(claimant, amount)`.
fn leaf_hash(claimant: &Address, amount: u128) -> Hash32 {
    // 9 (tag) + 32 (addr) + 16 (amount) = 57 bytes
    let mut buf = [0u8; 57];
    buf[..9].copy_from_slice(LEAF_TAG);
    buf[9..41].copy_from_slice(claimant);
    buf[41..].copy_from_slice(&amount.to_be_bytes());
    pyde::hash::blake3(&buf)
}

/// Compute an internal node hash from its two children.
fn node_hash(left: &Hash32, right: &Hash32) -> Hash32 {
    // 9 (tag) + 32 + 32 = 73 bytes
    let mut buf = [0u8; 73];
    buf[..9].copy_from_slice(NODE_TAG);
    buf[9..41].copy_from_slice(left);
    buf[41..].copy_from_slice(right);
    pyde::hash::blake3(&buf)
}

/// Walk a proof from `leaf` upward, applying each step's sibling
/// according to its position byte. Returns the computed root.
///
/// `proof_bytes` must be a multiple of 33; this is checked by the
/// caller before invocation.
fn walk_proof(leaf: Hash32, proof_bytes: &[u8]) -> Hash32 {
    let mut hash = leaf;
    let mut i = 0;
    while i < proof_bytes.len() {
        let position = proof_bytes[i];
        let mut sibling = [0u8; 32];
        sibling.copy_from_slice(&proof_bytes[i + 1..i + 33]);
        hash = if position == 0 {
            // Current hash is on the left.
            node_hash(&hash, &sibling)
        } else {
            // Current hash is on the right.
            node_hash(&sibling, &hash)
        };
        i += 33;
    }
    hash
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

/// Constructor — records the caller as the contract admin. The admin
/// is the only address allowed to call `set_root` later. Reverts if
/// called a second time (`admin` slot already populated).
///
/// Tagged `["constructor"]` in the manifest so the chain rejects any
/// post-deploy invocation. The in-source guard is defence in depth
/// + lets the test runner exercise the second-call revert path
/// without going through full constructor-only chain semantics.
#[pyde::entry]
fn init() {
    if storage::admin().read() != ZERO_ADDRESS {
        pyde::revert("merkle: already initialized");
    }
    storage::admin().write(pyde::ctx::caller());
}

/// Payable funding entry — accept PYDE into the contract's own balance.
/// Anyone can fund. Emits `Funded(from, amount)` for off-chain
/// accounting. Refuses zero-value calls so accidental empty calls are
/// caught loudly instead of emitting a meaningless event.
#[pyde::entry]
fn fund() {
    let amount = pyde::ctx::tx_value();
    if amount == 0 {
        pyde::revert("merkle: fund requires non-zero value");
    }
    let from = pyde::ctx::caller();
    events::Funded { from, amount }.emit();
}

/// Commit the merkle root. Admin-only — only the address recorded by
/// `init()` may set the root. Subsequent calls (even by the admin)
/// revert with `RootAlreadySet`, so the root is locked once committed.
#[pyde::entry]
fn set_root(root: Hash32) {
    let admin = storage::admin().read();
    if admin == ZERO_ADDRESS {
        pyde::revert("merkle: not initialized");
    }
    if pyde::ctx::caller() != admin {
        pyde::revert("merkle: caller is not admin");
    }
    if storage::root_set().read() {
        pyde::revert("RootAlreadySet");
    }
    storage::merkle_root().write(root);
    storage::root_set().write(true);
}

/// Claim `amount` tokens. Verifies `(caller, amount) ∈ tree` by
/// recomputing the root from `leaf_hash(caller, amount)` plus the
/// supplied path, then transfers `amount` PYDE from the contract's
/// balance to the claimant.
///
/// State + event happen BEFORE the value transfer (checks-effects-
/// interactions). Pyde has no implicit reentrancy — only
/// `cross_call` can re-enter — and `pay()` resolves through the
/// native `transfer` host fn rather than `cross_call`, so re-entry
/// is impossible. The ordering is for discipline + parity with the
/// `vesting` template, not because the contract is vulnerable
/// without it.
///
/// Reverts:
/// - `RootNotSet` — `set_root` hasn't been called yet.
/// - `AlreadyClaimed` — `caller` has already claimed.
/// - `MalformedProof` — `proof.len() % 33 != 0`.
/// - `ProofTooLong` — cap at 32 levels.
/// - `InvalidProof` — recomputed root doesn't match the stored root.
/// - `merkle: TransferFailed` — the contract's balance is short of
///   the proven amount. State changes are rolled back atomically
///   with the revert, so the `claimed` flag does not stick and the
///   claimant can retry once the contract has been topped up.
#[pyde::entry]
fn claim(amount: u128, proof: Vec<u8>) {
    if !storage::root_set().read() {
        pyde::revert("RootNotSet");
    }

    let claimant = pyde::ctx::caller();
    if storage::claimed().read(&claimant) {
        pyde::revert("AlreadyClaimed");
    }

    // Proof length must be a multiple of 33 (each step = 1B
    // position + 32B sibling). Empty proofs are allowed —
    // degenerate single-leaf trees verify directly against the
    // leaf hash.
    if !proof.len().is_multiple_of(33) {
        pyde::revert("MalformedProof");
    }

    // Cap at 32 levels × 33 bytes = 1056 — supports trees up to
    // 2^32 leaves, far beyond any realistic airdrop.
    const MAX_PROOF_BYTES: usize = 33 * 32;
    if proof.len() > MAX_PROOF_BYTES {
        pyde::revert("ProofTooLong");
    }

    let leaf = leaf_hash(&claimant, amount);
    let computed_root = walk_proof(leaf, &proof);
    let stored_root = storage::merkle_root().read();

    if computed_root != stored_root {
        pyde::revert("InvalidProof");
    }

    storage::claimed().write(&claimant, true);
    let prev_total = storage::total_claimed().read();
    let new_total = prev_total
        .checked_add(amount)
        .unwrap_or_else(|| pyde::revert("merkle: total claimed overflow"));
    storage::total_claimed().write(new_total);

    events::Claim { claimant, amount }.emit();

    // Actual PYDE transfer to the claimant. If the contract is
    // underfunded the host fn returns non-zero and `pay()` reverts
    // the whole frame — `storage::claimed` rolls back with it, so
    // the user can retry after a top-up.
    pay(&claimant, amount);
}

/// View: false if unclaimed, true if claimed.
#[pyde::entry]
fn is_claimed(addr: Address) -> bool {
    storage::claimed().read(&addr)
}

/// View: cumulative tokens claimed.
#[pyde::entry]
fn total_claimed() -> u128 {
    storage::total_claimed().read()
}

/// View: -1 if root unset, else the first byte of the stored root.
/// A cheap probe tests use to confirm `set_root` landed.
#[pyde::entry]
fn merkle_root_first_byte() -> i64 {
    if !storage::root_set().read() {
        return -1;
    }
    let root = storage::merkle_root().read();
    root[0] as i64
}
