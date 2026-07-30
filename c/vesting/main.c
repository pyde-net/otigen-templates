// vesting — linear-with-cliff token vesting. Canonical Pyde example of a
// time-locked allocation: configure at deploy time with
// (beneficiary, total, start, cliff, duration), fund the contract with at
// least `total` PYDE, then anyone can call release() over time to forward the
// vested-but-unreleased portion to the beneficiary. The schedule is evaluated
// against wave_timestamp().
//
// Vesting schedule:
//
//   t = wave_timestamp()
//   if t < start + cliff:      vested = 0
//   if t >= start + duration:  vested = total
//   else:                      vested = total * (t - start) / duration
//
// After the cliff is crossed, vesting is linear from `start` — not from the
// cliff. The cliff just delays the first release; once past, you can release
// (cliff / duration) * total in one shot, then linearly thereafter. (Matches
// OpenZeppelin VestingWallet's reference semantics.)
//
// You write only the typed inner functions (`__<fn>_impl`) plus otigen.toml.
// `otigen build` reads [functions.*] / [state] / [events] and generates
// `pyde_gen.h`: the () -> () export shims, the typed storage accessors, and the
// event emitters. Every __<fn>_impl below is forward-declared in that header,
// so a signature drift is a hard C compile error rather than a mis-decoded
// contract.

#include "pyde_gen.h"

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static const pyde_address ZERO_ADDRESS = {{0}};

// beneficiary_is_zero mirrors `beneficiary == ZERO_ADDRESS` — the zero-address
// sink guard.
static bool addr_is_zero(pyde_address a) {
    return pyde_addr_eq(a, ZERO_ADDRESS);
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
// whole frame on a non-zero return code — release() relies on this to
// propagate "contract is underfunded" failures back to the caller as a clean
// revert rather than silently advancing `released`.
static void pay(pyde_address to, __uint128_t amount) {
    uint8_t amount_bytes[16];
    __uint128_t v = amount;
    for (int i = 0; i < 16; i++) {
        amount_bytes[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    int32_t rc = transfer(to.bytes, amount_bytes);
    if (rc != 0) {
        pyde_revert_str("vesting: TransferFailed");
    }
}

// saturating_add_u64 mirrors Rust's u64::saturating_add — clamps at MaxU64 on
// overflow instead of wrapping.
static uint64_t saturating_add_u64(uint64_t a, uint64_t b) {
    uint64_t sum = a + b;
    if (sum < a) {
        return ~(uint64_t)0;
    }
    return sum;
}

// ────────────────────────────────────────────────────────────────
// Vesting math
// ────────────────────────────────────────────────────────────────

// vested_at computes `vested = total * (t - start) / duration` for the given
// timestamp. Returns 0 before cliff, `total` after duration ends.
static __uint128_t vested_at(uint64_t t) {
    uint64_t start = start_time_get();
    uint64_t cliff = cliff_seconds_get();
    uint64_t duration = duration_seconds_get();
    __uint128_t total = total_amount_get();

    if (duration == 0) {
        // Degenerate: zero-duration vesting = fully vested at start.
        return t >= start ? total : (__uint128_t)0;
    }

    if (t < saturating_add_u64(start, cliff)) {
        return (__uint128_t)0;
    }

    if (t >= saturating_add_u64(start, duration)) {
        return total;
    }

    // Inside the linear window. `elapsed` fits in u64; promoting to u128
    // before the multiply makes the product overflow-safe up to
    // total * 2^64 — way past any realistic vesting setup, but we still
    // bail explicitly if the product wraps.
    __uint128_t elapsed = (__uint128_t)(t - start);
    __uint128_t product = total * elapsed;
    if (elapsed != 0 && product / elapsed != total) {
        pyde_revert_str("vesting: overflow");
    }
    return product / (__uint128_t)duration;
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

// configure is the constructor — runs once at deploy. The manifest tags this
// as [functions.configure].attributes = ["constructor"] so the chain rejects
// any post-deploy call; the in-source `configured` flag is defence in depth.
//
// Reverts:
//   - "vesting: already configured" — second-call guard.
//   - "vesting: beneficiary is zero address" — zero-address sink guard;
//     prevents silently locking tokens forever.
//   - "vesting: total must be non-zero" — a zero-total vesting is
//     indistinguishable from an unfunded contract; reject so the
//     misconfiguration is loud rather than silent.
//   - "vesting: cliff exceeds duration" — past `start + duration` the vested
//     amount is `total` regardless of cliff, so a cliff longer than duration
//     is a configuration error.
void __configure_impl(
    pyde_address beneficiary,
    __uint128_t total,
    uint64_t start_time,
    uint64_t cliff_seconds,
    uint64_t duration_seconds) {
    if (configured_get()) {
        pyde_revert_str("vesting: already configured");
    }

    if (addr_is_zero(beneficiary)) {
        pyde_revert_str("vesting: beneficiary is zero address");
    }

    if (total == 0) {
        pyde_revert_str("vesting: total must be non-zero");
    }

    if (cliff_seconds > duration_seconds) {
        pyde_revert_str("vesting: cliff exceeds duration");
    }

    beneficiary_set(beneficiary);
    total_amount_set(total);
    start_time_set(start_time);
    cliff_seconds_set(cliff_seconds);
    duration_seconds_set(duration_seconds);
    configured_set(true);

    emit_Configured(beneficiary, total, start_time, cliff_seconds, duration_seconds);
}

// fund is the payable entry — accept PYDE into the contract's own balance.
// Anyone can fund. Emits Funded(from, amount) for off-chain accounting. The
// amount sent is tx_value() (the chain credits the contract's balance
// automatically; this entry just provides a labelled funding surface + audit
// event).
//
// Refuses zero-value calls so accidental fund() invocations without an
// attached value are caught loudly instead of emitting a meaningless event.
void __fund_impl(void) {
    __uint128_t amount = tx_value_u128();
    if (amount == 0) {
        pyde_revert_str("vesting: fund requires non-zero value");
    }
    pyde_address from = pyde_caller();
    emit_Funded(from, amount);
}

// release is permissionless: anyone can call. Pays `vested - released` to the
// configured beneficiary from this contract's balance, bumps `released`, emits
// Released.
//
// State + event happen BEFORE the value transfer (checks-effects-
// interactions). Pyde has no implicit reentrancy — only cross_call can
// re-enter — and pay() resolves through the native transfer host fn rather
// than cross_call, so re-entry is impossible. The ordering is for discipline +
// parity with the simple-multisig template, not because the contract is
// vulnerable without it.
//
// Reverts:
//   - "vesting: not configured" — configure() hasn't been called yet.
//   - "vesting: nothing to release" — vested == released.
//   - "vesting: TransferFailed" — the contract's balance is short of the
//     computed payout. State changes are rolled back atomically with the
//     revert, so `released` does not advance.
__uint128_t __release_impl(void) {
    if (!configured_get()) {
        pyde_revert_str("vesting: not configured");
    }

    uint64_t now = (uint64_t)wave_timestamp();
    __uint128_t vested = vested_at(now);
    __uint128_t already_released = released_get();

    if (vested <= already_released) {
        pyde_revert_str("vesting: nothing to release");
    }

    __uint128_t payout = vested - already_released;
    __uint128_t new_released = already_released + payout;
    if (new_released < already_released) {
        pyde_revert_str("vesting: overflow");
    }
    released_set(new_released);

    pyde_address beneficiary = beneficiary_get();
    emit_Released(beneficiary, payout, new_released);

    // Actual PYDE transfer to the beneficiary. If the contract is
    // underfunded the host fn returns non-zero and pay() reverts the whole
    // frame — `released` rolls back with it, so a subsequent topped-up
    // release() re-computes the same payout cleanly.
    pay(beneficiary, payout);

    return payout;
}

// vested_amount is a view: vested at current wave_timestamp.
__uint128_t __vested_amount_impl(void) {
    return vested_at((uint64_t)wave_timestamp());
}

// releasable is a view: amount that could be released right now
// (vested - released).
__uint128_t __releasable_impl(void) {
    __uint128_t v = vested_at((uint64_t)wave_timestamp());
    __uint128_t r = released_get();
    if (v <= r) {
        return (__uint128_t)0;
    }
    return v - r;
}

// released_amount is a view: cumulative released so far.
__uint128_t __released_amount_impl(void) {
    return released_get();
}

// configured_flag is a view: false if configure hasn't been called, true
// otherwise.
bool __configured_flag_impl(void) {
    return configured_get();
}
