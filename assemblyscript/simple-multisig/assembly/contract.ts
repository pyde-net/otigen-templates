// A 3-signer FALCON-512 multisig: the canonical Pyde example of verifying a
// post-quantum signature inside a contract.
//
// Three signers are registered at deploy. Each is identified on-chain by
// `Poseidon2(falcon_pubkey)` — a 32-byte hash of the signer's 897-byte
// FALCON-512 public key. Storing the hash instead of the key keeps state
// small; callers supply the full key at execute time.
//
// To move funds, anyone submits one call carrying up to three
// (pubkey, signature) pairs plus a nonce. Each pair must hash to a
// registered signer, must not repeat a signer already counted, and must
// verify against the action digest. Once `threshold` distinct signatures
// verify, the contract transfers and marks the digest used.
//
// ## Domain-separated digest
//
// Signers do not sign a bare amount — they sign a digest computed ON-CHAIN:
//
//   action_digest = Poseidon2(
//       "PYDE-MULTISIG-V1"  ‖  // 16 bytes, domain tag
//       self_address        ‖  // 32 bytes
//       chain_id            ‖  //  8 bytes, u64 LE
//       target              ‖  // 32 bytes
//       amount              ‖  // 16 bytes, u128 LE
//       nonce                  // 16 bytes, u128 LE
//   )
//
// Binding all of that means a signature over `transfer(A, 1)` cannot be
// replayed as `transfer(B, 1000000)`, a signature minted on devnet does not
// validate on mainnet, and a signature for one multisig does not validate
// against another sharing the same signers. The caller never supplies the
// digest, so there is nothing for them to lie about.

import {
  Address,
  Bytes32,
  u128,
  u128ToBytesLE,
  equals32,
  newAddress,
  selfAddress,
  chainId,
  poseidon2,
  falconVerify,
  transfer,
  revertStr,
  writeU64LE,
} from "@pyde-net/host/assembly";
import { storage } from "./generated/pyde.storage.generated";
import { events } from "./generated/pyde.events.generated";

/// Registered signer count. Three keeps the contract small and lets the
/// per-signer storage layout stay hard-coded.
const N_SIGNERS: u8 = 3;

/// Domain-separation tag. Bumping the suffix invalidates every existing
/// signature, which is the intended migration path if the digest scheme
/// ever changes.
const DOMAIN_TAG: StaticArray<u8> = [
  0x50, 0x59, 0x44, 0x45, 0x2d, 0x4d, 0x55, 0x4c, // "PYDE-MUL"
  0x54, 0x49, 0x53, 0x49, 0x47, 0x2d, 0x56, 0x31, // "TISIG-V1"
];

/// Exact digest preimage length: 16 + 32 + 8 + 32 + 16 + 16.
const PREIMAGE_LEN: i32 = 120;

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

/// The on-chain signer ID: `Poseidon2(pubkey)`. Matches the test runner's
/// `@pubkey_hash:NAME` DSL exactly.
function pubkeyHash(pk: StaticArray<u8>): Bytes32 {
    return poseidon2(pk);
}

/// Rebuild the canonical action digest. Sized exactly, so the contract
/// never reallocates on the happy path.
///
/// This is both the message every signer signs AND the replay-protection
/// storage key — one value serving both roles is what makes replay
/// impossible rather than merely unlikely.
function computeActionDigest(target: Address, amount: u128, nonce: u128): Bytes32 {
  const buf = new StaticArray<u8>(PREIMAGE_LEN);
  const base = changetype<usize>(buf);
  let off: usize = 0;

  memory.copy(base, changetype<usize>(DOMAIN_TAG), 16);
  off += 16;
  memory.copy(base + off, changetype<usize>(selfAddress()), 32);
  off += 32;
  writeU64LE(base + off, chainId());
  off += 8;
  memory.copy(base + off, changetype<usize>(target), 32);
  off += 32;
  memory.copy(base + off, changetype<usize>(u128ToBytesLE(amount)), 16);
  off += 16;
  memory.copy(base + off, changetype<usize>(u128ToBytesLE(nonce)), 16);

  return poseidon2(buf);
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

/// Configure the multisig once, at deploy.
///
/// Duplicate signer hashes are rejected: a multisig registered with
/// `(alice, alice, bob)` would silently degrade a threshold of 2 into a
/// 1-of-2, because alice alone fills two slots. That is not a multisig.
@entry
export function init(
  threshold: u8,
  signer0: Bytes32,
  signer1: Bytes32,
  signer2: Bytes32,
): void {
  if (threshold < 1 || threshold > N_SIGNERS) {
    revertStr("BadThreshold");
  }
  if (
    equals32(signer0, signer1) ||
    equals32(signer0, signer2) ||
    equals32(signer1, signer2)
  ) {
    revertStr("DuplicateSignerInInit");
  }
  storage.threshold.write(threshold);
  storage.signers.write(0, signer0);
  storage.signers.write(1, signer1);
  storage.signers.write(2, signer2);
}

/// Execute a transfer once `threshold` distinct signatures verify.
///
/// Each slot is a (pubkey, signature) pair; an empty pubkey means the slot
/// is unused, so a caller may pass any combination as long as enough
/// distinct signers verify.
///
/// Every signature is checked against the on-chain digest, so signers must
/// sign exactly what the contract will recompute. Query `action_digest`
/// first, or derive it off-chain from the canonical encoding above.
@entry
export function execute(
  target: Address,
  amount: u128,
  nonce: u128,
  pk1: StaticArray<u8>,
  sig1: StaticArray<u8>,
  pk2: StaticArray<u8>,
  sig2: StaticArray<u8>,
  pk3: StaticArray<u8>,
  sig3: StaticArray<u8>,
): void {
  if (equals32(target, newAddress())) {
    revertStr("multisig: execute to zero address");
  }

  const actionDigest = computeActionDigest(target, amount, nonce);

  if (storage.used.read(actionDigest)) {
    revertStr("AlreadyUsed");
  }

  const threshold = storage.threshold.read();
  const signers: StaticArray<Bytes32> = [
    storage.signers.read(0),
    storage.signers.read(1),
    storage.signers.read(2),
  ];

  const pks: StaticArray<StaticArray<u8>> = [pk1, pk2, pk3];
  const sigs: StaticArray<StaticArray<u8>> = [sig1, sig2, sig3];

  // Bitmap of signer indices already counted, so the same signer cannot be
  // presented twice across slots and counted twice toward the threshold.
  let seenSigners: u8 = 0;
  let valid: u8 = 0;

  for (let slot = 0; slot < 3; slot++) {
    const pk = unchecked(pks[slot]);
    if (pk.length == 0) continue;

    const pkHash = pubkeyHash(pk);
    let signerIdx: i32 = -1;
    // AssemblyScript will not compare an i32 loop counter against a u8
    // constant, unlike Rust's usize-vs-u8 coercion — widen explicitly.
    for (let i = 0; i < <i32>N_SIGNERS; i++) {
      if (equals32(unchecked(signers[i]), pkHash)) {
        signerIdx = i;
        break;
      }
    }
    if (signerIdx < 0) {
      revertStr("UnknownSigner");
    }

    const bit: u8 = (<u8>1) << (<u8>signerIdx);
    if ((seenSigners & bit) != 0) {
      revertStr("DuplicateSigner");
    }
    seenSigners |= bit;

    if (!falconVerify(pk, actionDigest, unchecked(sigs[slot]))) {
      revertStr("BadSignature");
    }
    valid += 1;
  }

  if (valid < threshold) {
    revertStr("InsufficientApprovals");
  }

  // Mark used BEFORE the transfer, so a re-entrant transfer cannot
  // double-spend by calling execute again.
  storage.used.write(actionDigest, true);

  transfer(target, amount);

  events.Executed(target, amount, actionDigest);
}

/// View: signatures required to execute.
@entry
@view
export function get_threshold(): u8 {
  return storage.threshold.read();
}

/// View: has this digest already executed?
@entry
@view
export function is_used(action_digest: Bytes32): bool {
  return storage.used.read(action_digest);
}

/// View: the digest the contract will recompute for
/// `execute(target, amount, nonce)`.
///
/// Off-chain wallets call this before collecting signatures, so every
/// signer signs the exact bytes the chain will check against.
@entry
@view
export function action_digest(target: Address, amount: u128, nonce: u128): Bytes32 {
  return computeActionDigest(target, amount, nonce);
}
