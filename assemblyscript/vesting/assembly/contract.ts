// Linear-with-cliff vesting. Configure at deploy with
// `(beneficiary, total, start, cliff, duration)`, fund the contract with at
// least `total` PYDE, then anyone can call `release()` over time to forward
// the vested-but-unreleased portion to the beneficiary. The schedule is
// evaluated against `waveTimestamp()`.
//
// ## Schedule
//
//   t = waveTimestamp()
//   if t < start + cliff:      vested = 0
//   if t >= start + duration:  vested = total
//   else:                      vested = total * (t - start) / duration
//
// After the cliff is crossed, vesting is LINEAR FROM `start`, not from the
// cliff. The cliff only delays the first release; once past it you can
// release `(cliff / duration) * total` in one shot, then linearly after.
// Matches OpenZeppelin VestingWallet's reference semantics.

import {
  Address,
  u128,
  equals32,
  newAddress,
  caller,
  txValue,
  waveTimestamp,
  revertStr,
} from "@pyde-net/host/assembly";
import { transfer } from "@pyde-net/host/assembly/raw";
import { u128ToBytesLE } from "@pyde-net/host/assembly";
import { storage } from "./generated/pyde.storage.generated";
import { events } from "./generated/pyde.events.generated";

// ─────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────

// Push `amount` PYDE from this contract's balance to `to`. Reverts the whole
// frame on a non-zero status — `release()` leans on this to surface an
// underfunded contract as a clean revert rather than silently advancing
// `released`.
function pay(to: Address, amount: u128): void {
  const amountBytes = u128ToBytesLE(amount);
  const rc = transfer(changetype<usize>(to), changetype<usize>(amountBytes));
  if (rc != 0) {
    revertStr("vesting: TransferFailed");
  }
}

// ─────────────────────────────────────────────────────────────────────
// Vesting math
// ─────────────────────────────────────────────────────────────────────

// `vested = total * (t - start) / duration` at the given timestamp.
// Returns 0 before the cliff and `total` once the duration has elapsed.
function vestedAt(t: u64): u128 {
  const start = storage.start_time.read();
  const cliff = storage.cliff_seconds.read();
  const duration = storage.duration_seconds.read();
  const total = storage.total_amount.read();

  if (duration == 0) {
    // Degenerate: zero-duration vesting is fully vested at start.
    return t >= start ? total : u128.Zero;
  }

  // Saturating adds: a start or cliff near u64::MAX must not wrap into a
  // small number and hand out the whole allocation early.
  const cliffEnd = start > u64.MAX_VALUE - cliff ? u64.MAX_VALUE : start + cliff;
  if (t < cliffEnd) {
    return u128.Zero;
  }

  const vestEnd = start > u64.MAX_VALUE - duration ? u64.MAX_VALUE : start + duration;
  if (t >= vestEnd) {
    return total;
  }

  // Inside the linear window. `muldiv` computes total * elapsed / duration
  // without overflowing the intermediate product, so there is no ceiling on
  // `total` the way a naive multiply would impose.
  return u128.muldiv(total, u128.fromU64(t - start), u128.fromU64(duration));
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

// Constructor — runs once at deploy. The manifest tags this
// `attributes = ["constructor"]` so the chain rejects any post-deploy call;
// the `configured` flag is defence in depth.
//
// Reverts:
// - `already configured` — second-call guard.
// - `beneficiary is zero address` — prevents locking the allocation forever.
// - `total must be non-zero` — a zero total is indistinguishable from an
//   unfunded contract, so reject it loudly.
// - `cliff exceeds duration` — past `start + duration` the vested amount is
//   `total` regardless of cliff, so a longer cliff is a config error.
@entry
export function configure(
  beneficiary: Address,
  total: u128,
  start_time: u64,
  cliff_seconds: u64,
  duration_seconds: u64,
): void {
  if (storage.configured.read()) {
    revertStr("vesting: already configured");
  }
  if (equals32(beneficiary, newAddress())) {
    revertStr("vesting: beneficiary is zero address");
  }
  if (total.isZero()) {
    revertStr("vesting: total must be non-zero");
  }
  if (cliff_seconds > duration_seconds) {
    revertStr("vesting: cliff exceeds duration");
  }

  storage.beneficiary.write(beneficiary);
  storage.total_amount.write(total);
  storage.start_time.write(start_time);
  storage.cliff_seconds.write(cliff_seconds);
  storage.duration_seconds.write(duration_seconds);
  storage.configured.write(true);

  events.Configured(beneficiary, total, start_time, cliff_seconds, duration_seconds);
}

// Payable entry — accept PYDE into the contract's own balance. Anyone may
// fund. The chain credits the balance automatically; this entry exists to
// give funding a labelled surface and an audit event.
//
// Zero-value calls are refused so an accidental `fund()` with nothing
// attached fails loudly instead of emitting a meaningless event.
@entry
@payable
export function fund(): void {
  const amount = txValue();
  if (amount.isZero()) {
    revertStr("vesting: fund requires non-zero value");
  }
  events.Funded(caller(), amount);
}

// Permissionless: anyone may call. Pays `vested - released` to the
// configured beneficiary out of this contract's balance, advances
// `released`, and emits `Released`.
//
// State and event happen BEFORE the transfer (checks-effects-interactions).
// Pyde has no implicit reentrancy — only `cross_call` can re-enter, and
// `pay()` goes through the native `transfer` host fn — so re-entry is not
// possible here. The ordering is discipline and parity with the multisig
// template, not a vulnerability fix.
//
// Reverts:
// - `not configured` — `configure()` has not run.
// - `nothing to release` — `vested == released`.
// - `TransferFailed` — the contract's balance is short of the payout. The
//   state change rolls back atomically with the revert, so `released` does
//   not advance and a later topped-up call recomputes the same payout.
@entry
export function release(): u128 {
  if (!storage.configured.read()) {
    revertStr("vesting: not configured");
  }

  const vested = vestedAt(waveTimestamp());
  const alreadyReleased = storage.released.read();

  if (vested <= alreadyReleased) {
    revertStr("vesting: nothing to release");
  }

  const payout = vested - alreadyReleased;
  // `alreadyReleased + payout` is exactly `vested`, and `vested <= total`,
  // so this cannot overflow — no checked add needed.
  const newReleased = vested;
  storage.released.write(newReleased);

  const beneficiary = storage.beneficiary.read();
  events.Released(beneficiary, payout, newReleased);

  pay(beneficiary, payout);

  return payout;
}

// View: vested at the current `waveTimestamp`.
@entry
@view
export function vested_amount(): u128 {
  return vestedAt(waveTimestamp());
}

// View: what could be released right now (`vested - released`).
@entry
@view
export function releasable(): u128 {
  const v = vestedAt(waveTimestamp());
  const r = storage.released.read();
  return v <= r ? u128.Zero : v - r;
}

// View: cumulative released so far.
@entry
@view
export function released_amount(): u128 {
  return storage.released.read();
}

// View: false until `configure` has run.
@entry
@view
export function configured_flag(): bool {
  return storage.configured.read();
}
