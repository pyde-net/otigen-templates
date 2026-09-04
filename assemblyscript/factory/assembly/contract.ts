// A factory: mint and drive child contracts from a deployed template.
//
// A factory creates fresh instances of a TEMPLATE, and the template is just
// any contract you already deployed. Deploy one, copy its address, pass that
// address to create. Each child is a first-class contract with its own
// address and its own isolated storage, sharing the template's already
// cached code — nothing is copied and nothing is recompiled.
//
// ## Deterministic addressing
//
//   child = Poseidon2("pyde-child:" ‖ factory ‖ template ‖ salt)
//
// The salt is derived from an identity rather than randomness, so the same
// identity always maps to the same child. That is what makes an address
// knowable BEFORE the child exists (fund it in advance) and makes creation
// idempotent (a repeat reverts instead of silently minting a duplicate).
//
// A random salt throws both properties away, which is why none of the three
// creation paths below offers one.
//
// The template is instantiated with NO constructor arguments, so it works
// with any contract that has no required constructor. To drive a child, this
// cross-calls its `increment() -> u64`.

import {
  Address,
  Bytes32,
  BorshEncoder,
  BorshDecoder,
  New,
  Call,
  poseidon2,
  revertStr,
  statusName,
  ERR_CHILD_ADDRESS_TAKEN,
  ERR_TEMPLATE_NOT_CONTRACT,
} from "@pyde-net/host/assembly";
import { storage } from "./generated/pyde.storage.generated";

// ─────────────────────────────────────────────────────────────────────
// Salt derivation
// ─────────────────────────────────────────────────────────────────────

/// `Poseidon2(borsh(key))` — the identity salt for a u64.
///
/// Matches Rust's `Salt::of(&key)`: borsh-encode the value, then hash. The
/// encoding has to agree exactly, or the same logical identity would map to
/// a different child than the Rust factory produces.
function saltOfU64(key: u64): Bytes32 {
  return poseidon2(new BorshEncoder().u64(key).toBytes());
}

/// `Poseidon2(borsh(name))` — the identity salt for a string.
///
/// borsh encodes a string as a u32-LE length prefix followed by UTF-8, so
/// any name is a valid deterministic identity.
function saltOfName(name: string): Bytes32 {
  return poseidon2(new BorshEncoder().string(name).toBytes());
}

// ─────────────────────────────────────────────────────────────────────
// Internals
// ─────────────────────────────────────────────────────────────────────

/// Instantiate `template` at `salt` with no constructor arguments.
///
/// The engine's outcomes are surfaced as clean reverts rather than raw
/// codes: a repeated identity is `exists`, an address that is not a
/// contract is `template-not-found`, and a child constructor's own revert
/// message is forwarded verbatim so the caller sees what the child said.
function mint(template: Address, salt: Bytes32): Address {
  const r = New(template).salt(salt).instantiate();
  if (r.ok) {
    return r.child;
  }
  if (r.status == ERR_CHILD_ADDRESS_TAKEN) {
    revertStr("exists");
  }
  if (r.status == ERR_TEMPLATE_NOT_CONTRACT) {
    revertStr("template-not-found");
  }
  const msg = r.revertMessage;
  if (msg.length > 0) {
    revertStr(msg);
  }
  revertStr("instantiate-failed: " + statusName(r.status));
  return r.child; // unreachable; revertStr does not return
}

/// Cross-call a child's `increment` (no arguments) and return its new value.
function increment(child: Address): u64 {
  const r = Call(child, "increment").exec();
  if (!r.ok) {
    const msg = r.revertMessage;
    if (msg.length > 0) {
      revertStr(msg);
    }
    revertStr("child-call-failed");
  }
  return new BorshDecoder(r.data).u64();
}

/// Tick the minted-children counter.
function record(): void {
  storage.created.write(storage.created.read() + 1);
}

// ─────────────────────────────────────────────────────────────────────
// Create
// ─────────────────────────────────────────────────────────────────────

/// Mint a child with an AUTO salt: the factory's own counter. The caller
/// manages nothing; each call takes the next slot.
@entry
export function create(template: Address): Address {
  const key = storage.next_key.read();
  const child = mint(template, saltOfU64(key));
  storage.children.write(key, child);
  storage.next_key.write(key + 1);
  record();
  return child;
}

/// Mint a child at an explicit u64 key, such as a user id. The address is a
/// function of (factory, template, salt), so the same key always targets the
/// same child and a second call reverts `exists`.
@entry
export function create_with_key(template: Address, key: u64): Address {
  const child = mint(template, saltOfU64(key));
  storage.children.write(key, child);
  record();
  return child;
}

/// Mint a child at a string salt, such as a market pair "ETH/USDC".
@entry
export function create_named(template: Address, name: string): Address {
  const child = mint(template, saltOfName(name));
  storage.named.write(name, child);
  record();
  return child;
}

// ─────────────────────────────────────────────────────────────────────
// Look up
// ─────────────────────────────────────────────────────────────────────

/// A child by its u64 key. Zero address when that key was never created.
@entry
@view
export function child_of(key: u64): Address {
  return storage.children.read(key);
}

/// A child by its string name. Zero address when never created.
@entry
@view
export function child_of_name(name: string): Address {
  return storage.named.read(name);
}

/// The key `create` will use next.
@entry
@view
export function next_key(): u64 {
  return storage.next_key.read();
}

/// Total children this factory has minted.
@entry
@view
export function created(): u64 {
  return storage.created.read();
}

// ─────────────────────────────────────────────────────────────────────
// Interact
// ─────────────────────────────────────────────────────────────────────

/// Drive a child BY u64 KEY: pull it from the registry, then cross-call
/// its `increment`.
@entry
export function bump(key: u64): u64 {
  return increment(storage.children.read(key));
}

/// Drive a child BY STRING NAME.
@entry
export function bump_named(name: string): u64 {
  return increment(storage.named.read(name));
}

/// Drive a child BY ADDRESS, with no lookup.
///
/// This is how a contract talks to a contract it did not create: by
/// address, sharing the template's ABI, with no typed handle required.
@entry
export function bump_at(child: Address): u64 {
  return increment(child);
}
