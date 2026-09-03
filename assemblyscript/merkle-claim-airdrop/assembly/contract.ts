// Merkle-tree airdrop claim: the canonical Pyde proof-of-inclusion example.
//
// An admin commits one 32-byte merkle root covering the whole allocation
// list. Each recipient then proves membership and claims, so the chain
// never stores the list — only the root. A million-entry airdrop costs the
// same storage as a one-entry airdrop.
//
// ## Tree shape
//
//   Leaf:  Blake3(LEAF_TAG ‖ claimant_addr[32] ‖ amount_u128_BE[16])
//   Node:  Blake3(NODE_TAG ‖ left[32] ‖ right[32])
//
// The two tags are what stop a leaf from being replayed as an internal
// node: without domain separation, an attacker who finds a leaf whose hash
// happens to sit at an internal position could forge a proof.
//
// ⚠️ The amount is BIG-endian in the leaf preimage, unlike every other
// integer this SDK touches, which is little-endian. It has to match the
// Rust template byte for byte or no proof built off-chain will verify.
//
// ## Proof encoding
//
// A flat byte run, 33 bytes per level: one position byte (0 = the running
// hash is the LEFT child, anything else = right), then the 32-byte
// sibling. An empty proof is legal — a single-leaf tree verifies directly
// against the leaf hash.

import {
  Address,
  Bytes32,
  u128,
  u128ToBytesLE,
  equals32,
  newAddress,
  newBytes32,
  caller,
  txValue,
  blake3,
  tryTransfer,
  revertStr,
} from "@pyde-net/host/assembly";
import { storage } from "./pyde.storage.generated";
import { events } from "./pyde.events.generated";

/// Domain-separation tag for leaf hashing: "PYDE_LEAF", 9 bytes.
const LEAF_TAG: StaticArray<u8> = [0x50, 0x59, 0x44, 0x45, 0x5f, 0x4c, 0x45, 0x41, 0x46];

/// Domain-separation tag for internal nodes: "PYDE_NODE", 9 bytes.
const NODE_TAG: StaticArray<u8> = [0x50, 0x59, 0x44, 0x45, 0x5f, 0x4e, 0x4f, 0x44, 0x45];

/// One proof step: 1 position byte + a 32-byte sibling.
const PROOF_STEP: i32 = 33;

/// 32 levels covers a tree of 2^32 leaves, far past any real airdrop.
const MAX_PROOF_BYTES: i32 = PROOF_STEP * 32;

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

/// A u128 as 16 BIG-endian bytes.
///
/// The SDK only ships little-endian conversion, because that is what the
/// chain uses everywhere else. The merkle leaf preimage is the exception,
/// so the byte order is reversed here rather than assumed.
function u128ToBytesBE(value: u128): StaticArray<u8> {
  const le = u128ToBytesLE(value);
  const be = new StaticArray<u8>(16);
  for (let i = 0; i < 16; i++) {
    unchecked((be[i] = le[15 - i]));
  }
  return be;
}

/// Push `amount` PYDE to `to`, reverting the whole frame on failure.
///
/// `claim` leans on this: an underfunded contract must roll the claimed
/// flag back with the revert, so the claimant can retry after a top-up
/// rather than being permanently marked as paid.
///
/// Uses `tryTransfer` rather than `transfer` so the revert carries THIS
/// contract's message. `transfer` reverts with the SDK's own generic
/// wording, which would replace a reason the caller can act on with one
/// that only says a transfer failed somewhere.
function pay(to: Address, amount: u128): void {
  if (!tryTransfer(to, amount)) {
    revertStr("merkle: TransferFailed");
  }
}

/// `Blake3(LEAF_TAG ‖ claimant ‖ amount_BE)` — 9 + 32 + 16 = 57 bytes.
function leafHash(claimant: Address, amount: u128): Bytes32 {
  const buf = new StaticArray<u8>(57);
  const base = changetype<usize>(buf);
  memory.copy(base, changetype<usize>(LEAF_TAG), 9);
  memory.copy(base + 9, changetype<usize>(claimant), 32);
  memory.copy(base + 41, changetype<usize>(u128ToBytesBE(amount)), 16);
  return blake3(buf);
}

/// `Blake3(NODE_TAG ‖ left ‖ right)` — 9 + 32 + 32 = 73 bytes.
function nodeHash(left: Bytes32, right: Bytes32): Bytes32 {
  const buf = new StaticArray<u8>(73);
  const base = changetype<usize>(buf);
  memory.copy(base, changetype<usize>(NODE_TAG), 9);
  memory.copy(base + 9, changetype<usize>(left), 32);
  memory.copy(base + 41, changetype<usize>(right), 32);
  return blake3(buf);
}

/// Walk a proof upward from `leaf`, applying each sibling on the side its
/// position byte names. Returns the root the proof implies, which the
/// caller compares against the committed one.
function walkProof(leaf: Bytes32, proof: StaticArray<u8>): Bytes32 {
  let hash = leaf;
  let i = 0;
  while (i < proof.length) {
    const position = unchecked(proof[i]);
    const sibling = new StaticArray<u8>(32);
    memory.copy(
      changetype<usize>(sibling),
      changetype<usize>(proof) + <usize>(i + 1),
      32,
    );
    hash = position == 0 ? nodeHash(hash, sibling) : nodeHash(sibling, hash);
    i += PROOF_STEP;
  }
  return hash;
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

/// Record the deployer as admin. A second call reverts, since the admin
/// slot is only zero before the first.
@entry
@mutating
export function init(): void {
  if (!equals32(storage.admin.read(), newAddress())) {
    revertStr("merkle: already initialized");
  }
  storage.admin.write(caller());
}

/// Payable funding entry. Anyone may fund; the chain credits the balance
/// automatically, and this gives it a labelled surface plus an audit
/// event. Zero-value calls are refused so an accidental empty call fails
/// loudly rather than emitting a meaningless event.
@entry
@payable
export function fund(): void {
  const amount = txValue();
  if (amount.isZero()) {
    revertStr("merkle: fund requires non-zero value");
  }
  events.Funded(caller(), amount);
}

/// Commit the merkle root. Admin only, and only once: the root locks on
/// first write so the allocation cannot be swapped after claims begin.
@entry
@mutating
export function set_root(root: Bytes32): void {
  const admin = storage.admin.read();
  if (equals32(admin, newAddress())) {
    revertStr("merkle: not initialized");
  }
  if (!equals32(caller(), admin)) {
    revertStr("merkle: caller is not admin");
  }
  if (storage.root_set.read()) {
    revertStr("RootAlreadySet");
  }
  storage.merkle_root.write(root);
  storage.root_set.write(true);
}

/// Claim an allocation by proving membership in the committed tree.
///
/// The claimant is the caller, never a parameter — a proof is bound to the
/// address inside the leaf, so nobody can claim on another's behalf or
/// redirect a payout.
@entry
@mutating
export function claim(amount: u128, proof: StaticArray<u8>): void {
  if (!storage.root_set.read()) {
    revertStr("RootNotSet");
  }

  const claimant = caller();
  if (storage.claimed.read(claimant)) {
    revertStr("AlreadyClaimed");
  }

  // Each level is exactly 33 bytes. A ragged length means the proof was
  // not produced by the canonical encoder, so reject rather than reading
  // a truncated sibling. Empty is fine: a single-leaf tree has no levels.
  if (proof.length % PROOF_STEP != 0) {
    revertStr("MalformedProof");
  }
  if (proof.length > MAX_PROOF_BYTES) {
    revertStr("ProofTooLong");
  }

  const computedRoot = walkProof(leafHash(claimant, amount), proof);
  if (!equals32(computedRoot, storage.merkle_root.read())) {
    revertStr("InvalidProof");
  }

  storage.claimed.write(claimant, true);

  const prevTotal = storage.total_claimed.read();
  const newTotal = prevTotal + amount;
  // u128 addition wraps rather than trapping, so the overflow is caught
  // by comparison: a sum smaller than either operand can only mean wrap.
  if (newTotal < prevTotal) {
    revertStr("merkle: total claimed overflow");
  }
  storage.total_claimed.write(newTotal);

  events.Claim(claimant, amount);

  // Transfer last. If the contract is underfunded this reverts the whole
  // frame, rolling the claimed flag back so the claimant can retry.
  pay(claimant, amount);
}

/// View: has this address already claimed?
@entry
@view
export function is_claimed(addr: Address): bool {
  return storage.claimed.read(addr);
}

/// View: cumulative amount claimed so far.
@entry
@view
export function total_claimed(): u128 {
  return storage.total_claimed.read();
}

/// View: -1 when no root is committed, otherwise the root's first byte.
/// A cheap probe for confirming `set_root` landed without returning the
/// whole root.
@entry
@view
export function merkle_root_first_byte(): i64 {
  if (!storage.root_set.read()) {
    return -1;
  }
  return <i64>unchecked(storage.merkle_root.read()[0]);
}
