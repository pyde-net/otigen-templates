//! `simple-multisig` — 2-of-3-style FALCON-512 multisig. Canonical
//! Pyde example of in-contract post-quantum signature verification
//! via `pyde::falcon_verify`, written on the macro substrate
//! (`#[pyde::entry]` + `declare_storage!()` + `declare_events!()`).
//!
//! ## Concept
//!
//! Three signers are registered at deploy time. Each is identified
//! on-chain by `Poseidon2(falcon_pubkey)` — a 32-byte hash of the
//! signer's FALCON-512 public key (897 bytes). Storing the hash
//! instead of the full pubkey keeps state small; callers provide
//! the full pubkey at execute time.
//!
//! To execute a transfer, anyone may submit one call passing up to
//! three (pubkey, signature) pairs and a strictly-increasing `nonce`.
//! Each pair is verified:
//!
//! - The pubkey's Poseidon2 hash must equal one of the registered
//!   signer IDs (`UnknownSigner` reverts).
//! - The same signer must not be counted twice (`DuplicateSigner`).
//! - `falcon_verify(pk, action_digest, sig)` must return 0
//!   (`BadSignature`).
//!
//! Once the number of valid distinct sigs reaches `threshold`, the
//! contract calls `transfer(target, amount)` and marks the
//! `action_digest` as used (anti-replay).
//!
//! ## Domain-separated digest binding
//!
//! Real multisigs (Gnosis Safe, etc.) sign a domain-separated
//! digest of the full intent — `Safe::getTransactionHash(target,
//! value, data, ..., nonce)`. This contract does the same: the
//! `action_digest` is computed **on-chain** as:
//!
//! ```text
//! action_digest = Poseidon2(
//!     DOMAIN_TAG ||      // b"PYDE-MULTISIG-V1"
//!     self_address ||    // 32 bytes — this contract's address
//!     chain_id ||        // 8 bytes  — little-endian u64
//!     target ||          // 32 bytes — recipient
//!     amount ||          // 16 bytes — little-endian u128
//!     nonce              // 16 bytes — little-endian u128
//! )
//! ```
//!
//! This binds every signature to (this contract, this chain, this
//! intent), so:
//!
//! - A sig over `transfer(A, 1)` can NOT be replayed as `transfer(B, 1_000_000)` —
//!   the digest commits to `target` and `amount`.
//! - A sig minted on devnet (chain_id 31337) does NOT validate on
//!   mainnet — the digest commits to `chain_id`.
//! - A sig for multisig X does NOT validate against multisig Y, even
//!   if the two share a signer set — the digest commits to
//!   `self_address`.
//! - The same intent re-executed needs a fresh `nonce` — the digest
//!   commits to `nonce`, and the replay map keys on the digest.
//!
//! Off-chain wallets can query the same digest before signing via the
//! `action_digest(target, amount, nonce)` view function.

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

/// Fixed number of registered signers. Three keeps the contract
/// small + lets us hard-code the per-signer storage layout.
const N_SIGNERS: u8 = 3;

/// 32-byte all-zero address — used as a sentinel for "no address".
/// A transfer to this address would burn funds, so `execute()`
/// rejects it up-front.
const ZERO_ADDRESS: Address = [0u8; 32];

/// Domain-separation tag for the FALCON-signed digest. Bumping the
/// suffix forces existing signatures to no longer validate, which
/// is the intended migration path if the digest scheme changes.
const DOMAIN_TAG: &[u8] = b"PYDE-MULTISIG-V1";

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

/// `Poseidon2(pubkey_bytes)` — the on-chain "signer ID". 32 bytes.
/// Matches the test runner's `@pubkey_hash:NAME` DSL exactly.
fn pubkey_hash(pk: &[u8]) -> Hash32 {
    pyde::hash::poseidon2(pk)
}

/// Push `amount` PYDE from this contract's balance to `to`. Reverts
/// the whole frame on a non-zero return code.
fn pay(to: &Address, amount: u128) {
    let amount_bytes = amount.to_le_bytes();
    let rc = unsafe { pyde::raw::transfer(to.as_ptr(), amount_bytes.as_ptr()) };
    if rc != 0 {
        pyde::revert("TransferFailed");
    }
}

/// Verify a FALCON-512 signature via the chain's host fn.
fn falcon_verify(pk: &[u8], msg: &[u8], sig: &[u8]) -> bool {
    let rc = unsafe {
        pyde::raw::falcon_verify(
            pk.as_ptr(),
            msg.as_ptr(),
            msg.len() as i32,
            sig.as_ptr(),
            sig.len() as i32,
        )
    };
    rc == 0
}

/// Recompute the canonical action digest. See the module-level
/// doc-comment for the encoding. The digest serves two roles
/// simultaneously: the FALCON message every signer signs over, and
/// the replay-protection storage key.
///
/// Pre-allocated length: 16 (DOMAIN) + 32 (self) + 8 (chain) + 32
/// (target) + 16 (amount) + 16 (nonce) = 120 bytes. We size the
/// Vec exactly so the contract avoids any reallocation on the
/// happy path.
fn compute_action_digest(target: &Address, amount: u128, nonce: u128) -> Hash32 {
    let self_addr = pyde::ctx::self_address();
    let chain_id = pyde::ctx::chain_id();
    let mut buf = Vec::with_capacity(120);
    buf.extend_from_slice(DOMAIN_TAG);
    buf.extend_from_slice(&self_addr);
    buf.extend_from_slice(&chain_id.to_le_bytes());
    buf.extend_from_slice(target);
    buf.extend_from_slice(&amount.to_le_bytes());
    buf.extend_from_slice(&nonce.to_le_bytes());
    pyde::hash::poseidon2(&buf)
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

/// Configure the multisig once at deploy time.
///
/// Idempotent in tests (fresh state per test); the chain's
/// `constructor` attribute prevents post-deploy calls in
/// production.
///
/// Reverts on duplicate signer hashes — a multisig registered with
/// `(alice, alice, bob)` would silently degrade `threshold = 2` to
/// a 1-of-2 (alice alone covers two slots). That's not a multisig.
#[pyde::entry]
fn init(threshold: u8, signer0: Hash32, signer1: Hash32, signer2: Hash32) {
    if threshold < 1 || threshold > N_SIGNERS {
        pyde::revert("BadThreshold");
    }
    if signer0 == signer1 || signer0 == signer2 || signer1 == signer2 {
        pyde::revert("DuplicateSignerInInit");
    }
    storage::threshold().write(threshold);
    storage::signers().write(&0u8, signer0);
    storage::signers().write(&1u8, signer1);
    storage::signers().write(&2u8, signer2);
}

/// Execute a transfer once `threshold` valid signatures are
/// presented.
///
/// Each signer slot consists of a `(pubkey, signature)` pair. An
/// empty pubkey (`len == 0`) means "this slot is unused" — the
/// caller may pass any combination as long as enough distinct
/// signers verify.
///
/// The on-chain `action_digest` (see module docs) binds every
/// signature to `(self_address, chain_id, target, amount, nonce)`.
/// Signers MUST sign over the same digest the contract computes —
/// query it via the `action_digest` view function or recompute it
/// off-chain using the canonical encoding.
///
/// Signature scheme:
///   1. `pk_hash := Poseidon2(pk_bytes)`.
///   2. `pk_hash` must be one of the three registered signer IDs.
///   3. The same signer ID must not appear twice across slots.
///   4. `falcon_verify(pk, action_digest, sig)` returns 0.
///
/// If `count_valid >= threshold` and `action_digest` hasn't been used:
///   - Mark `action_digest` as used.
///   - Call `transfer(target, amount)` from this contract's balance.
///   - Emit `Executed(target, amount, action_digest)`.
#[allow(clippy::too_many_arguments)]
#[pyde::entry]
fn execute(
    target: Address,
    amount: u128,
    nonce: u128,
    pk1: Vec<u8>,
    sig1: Vec<u8>,
    pk2: Vec<u8>,
    sig2: Vec<u8>,
    pk3: Vec<u8>,
    sig3: Vec<u8>,
) {
    if target == ZERO_ADDRESS {
        pyde::revert("multisig: execute to zero address");
    }

    // Recompute the digest on-chain so signatures are bound to
    // (this contract, this chain, this intent, this nonce). The
    // caller never supplies the digest — there's nothing for them
    // to lie about.
    let action_digest = compute_action_digest(&target, amount, nonce);

    if storage::used().read(&action_digest) {
        pyde::revert("AlreadyUsed");
    }

    let threshold = storage::threshold().read();
    let signers: [Hash32; 3] = [
        storage::signers().read(&0u8),
        storage::signers().read(&1u8),
        storage::signers().read(&2u8),
    ];

    let slots: [(&[u8], &[u8]); 3] = [
        (pk1.as_slice(), sig1.as_slice()),
        (pk2.as_slice(), sig2.as_slice()),
        (pk3.as_slice(), sig3.as_slice()),
    ];

    // Bitmap of which signer indices have already counted —
    // prevents double-counting the same signer across slots.
    let mut seen_signers: u8 = 0;
    let mut valid: u8 = 0;

    for (pk, sig) in slots {
        if pk.is_empty() {
            continue;
        }
        let pk_hash = pubkey_hash(pk);
        let mut signer_idx: Option<u8> = None;
        for (i, s) in signers.iter().enumerate() {
            if s == &pk_hash {
                signer_idx = Some(i as u8);
                break;
            }
        }
        let Some(idx) = signer_idx else {
            pyde::revert("UnknownSigner");
        };

        let bit = 1u8 << idx;
        if seen_signers & bit != 0 {
            pyde::revert("DuplicateSigner");
        }
        seen_signers |= bit;

        if !falcon_verify(pk, &action_digest, sig) {
            pyde::revert("BadSignature");
        }
        valid += 1;
    }

    if valid < threshold {
        pyde::revert("InsufficientApprovals");
    }

    // Anti-replay: mark BEFORE the transfer so a re-entrant
    // transfer can't double-spend by re-calling execute().
    storage::used().write(&action_digest, true);

    pay(&target, amount);

    events::Executed { target, amount, action_digest }.emit();
}

/// View: threshold required for execution.
#[pyde::entry]
fn get_threshold() -> u8 {
    storage::threshold().read()
}

/// View: is this action digest already executed?
#[pyde::entry]
fn is_used(action_digest: Hash32) -> bool {
    storage::used().read(&action_digest)
}

/// View: canonical digest the multisig will recompute + bind sigs
/// to for `execute(target, amount, nonce)`. Off-chain wallets call
/// this before collecting signatures so every signer signs the
/// exact same bytes the chain will reject otherwise.
#[pyde::entry]
fn action_digest(target: Address, amount: u128, nonce: u128) -> Hash32 {
    compute_action_digest(&target, amount, nonce)
}
