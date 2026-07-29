// vesting — linear-with-cliff token vesting. Canonical Pyde example of a
// time-locked allocation: configure at deploy time with
// (beneficiary, total, start, cliff, duration), fund the contract with at
// least `total` PYDE, then anyone can call release() over time to forward the
// vested-but-unreleased portion to the beneficiary. The schedule is evaluated
// against WaveTimestamp().
//
// Vesting schedule:
//
//	t = WaveTimestamp()
//	if t < start + cliff:      vested = 0
//	if t >= start + duration:  vested = total
//	else:                      vested = total * (t - start) / duration
//
// After the cliff is crossed, vesting is linear from `start` — not from the
// cliff. The cliff just delays the first release; once past, you can release
// (cliff / duration) * total in one shot, then linearly thereafter. (Matches
// OpenZeppelin VestingWallet's reference semantics.)

package main

import pyde "github.com/pyde-net/pyde-host/go"

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

// pay pushes `amount` PYDE from this contract's balance to `to`. Reverts the
// whole frame on a non-zero return code — release() relies on this to
// propagate "contract is underfunded" failures back to the caller as a clean
// revert rather than silently advancing `released`.
func pay(to pyde.Address, amount pyde.U128) {
	if !pyde.TryTransfer(to, amount) {
		pyde.Revert("vesting: TransferFailed")
	}
}

// saturatingAddU64 mirrors Rust's u64::saturating_add — clamps at MaxU64 on
// overflow instead of wrapping.
func saturatingAddU64(a uint64, b uint64) uint64 {
	sum := a + b
	if sum < a {
		return ^uint64(0)
	}
	return sum
}

// ────────────────────────────────────────────────────────────────
// Vesting math
// ────────────────────────────────────────────────────────────────

// vestedAt computes `vested = total * (t - start) / duration` for the given
// timestamp. Returns 0 before cliff, `total` after duration ends.
func vestedAt(t uint64) pyde.U128 {
	start := State.StartTime.Get()
	cliff := State.CliffSeconds.Get()
	duration := State.DurationSeconds.Get()
	total := State.TotalAmount.Get()

	if duration == 0 {
		// Degenerate: zero-duration vesting = fully vested at start.
		if t >= start {
			return total
		}
		return pyde.U128{}
	}

	if t < saturatingAddU64(start, cliff) {
		return pyde.U128{}
	}

	if t >= saturatingAddU64(start, duration) {
		return total
	}

	// Inside the linear window. `elapsed` fits in u64; promoting to u128
	// before the multiply makes the product overflow-safe up to
	// total * 2^64 — way past any realistic vesting setup, but we still
	// bail explicitly if the product wraps.
	elapsed := pyde.U128From(t - start)
	product, ok := total.MulChecked(elapsed)
	if !ok {
		pyde.Revert("vesting: overflow")
		return pyde.U128{}
	}
	return product.Div(pyde.U128From(duration))
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

// Configure is the constructor — runs once at deploy. The manifest tags this
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
func Configure(
	beneficiary pyde.Address,
	total pyde.U128,
	startTime uint64,
	cliffSeconds uint64,
	durationSeconds uint64,
) {
	if State.Configured.Get() {
		pyde.Revert("vesting: already configured")
	}

	if beneficiary.IsZero() {
		pyde.Revert("vesting: beneficiary is zero address")
	}

	if total.IsZero() {
		pyde.Revert("vesting: total must be non-zero")
	}

	if cliffSeconds > durationSeconds {
		pyde.Revert("vesting: cliff exceeds duration")
	}

	State.Beneficiary.Set(beneficiary)
	State.TotalAmount.Set(total)
	State.StartTime.Set(startTime)
	State.CliffSeconds.Set(cliffSeconds)
	State.DurationSeconds.Set(durationSeconds)
	State.Configured.Set(true)

	Configured{
		Beneficiary:     beneficiary,
		TotalAmount:     total,
		StartTime:       startTime,
		CliffSeconds:    cliffSeconds,
		DurationSeconds: durationSeconds,
	}.Emit()
}

// Fund is the payable entry — accept PYDE into the contract's own balance.
// Anyone can fund. Emits Funded(from, amount) for off-chain accounting. The
// amount sent is TxValue() (the chain credits the contract's balance
// automatically; this entry just provides a labelled funding surface + audit
// event).
//
// Refuses zero-value calls so accidental fund() invocations without an
// attached value are caught loudly instead of emitting a meaningless event.
func Fund() {
	amount := pyde.TxValue()
	if amount.IsZero() {
		pyde.Revert("vesting: fund requires non-zero value")
	}
	from := pyde.Caller()
	Funded{From: from, Amount: amount}.Emit()
}

// Release is permissionless: anyone can call. Pays `vested - released` to the
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
func Release() pyde.U128 {
	if !State.Configured.Get() {
		pyde.Revert("vesting: not configured")
	}

	now := pyde.WaveTimestamp()
	vested := vestedAt(now)
	alreadyReleased := State.Released.Get()

	if vested.Lte(alreadyReleased) {
		pyde.Revert("vesting: nothing to release")
	}

	payout := vested.Sub(alreadyReleased)
	newReleased, ok := alreadyReleased.AddChecked(payout)
	if !ok {
		pyde.Revert("vesting: overflow")
		return pyde.U128{}
	}
	State.Released.Set(newReleased)

	beneficiary := State.Beneficiary.Get()
	Released{
		Beneficiary:   beneficiary,
		Amount:        payout,
		TotalReleased: newReleased,
	}.Emit()

	// Actual PYDE transfer to the beneficiary. If the contract is
	// underfunded the host fn returns non-zero and pay() reverts the whole
	// frame — `released` rolls back with it, so a subsequent topped-up
	// release() re-computes the same payout cleanly.
	pay(beneficiary, payout)

	return payout
}

// VestedAmount is a view: vested at current WaveTimestamp.
func VestedAmount() pyde.U128 {
	return vestedAt(pyde.WaveTimestamp())
}

// Releasable is a view: amount that could be released right now
// (vested - released).
func Releasable() pyde.U128 {
	v := vestedAt(pyde.WaveTimestamp())
	r := State.Released.Get()
	if v.Lte(r) {
		return pyde.U128{}
	}
	return v.Sub(r)
}

// ReleasedAmount is a view: cumulative released so far.
func ReleasedAmount() pyde.U128 {
	return State.Released.Get()
}

// ConfiguredFlag is a view: false if configure hasn't been called, true
// otherwise.
func ConfiguredFlag() bool {
	return State.Configured.Get()
}

// main is required by TinyGo's wasm target; the chain never calls it — it
// dispatches the //go:wasmexport entry points generated into pyde_gen.go.
func main() {}
