// Thin proxy + admin-swappable logic: the canonical upgradeable-contract
// pattern.
//
// `forward(function, calldata)` runs the logic contract's code in THIS
// contract's frame via a delegate-call:
//
//   - self_address = the proxy
//   - caller       = the original caller, preserved across the delegate
//   - storage slots derived from the PROXY's self_address, so the logic's
//     `storage.value` accessor writes to the proxy's slot
//
// So when the admin swaps `logic` for a newer implementation, the proxy's
// state — `value` in particular — survives the upgrade. That is the whole
// point of the pattern.
//
// ## Privileged-slot namespacing
//
// Slots are derived as Poseidon2(self_address ‖ field_name). Under a
// delegate-call the logic contract sees the PROXY's self_address, so a
// logic contract that happened to declare a field named `admin` would
// derive the proxy's admin slot and clobber it. The fix is the `proxy_`
// prefix on the privileged fields: a logic contract would have to pick
// that exact prefixed name to collide, which is a far louder mistake.
// `value` stays unprefixed because it is the intentionally shared
// demonstration field.

import {
  Address,
  equals32,
  newAddress,
  caller,
  DelegateCall,
  revertStr,
  ERR_INVALID_FUNCTION_NAME,
} from "@pyde-net/host/assembly";
import { storage } from "./pyde.storage.generated";
import { events } from "./pyde.events.generated";

// ─────────────────────────────────────────────────────────────────────
// Admin
// ─────────────────────────────────────────────────────────────────────

/// Revert unless the caller currently holds the admin role.
///
/// After `renounce_admin` the admin slot is the zero address, and no
/// caller can equal it, so every admin-gated entry is permanently shut.
function requireAdmin(): Address {
  const admin = storage.proxy_admin.read();
  if (!equals32(caller(), admin)) {
    revertStr("proxy: caller is not admin");
  }
  return admin;
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

// Constructor — runs once at deploy, recording the deployer as admin and
// the first logic contract.
//
// The manifest tags this `attributes = ["constructor"]` so the chain
// rejects any post-deploy call; the zero-admin check is defence in depth
// against an accidental re-run.
//
// Both zero-address guards prevent bricking the proxy on the first call:
// a zero admin means nobody can ever upgrade, and a zero logic means
// every `forward` delegates into a contract that does not exist.
@entry
export function init(initial_logic: Address): void {
  const zero = newAddress();
  if (!equals32(storage.proxy_admin.read(), zero)) {
    revertStr("proxy: already initialized");
  }
  const admin = caller();
  if (equals32(admin, zero) || equals32(initial_logic, zero)) {
    revertStr("proxy: init with zero address");
  }
  storage.proxy_admin.write(admin);
  storage.proxy_logic.write(initial_logic);
  events.Initialized(admin, initial_logic);
}

/// Admin-only logic-pointer swap. Deliberately does not touch `value` —
/// preserving that across the upgrade is the point of the pattern.
@entry
export function upgrade_to(new_logic: Address): void {
  requireAdmin();
  if (equals32(new_logic, newAddress())) {
    revertStr("proxy: upgrade to zero address");
  }
  const old_logic = storage.proxy_logic.read();
  storage.proxy_logic.write(new_logic);
  events.Upgraded(old_logic, new_logic);
}

/// Rotate the admin role. Admin only.
///
/// A zero-address new admin reverts: that would silently make the lock
/// irreversible. `renounce_admin` exists for exactly that, so the
/// irrevocable path is loud rather than disguised as a transfer.
@entry
export function transfer_admin(new_admin: Address): void {
  const old_admin = requireAdmin();
  if (equals32(new_admin, newAddress())) {
    revertStr("proxy: transfer to zero address; use renounce_admin");
  }
  storage.proxy_admin.write(new_admin);
  events.AdminTransferred(old_admin, new_admin);
}

/// Renounce the admin role by zeroing the admin slot.
///
/// After this NO ONE can `upgrade_to` or call any admin-gated entry, and
/// the logic pointer is frozen at its current value forever. Irreversible.
@entry
export function renounce_admin(): void {
  const admin = requireAdmin();
  const zero = newAddress();
  storage.proxy_admin.write(zero);
  events.AdminTransferred(admin, zero);
}

/// Dispatcher: delegate-call `logic.function(calldata)` in this contract's
/// frame, returning whatever bytes the logic produced.
///
/// The bytes are handed back verbatim rather than decoded, because the
/// proxy is a type-erased forwarder — it does not know the logic's return
/// shape and could not borsh-decode it. The caller decodes them per the
/// function's documented return type.
@entry
export function forward(functionName: string, calldata: StaticArray<u8>): StaticArray<u8> {
  const logic = storage.proxy_logic.read();
  const r = DelegateCall(logic, functionName).args(calldata).exec();
  if (r.ok) {
    return r.data;
  }
  // Pass the logic's own revert string straight through, so the proxy's
  // caller sees exactly what the logic said instead of a generic failure.
  const msg = r.revertMessage;
  if (msg.length > 0) {
    revertStr(msg);
  }
  if (r.status == ERR_INVALID_FUNCTION_NAME) {
    revertStr("proxy: logic has no such function");
  }
  revertStr("proxy: delegate-call failed");
  return r.data; // unreachable; revertStr does not return
}

// ─────────────────────────────────────────────────────────────────────
// Views
// ─────────────────────────────────────────────────────────────────────

/// The current admin. Zero address once renounced.
@entry
@view
export function get_admin(): Address {
  return storage.proxy_admin.read();
}

/// The logic contract `forward` currently delegates into.
@entry
@view
export function get_logic(): Address {
  return storage.proxy_logic.read();
}

/// The shared `value` slot — the state that survives an upgrade.
@entry
@view
export function get_value(): u64 {
  return storage.value.read();
}
