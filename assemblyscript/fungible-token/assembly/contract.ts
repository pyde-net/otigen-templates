// The PTS-F reference implementation (`pts-f/1`, PIP-0005), default
// extension configuration.
//
// What this surface fixes, each deviation pointing at a documented loss
// class on other chains:
//
//   - **Revert-only mutations.** No boolean returns to mis-handle;
//     failures are canonical machine-readable `token:*` codes that
//     propagate verbatim through a cross-call unwind.
//   - **Expiring, delta-only allowances.** Every grant carries a
//     mandatory expiry wave (TTL-capped at ~1 year); increase/decrease
//     deltas replace raw overwrites, so the approve race is dead by
//     construction and "unlimited forever" is unrepresentable.
//   - **Settle-then-notify deposits.** `transfer_call` writes balances
//     and emits `Transfer` FIRST, then notifies the recipient, which must
//     return the `ACK_TOKEN` acknowledgement — a name-miss falling
//     through to a fallback cannot silently swallow tokens. Plain
//     `transfer` never invokes recipient code.
//   - **Consent-visible control.** Minter/manager roles are declared
//     state, rotated by the manager, renounced by provable zeroing.
//   - **Parallel-execution-ready layout.** A transfer writes exactly the
//     two parties' balance slots; `total_supply` is written only by
//     mint/burn. Disjoint transfers commute under Block-STM.
//
// Storage economics: balances and allowances are zeroed, never deleted —
// the chain has no gas refunds, so deleting a slot is strictly costlier
// than writing zero.
//
// ## A note on integer overflow
//
// Rust reaches for `checked_add` / `saturating_sub`. AssemblyScript's
// u128 wraps silently instead, so every place the Rust checks a carry is
// re-expressed here as a comparison against the operands — a sum smaller
// than an addend can only mean wrap. The helpers at the bottom keep that
// reasoning in one place rather than scattered through the entries.

import {
  Address,
  u128,
  BorshEncoder,
  BorshDecoder,
  Call,
  caller,
  selfAddress,
  waveId,
  equals32,
  newAddress,
  revertStr,
} from "@pyde-net/host/assembly";
import { storage } from "./generated/pyde.storage.generated";
import { events } from "./generated/pyde.events.generated";
import { TokenInfo } from "./generated/pyde.types.generated";

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

/// The surface this contract conforms to.
const STANDARD: string = "pts-f/1";

/// Hard cap on allowance lifetime: ~1 year at 500 ms/wave. Even a
/// maximal grant self-destructs.
const MAX_ALLOWANCE_TTL_WAVES: u64 = 63_072_000;

/// Max `owners` accepted by `balance_of_batch`.
const MAX_BATCH: i32 = 256;

/// Max bytes of `data` forwarded by `transfer_call` — bounds
/// payload-driven gas griefing under no-refund economics.
const MAX_CALL_DATA: i32 = 4_096;

/// Required return of a conformant receiver:
/// `u32::from_le_bytes(Blake3("pts/on_token_received/1")[0..4])`.
const ACK_TOKEN: u32 = 2_266_754_145; // LE bytes 61 ec 1b 87

/// 32-byte role identifiers carried by `RoleTransfer` (PIP-0005 §3):
/// `Blake3("minter")` / `Blake3("manager")`.
const ROLE_MINTER: StaticArray<u8> = [
  0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
  0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
  0xe5, 0xdd,
];
const ROLE_MANAGER: StaticArray<u8> = [
  0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
  0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
  0x35, 0xc1,
];

/// Extension flags reported by `token_info()` (PIP-0005 §5): this is the
/// default configuration — bit 4 (`transfer_call`) + bit 5 (`burnable`).
const EXTENSION_FLAGS: u32 = (1 << 4) | (1 << 5);

// ─── Constructor ───────────────────────────────────────────────────────

/// Set metadata, mint `initial_supply` to the deployer, and seat the
/// deployer as both minter and manager. `max_supply = 0` = uncapped.
///
/// `decimals` is a display-only hint. The PTS default (and the value
/// every example uses) is 9 — parity with native quanta, 1 PYDE = 10⁹.
@entry
export function init(
  name: string,
  symbol: string,
  decimals: u8,
  initial_supply: u128,
  max_supply: u128,
): void {
  if (decimals > 18) {
    revertStr("token:invalid_decimals");
  }
  if (!max_supply.isZero() && initial_supply > max_supply) {
    revertStr("token:cap_exceeded");
  }
  const deployer = caller();
  const zero = newAddress();

  storage.token_name.write(name);
  storage.token_symbol.write(symbol);
  storage.token_decimals.write(decimals);
  storage.total_supply.write(initial_supply);
  storage.max_supply.write(max_supply);
  storage.minter.write(deployer);
  storage.manager.write(deployer);
  storage.balances.write(deployer, initial_supply);

  // Mint sentinel: from = zero address. One event family carries all
  // supply accounting.
  events.Transfer(zero, deployer, initial_supply);
  events.RoleTransfer(ROLE_MINTER, deployer, zero);
  events.RoleTransfer(ROLE_MANAGER, deployer, zero);
}

// ─── Views ─────────────────────────────────────────────────────────────

/// Runtime discovery handshake: which surface, which version.
@entry
@view
export function standard(): string {
  return STANDARD;
}

@entry
@view
export function name(): string {
  return storage.token_name.read();
}

@entry
@view
export function symbol(): string {
  return storage.token_symbol.read();
}

@entry
@view
export function decimals(): u8 {
  return storage.token_decimals.read();
}

@entry
@view
export function total_supply(): u128 {
  return storage.total_supply.read();
}

/// 0 = uncapped.
@entry
@view
export function max_supply(): u128 {
  return storage.max_supply.read();
}

/// A never-written slot reads as 0 — holding zero and never having held
/// are distinguishable at the state layer, not here.
@entry
@view
export function balance_of(owner: Address): u128 {
  return storage.balances.read(owner);
}

/// Batched balance reads for routers and wallets — one call instead of an
/// N-round-trip loop. Views are free off-chain, so the only bound is the
/// anti-griefing cap.
@entry
@view
export function balance_of_batch(owners: Array<Address>): Array<u128> {
  if (owners.length > MAX_BATCH) {
    revertStr("token:batch_too_large");
  }
  const out = new Array<u128>(owners.length);
  for (let i = 0, n = owners.length; i < n; i++) {
    out[i] = storage.balances.read(unchecked(owners[i]));
  }
  return out;
}

/// Remaining spendable NOW: 0 once expired. Expiry is evaluated lazily
/// against the wave clock — no keeper, no revocation gas for the
/// forgetful.
@entry
@view
export function allowance(owner: Address, spender: Address): u128 {
  return effectiveAllowanceAmount(owner, spender);
}

/// The raw expiry wave of the stored grant (0 = no grant).
@entry
@view
export function allowance_expiry(owner: Address, spender: Address): u64 {
  return storage.allowance_expiries.read(owner, spender);
}

/// The whole metadata surface in one free call. `extension_flags` is the
/// wallet capability renderer: bit 0 freeze, 1 pause, 2
/// registration-required, 3 metadata_uri, 4 transfer_call, 5 burnable.
@entry
@view
export function token_info(): TokenInfo {
  const info = new TokenInfo();
  info.name = storage.token_name.read();
  info.symbol = storage.token_symbol.read();
  info.decimals = storage.token_decimals.read();
  info.total_supply = storage.total_supply.read();
  info.max_supply = storage.max_supply.read();
  info.minter = storage.minter.read();
  info.extension_flags = EXTENSION_FLAGS;
  return info;
}

/// Zero address = renounced.
@entry
@view
export function minter(): Address {
  return storage.minter.read();
}

@entry
@view
export function manager(): Address {
  return storage.manager.read();
}

// ─── Transfers ─────────────────────────────────────────────────────────

/// Move `amount` from the caller to `to`. Writes exactly two balance
/// slots; NEVER invokes code on `to` — mandatory hooks are the costliest
/// exploit mechanism in token history, so notification is the opt-in
/// `transfer_call` path.
@entry
export function transfer(to: Address, amount: u128): void {
  const from = caller();
  moveTokens(from, to, amount);
  events.Transfer(from, to, amount);
}

/// Settle-then-notify (PIP-0005 §6): (1) full balance settlement,
/// (2) `Transfer` emission, (3) notify the recipient, which must return
/// `ACK_TOKEN`. A recipient revert — or a missing/wrong acknowledgement,
/// including a name-miss swallowed by a fallback — reverts the ENTIRE
/// operation atomically (`token:bad_receiver`).
///
/// This one atomic message replaces approve-then-pull for deposits: no
/// standing authority is ever created.
@entry
export function transfer_call(to: Address, amount: u128, data: Array<u8>): u32 {
  if (data.length > MAX_CALL_DATA) {
    revertStr("token:data_too_large");
  }
  const operator = caller();

  // (1) settle
  moveTokens(operator, to, amount);
  // (2) emit — discarded with the frame if the notify leg reverts
  events.Transfer(operator, to, amount);

  // (3) notify. The arguments are filled by the token and are never
  // spoofable by the sender; the receiver frame sees caller() = this
  // contract. A borsh tuple is its fields back to back with no framing.
  const bytes = new StaticArray<u8>(data.length);
  for (let i = 0, n = data.length; i < n; i++) {
    unchecked((bytes[i] = data[i]));
  }
  const calldata = new BorshEncoder()
    .address(operator)
    .address(operator)
    .u128(amount)
    .bytes(bytes)
    .toBytes();

  const r = Call(to, "on_token_received").args(calldata).exec();
  // Wrong magic, decode mismatch, revert, no code at `to`, name-miss
  // falling through to a fallback (which cannot produce the ack): all
  // collapse to the one canonical rejection.
  if (!r.ok || r.data.length != 4) {
    revertStr("token:bad_receiver");
  }
  const ack = new BorshDecoder(r.data).u32();
  if (ack != ACK_TOKEN) {
    revertStr("token:bad_receiver");
  }
  return ACK_TOKEN;
}

/// Spend a live allowance of `(from, caller)`: expiry checked against the
/// wave clock, amount decremented, balance moved. Emits `Transfer` only —
/// the hot pull path stays at one event, and operator attribution is
/// derivable from the enclosing transaction.
@entry
export function transfer_from(from: Address, to: Address, amount: u128): void {
  const spender = caller();
  spendAllowance(from, spender, amount);
  moveTokens(from, to, amount);
  events.Transfer(from, to, amount);
}

// ─── Delegated spending ────────────────────────────────────────────────

/// Compatibility form: set the allowance to exactly `amount` with the
/// maximum TTL auto-applied. Integrators pattern-match the name;
/// "unlimited forever" stays unrepresentable — even this grant
/// self-destructs after ~1 year of waves.
@entry
export function approve(spender: Address, amount: u128): void {
  const owner = caller();
  writeAllowance(owner, spender, amount, satAddU64(waveId(), MAX_ALLOWANCE_TTL_WAVES));
}

/// Delta increase with an explicit expiry. Deltas kill the approve
/// front-running race by construction, because there is no overwrite to
/// race. An expired grant contributes 0 to the new amount.
@entry
export function increase_allowance(spender: Address, amount: u128, expiry_wave: u64): void {
  const owner = caller();
  requireValidExpiry(expiry_wave);
  const current = effectiveAllowanceAmount(owner, spender);
  const next = current + amount;
  if (next < current) {
    revertStr("token:overflow");
  }
  writeAllowance(owner, spender, next, expiry_wave);
}

/// Delta decrease, floored at zero. Expiry is unchanged for a live grant;
/// an expired grant collapses to `{0, 0}`.
@entry
export function decrease_allowance(spender: Address, amount: u128): void {
  const owner = caller();
  const current = effectiveAllowanceAmount(owner, spender);
  const nextExpiry = current.isZero() ? 0 : storage.allowance_expiries.read(owner, spender);
  writeAllowance(owner, spender, satSubU128(current, amount), nextExpiry);
}

/// Hard-zero the grant. Writes `{0, 0}` — never deletes the slot, since
/// no gas refunds exist and deletion is strictly costlier than zeroing.
@entry
export function revoke_allowance(spender: Address): void {
  writeAllowance(caller(), spender, u128.Zero, 0);
}

/// Compare-and-set: change the grant to `new_remaining` ONLY if the
/// current effective remaining equals `expected_remaining`. This closes
/// the delta-accumulation footgun, where re-granting without reading
/// hands the spender old + new. Under commit-reveal the check can fail at
/// reveal time if state moved; `token:allowance_changed` is a retriable
/// condition, not corruption.
@entry
export function set_allowance_exact(
  spender: Address,
  expected_remaining: u128,
  new_remaining: u128,
  expiry_wave: u64,
): void {
  const owner = caller();
  if (effectiveAllowanceAmount(owner, spender) != expected_remaining) {
    revertStr("token:allowance_changed");
  }
  if (new_remaining.isZero()) {
    writeAllowance(owner, spender, u128.Zero, 0);
    return;
  }
  requireValidExpiry(expiry_wave);
  writeAllowance(owner, spender, new_remaining, expiry_wave);
}

// ─── Supply and roles ──────────────────────────────────────────────────

/// Minter only. Reverts above `max_supply` when capped. Emits the
/// zero-sentinel `Transfer`.
@entry
export function mint(to: Address, amount: u128): void {
  const who = caller();
  if (!equals32(who, storage.minter.read()) || equals32(who, newAddress())) {
    revertStr("token:not_minter");
  }
  guardRecipient(to);

  const supply = storage.total_supply.read();
  const nextSupply = supply + amount;
  if (nextSupply < supply) {
    revertStr("token:overflow");
  }
  const cap = storage.max_supply.read();
  if (!cap.isZero() && nextSupply > cap) {
    revertStr("token:cap_exceeded");
  }

  storage.total_supply.write(nextSupply);
  // A balance cannot overflow if the supply did not: balance ≤ supply.
  storage.balances.write(to, storage.balances.read(to) + amount);
  events.Transfer(newAddress(), to, amount);
}

/// Burn the caller's own tokens (the default configuration is burnable).
/// Emits the zero-sentinel `Transfer`.
@entry
export function burn(amount: u128): void {
  burnTokens(caller(), amount);
}

/// Burn from `from`, spending a live allowance of `(from, caller)`.
@entry
export function burn_from(from: Address, amount: u128): void {
  spendAllowance(from, caller(), amount);
  burnTokens(from, amount);
}

/// Manager only. Zero address = provable renounce; a renounced minter can
/// never be re-seated once the manager is also renounced.
@entry
export function set_minter(holder: Address): void {
  requireManager();
  const previous = storage.minter.read();
  storage.minter.write(holder);
  events.RoleTransfer(ROLE_MINTER, holder, previous);
}

/// Manager only. Renouncing the manager (zero address) freezes role
/// governance permanently.
@entry
export function set_manager(holder: Address): void {
  requireManager();
  const previous = storage.manager.read();
  storage.manager.write(holder);
  events.RoleTransfer(ROLE_MANAGER, holder, previous);
}

// ─── Internal helpers ──────────────────────────────────────────────────

/// `a + b` for u64, clamped at the maximum instead of wrapping.
function satAddU64(a: u64, b: u64): u64 {
  const sum = a + b;
  return sum < a ? u64.MAX_VALUE : sum;
}

/// `a - b` for u128, floored at zero instead of wrapping.
function satSubU128(a: u128, b: u128): u128 {
  return a < b ? u128.Zero : a - b;
}

/// Reject an expiry that is already past or beyond the TTL ceiling.
function requireValidExpiry(expiry_wave: u64): void {
  const now = waveId();
  if (expiry_wave <= now || expiry_wave > satAddU64(now, MAX_ALLOWANCE_TTL_WAVES)) {
    revertStr("token:invalid_expiry");
  }
}

/// Revert unless the caller is the seated manager. A zero-address caller
/// can never match, so a renounced manager freezes role governance.
function requireManager(): void {
  const who = caller();
  if (!equals32(who, storage.manager.read()) || equals32(who, newAddress())) {
    revertStr("token:not_manager");
  }
}

/// Recipient guards shared by every credit path: the zero address (a burn
/// must be explicit) and the token's own address — the single largest
/// measured stuck-token bucket on chains without this check.
function guardRecipient(to: Address): void {
  if (equals32(to, newAddress()) || equals32(to, selfAddress())) {
    revertStr("token:invalid_recipient");
  }
}

/// Debit `from`, credit `to`, with the full guard set. `from == to`
/// validates and no-ops, because the two slots are the same slot.
function moveTokens(from: Address, to: Address, amount: u128): void {
  guardRecipient(to);
  const fromBal = storage.balances.read(from);
  if (fromBal < amount) {
    revertStr("token:insufficient_balance");
  }
  if (equals32(from, to)) {
    return;
  }
  const toBal = storage.balances.read(to);
  const toNext = toBal + amount;
  if (toNext < toBal) {
    revertStr("token:overflow");
  }
  storage.balances.write(from, fromBal - amount);
  storage.balances.write(to, toNext);
}

function burnTokens(from: Address, amount: u128): void {
  const bal = storage.balances.read(from);
  if (bal < amount) {
    revertStr("token:insufficient_balance");
  }
  storage.balances.write(from, bal - amount);
  storage.total_supply.write(satSubU128(storage.total_supply.read(), amount));
  events.Transfer(from, newAddress(), amount);
}

/// The stored grant's amount if live, 0 if expired or absent. Live means
/// `waveId() <= expiry_wave`.
function effectiveAllowanceAmount(owner: Address, spender: Address): u128 {
  const amount = storage.allowance_amounts.read(owner, spender);
  if (!amount.isZero() && waveId() <= storage.allowance_expiries.read(owner, spender)) {
    return amount;
  }
  return u128.Zero;
}

/// Decrement a live allowance by `amount`, distinguishing "expired" from
/// "too small" so integrators get the retriable signal.
function spendAllowance(owner: Address, spender: Address, amount: u128): void {
  const stored = storage.allowance_amounts.read(owner, spender);
  if (!stored.isZero() && waveId() > storage.allowance_expiries.read(owner, spender)) {
    revertStr("token:allowance_expired");
  }
  const live = effectiveAllowanceAmount(owner, spender);
  if (live < amount) {
    revertStr("token:insufficient_allowance");
  }
  storage.allowance_amounts.write(owner, spender, live - amount);
}

/// Write the grant and emit the absolute post-state — indexers
/// reconstruct allowance state from the latest `Approval` per pair, with
/// no delta replay needed.
function writeAllowance(
  owner: Address,
  spender: Address,
  amount: u128,
  expiry_wave: u64,
): void {
  storage.allowance_amounts.write(owner, spender, amount);
  storage.allowance_expiries.write(owner, spender, expiry_wave);
  events.Approval(owner, spender, amount, expiry_wave);
}
