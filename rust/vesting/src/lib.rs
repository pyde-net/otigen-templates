//! `vesting` — linear-with-cliff token vesting. Canonical Pyde
//! example of a time-locked allocation: configure at deploy time
//! with `(beneficiary, total, start, cliff, duration)`, fund the
//! contract with at least `total` PYDE, then anyone can call
//! `release()` over time to forward the vested-but-unreleased
//! portion to the beneficiary. The schedule is evaluated against
//! `wave_timestamp()`.
//!
//! ## Vesting schedule
//!
//!   `t = wave_timestamp()`
//!   `if t < start + cliff:           vested = 0`
//!   `if t >= start + duration:       vested = total`
//!   `else:                           vested = total * (t - start) / duration`
//!
//! After the cliff is crossed, vesting is **linear from `start`** —
//! not from the cliff. The cliff just delays the *first* release;
//! once past, you can release `(cliff / duration) * total` in one
//! shot, then linearly thereafter. (Matches OpenZeppelin
//! VestingWallet's reference semantics.)

#![no_std]

extern crate alloc;

use core::panic::PanicInfo;
use pyde_host as pyde;
use pyde_host::Address;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

pyde::declare_storage!();
pyde::declare_events!();

const ZERO_ADDRESS: Address = [0u8; 32];

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

/// Push `amount` PYDE from this contract's balance to `to`. Reverts
/// the whole frame on a non-zero return code — `release()` relies on
/// this to propagate "contract is underfunded" failures back to the
/// caller as a clean revert rather than silently advancing `released`.
fn pay(to: &Address, amount: u128) {
    let amount_bytes = amount.to_le_bytes();
    let rc = unsafe { pyde::raw::transfer(to.as_ptr(), amount_bytes.as_ptr()) };
    if rc != 0 {
        pyde::revert("vesting: TransferFailed");
    }
}

// ────────────────────────────────────────────────────────────────
// Vesting math
// ────────────────────────────────────────────────────────────────

/// Compute `vested = total * (t - start) / duration` for the given
/// timestamp. Returns 0 before cliff, `total` after duration ends.
fn vested_at(t: u64) -> u128 {
    let start = storage::start_time().read();
    let cliff = storage::cliff_seconds().read();
    let duration = storage::duration_seconds().read();
    let total = storage::total_amount().read();

    if duration == 0 {
        // Degenerate: zero-duration vesting = fully vested at start.
        return if t >= start { total } else { 0 };
    }

    if t < start.saturating_add(cliff) {
        return 0;
    }

    if t >= start.saturating_add(duration) {
        return total;
    }

    // Inside the linear window. `elapsed` fits in u64; promoting to
    // u128 before the multiply makes the product overflow-safe up
    // to total * 2^64 — way past any realistic vesting setup, but
    // we still bail explicitly if the product wraps.
    let elapsed = (t - start) as u128;
    total
        .checked_mul(elapsed)
        .unwrap_or_else(|| pyde::revert("vesting: overflow"))
        / (duration as u128)
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

/// Constructor — runs once at deploy. The manifest tags this as
/// `[functions.configure].attributes = ["constructor"]` so the
/// chain rejects any post-deploy call; the in-source
/// `storage::configured` flag is defence in depth.
///
/// Reverts:
/// - `vesting: already configured` — second-call guard.
/// - `vesting: beneficiary is zero address` — zero-address sink
///   guard; prevents silently locking tokens forever.
/// - `vesting: total must be non-zero` — a zero-total vesting is
///   indistinguishable from an unfunded contract; reject so the
///   misconfiguration is loud rather than silent.
/// - `vesting: cliff exceeds duration` — past `start + duration`
///   the vested amount is `total` regardless of cliff, so a cliff
///   longer than duration is a configuration error.
#[pyde::entry]
fn configure(
    beneficiary: Address,
    total: u128,
    start_time: u64,
    cliff_seconds: u64,
    duration_seconds: u64,
) {
    if storage::configured().read() {
        pyde::revert("vesting: already configured");
    }

    if beneficiary == ZERO_ADDRESS {
        pyde::revert("vesting: beneficiary is zero address");
    }

    if total == 0 {
        pyde::revert("vesting: total must be non-zero");
    }

    if cliff_seconds > duration_seconds {
        pyde::revert("vesting: cliff exceeds duration");
    }

    storage::beneficiary().write(beneficiary);
    storage::total_amount().write(total);
    storage::start_time().write(start_time);
    storage::cliff_seconds().write(cliff_seconds);
    storage::duration_seconds().write(duration_seconds);
    storage::configured().write(true);

    events::Configured {
        beneficiary,
        total_amount: total,
        start_time,
        cliff_seconds,
        duration_seconds,
    }
    .emit();
}

/// Payable entry — accept PYDE into the contract's own balance.
/// Anyone can fund. Emits `Funded(from, amount)` for off-chain
/// accounting. The amount sent is `pyde::ctx::tx_value()` (the
/// chain credits the contract's balance automatically; this entry
/// just provides a labelled funding surface + audit event).
///
/// Refuses zero-value calls so accidental `fund()` invocations
/// without an attached value are caught loudly instead of
/// emitting a meaningless event.
#[pyde::entry]
fn fund() {
    let amount = pyde::ctx::tx_value();
    if amount == 0 {
        pyde::revert("vesting: fund requires non-zero value");
    }
    let from = pyde::ctx::caller();
    events::Funded { from, amount }.emit();
}

/// Permissionless: anyone can call. Pays `vested - released` to
/// the configured beneficiary from this contract's balance, bumps
/// `released`, emits `Released`.
///
/// State + event happen BEFORE the value transfer (checks-effects-
/// interactions). Pyde has no implicit reentrancy — only
/// `cross_call` can re-enter — and `pay()` resolves through the
/// native `transfer` host fn rather than `cross_call`, so re-entry
/// is impossible. The ordering is for discipline + parity with
/// the `simple-multisig` template, not because the contract is
/// vulnerable without it.
///
/// Reverts:
/// - `vesting: not configured` — `configure()` hasn't been called yet.
/// - `vesting: nothing to release` — `vested == released`.
/// - `vesting: TransferFailed` — the contract's balance is short
///   of the computed payout. State changes are rolled back atomically
///   with the revert, so `released` does not advance.
#[pyde::entry]
fn release() -> u128 {
    if !storage::configured().read() {
        pyde::revert("vesting: not configured");
    }

    let now = pyde::ctx::wave_timestamp();
    let vested = vested_at(now);
    let already_released = storage::released().read();

    if vested <= already_released {
        pyde::revert("vesting: nothing to release");
    }

    let payout = vested - already_released;
    let new_released = already_released
        .checked_add(payout)
        .unwrap_or_else(|| pyde::revert("vesting: overflow"));
    storage::released().write(new_released);

    let beneficiary = storage::beneficiary().read();
    events::Released {
        beneficiary,
        amount: payout,
        total_released: new_released,
    }
    .emit();

    // Actual PYDE transfer to the beneficiary. If the contract is
    // underfunded the host fn returns non-zero and `pay()` reverts
    // the whole frame — `storage::released` rolls back with it, so
    // a subsequent topped-up `release()` re-computes the same
    // payout cleanly.
    pay(&beneficiary, payout);

    payout
}

/// View: vested at current `wave_timestamp`.
#[pyde::entry]
fn vested_amount() -> u128 {
    vested_at(pyde::ctx::wave_timestamp())
}

/// View: amount that could be released right now
/// (`vested - released`).
#[pyde::entry]
fn releasable() -> u128 {
    let v = vested_at(pyde::ctx::wave_timestamp());
    let r = storage::released().read();
    if v <= r {
        return 0;
    }
    v - r
}

/// View: cumulative released so far.
#[pyde::entry]
fn released_amount() -> u128 {
    storage::released().read()
}

/// View: false if `configure` hasn't been called, true otherwise.
#[pyde::entry]
fn configured_flag() -> bool {
    storage::configured().read()
}
