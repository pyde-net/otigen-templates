// fungible-token — the PTS-F reference implementation (pts-f/1,
// PIP-0005), default extension configuration, ported to Go.
//
// What this surface fixes, each deviation pointing at a documented
// loss class on other chains:
//
//   - Revert-only mutations. No boolean returns to mis-handle; failures
//     are canonical machine-readable token:* codes that propagate
//     verbatim through cross_call unwinds.
//   - Expiring, delta-only allowances. Every grant carries a mandatory
//     expiry wave (TTL-capped at ~1 year); increase/decrease deltas
//     replace raw overwrites, so the approve race is dead by
//     construction and "unlimited forever" is unrepresentable.
//   - Settle-then-notify deposits. transfer_call writes balances and
//     emits Transfer FIRST, then notifies the recipient, which must
//     return the ACK_TOKEN acknowledgement — a name-miss falling
//     through to a fallback cannot silently swallow tokens. Plain
//     transfer never invokes recipient code.
//   - Consent-visible control. Minter/manager roles are declared state,
//     rotated by the manager, renounced by provable zeroing.
//   - Parallel-execution-ready layout. A transfer writes exactly the two
//     parties' balance slots; total_supply is written only by
//     mint/burn. Disjoint transfers commute under Block-STM.
//
// Storage economics: balances and allowances are zeroed, never deleted —
// the chain has no gas refunds, so sdelete is strictly costlier than
// writing zero.

package main

import pyde "github.com/pyde-net/pyde-host/go"

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

// STANDARD is the surface this contract conforms to.
const STANDARD = "pts-f/1"

// MAX_ALLOWANCE_TTL_WAVES is the hard cap on allowance lifetime: ~1 year
// at 500 ms/wave. Even a maximal grant self-destructs.
const MAX_ALLOWANCE_TTL_WAVES uint64 = 63_072_000

// MAX_BATCH is the max owners accepted by balance_of_batch.
const MAX_BATCH = 256

// MAX_CALL_DATA is the max bytes of data forwarded by transfer_call —
// bounds payload-driven gas griefing under no-refund economics.
const MAX_CALL_DATA = 4_096

// ACK_TOKEN is the required return of a conformant receiver:
// u32::from_le_bytes(Blake3("pts/on_token_received/1")[0..4]).
const ACK_TOKEN uint32 = 2_266_754_145 // LE bytes 61 ec 1b 87

// ROLE_MINTER / ROLE_MANAGER are the 32-byte role identifiers carried by
// RoleTransfer (PIP-0005 §3): Blake3("minter") / Blake3("manager").
var ROLE_MINTER = pyde.Bytes32{
	0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
	0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
	0xe5, 0xdd,
}
var ROLE_MANAGER = pyde.Bytes32{
	0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
	0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
	0x35, 0xc1,
}

// EXTENSION_FLAGS are the extension flags reported by token_info()
// (PIP-0005 §5): this is the default configuration — bit 4
// (transfer_call) + bit 5 (burnable).
const EXTENSION_FLAGS uint32 = (1 << 4) | (1 << 5)

// ZERO_ADDRESS is the null address.
var ZERO_ADDRESS = pyde.Address{}

// ─── Custom types (declared in otigen.toml [types.*]) ─────────────────

// allowance is one allowance grant, as read/written through its two
// sibling storage slots (allowance_amounts + allowance_expiries, both
// keyed (owner, spender)). {0, 0} means "no allowance".
type allowance struct {
	amount     pyde.U128
	expiryWave uint64
}

func readAllowance(owner pyde.Address, spender pyde.Address) allowance {
	return allowance{
		amount:     State.AllowanceAmounts.Get(owner, spender),
		expiryWave: State.AllowanceExpiries.Get(owner, spender),
	}
}

// ─── Constructor ───────────────────────────────────────────────────────

// Init sets metadata, mints initial_supply to the deployer, and seats the
// deployer as both minter and manager. max_supply = 0 = uncapped.
//
// decimals is a display-only hint. The PTS default (and the value every
// example uses) is 9 — parity with native quanta, 1 PYDE = 10⁹.
func Init(name string, symbol string, decimals uint8, initialSupply pyde.U128, maxSupply pyde.U128) {
	if decimals > 18 {
		pyde.Revert("token:invalid_decimals")
	}
	if !maxSupply.IsZero() && initialSupply.Gt(maxSupply) {
		pyde.Revert("token:cap_exceeded")
	}
	deployer := pyde.Caller()

	State.TokenName.Set(name)
	State.TokenSymbol.Set(symbol)
	State.TokenDecimals.Set(decimals)
	State.TotalSupply.Set(initialSupply)
	State.MaxSupply.Set(maxSupply)
	State.Minter.Set(deployer)
	State.Manager.Set(deployer)
	State.Balances.Set(deployer, initialSupply)

	// Mint sentinel: from = zero address. One event family carries all
	// supply accounting.
	TransferEvent{From: ZERO_ADDRESS, To: deployer, Amount: initialSupply}.Emit()
	RoleTransfer{Role: ROLE_MINTER, Holder: deployer, Previous: ZERO_ADDRESS}.Emit()
	RoleTransfer{Role: ROLE_MANAGER, Holder: deployer, Previous: ZERO_ADDRESS}.Emit()
}

// ─── Views ─────────────────────────────────────────────────────────────

// Standard is the runtime discovery handshake: which surface, which version.
func Standard() string {
	return STANDARD
}

func Name() string {
	return State.TokenName.Get()
}

func Symbol() string {
	return State.TokenSymbol.Get()
}

func Decimals() uint8 {
	return State.TokenDecimals.Get()
}

func TotalSupply() pyde.U128 {
	return State.TotalSupply.Get()
}

// MaxSupply — 0 = uncapped.
func MaxSupply() pyde.U128 {
	return State.MaxSupply.Get()
}

// BalanceOf — a never-written slot reads as 0.
func BalanceOf(owner pyde.Address) pyde.U128 {
	return State.Balances.Get(owner)
}

// BalanceOfBatch does batched balance reads for routers/wallets — one call
// instead of an N-round-trip loop.
func BalanceOfBatch(owners []pyde.Address) []pyde.U128 {
	if len(owners) > MAX_BATCH {
		pyde.Revert("token:batch_too_large")
	}
	out := make([]pyde.U128, 0, len(owners))
	for _, owner := range owners {
		out = append(out, State.Balances.Get(owner))
	}
	return out
}

// Allowance — remaining spendable NOW: 0 once expired.
func Allowance(owner pyde.Address, spender pyde.Address) pyde.U128 {
	return effectiveAllowance(owner, spender).amount
}

// AllowanceExpiry — the raw expiry wave of the stored grant (0 = no grant).
func AllowanceExpiry(owner pyde.Address, spender pyde.Address) uint64 {
	return State.AllowanceExpiries.Get(owner, spender)
}

// TokenInfo — the whole metadata surface in one free call.
func TokenInfo() TokenInfoView {
	return TokenInfoView{
		Name:           State.TokenName.Get(),
		Symbol:         State.TokenSymbol.Get(),
		Decimals:       State.TokenDecimals.Get(),
		TotalSupply:    State.TotalSupply.Get(),
		MaxSupply:      State.MaxSupply.Get(),
		Minter:         State.Minter.Get(),
		ExtensionFlags: EXTENSION_FLAGS,
	}
}

// Minter — zero address = renounced.
func Minter() pyde.Address {
	return State.Minter.Get()
}

func Manager() pyde.Address {
	return State.Manager.Get()
}

// ─── Transfers ─────────────────────────────────────────────────────────

// Transfer moves amount from the caller to to. Writes exactly two balance
// slots; NEVER invokes code on to.
func Transfer(to pyde.Address, amount pyde.U128) {
	from := pyde.Caller()
	moveTokens(from, to, amount)
	TransferEvent{From: from, To: to, Amount: amount}.Emit()
}

// TransferCall is settle-then-notify (PIP-0005 §6): (1) full balance
// settlement, (2) Transfer emission, (3) notify the recipient, which must
// return ACK_TOKEN. A recipient revert — or a missing/wrong acknowledgement,
// including a name-miss swallowed by a fallback — reverts the ENTIRE
// operation atomically (token:bad_receiver).
func TransferCall(to pyde.Address, amount pyde.U128, data []byte) uint32 {
	if len(data) > MAX_CALL_DATA {
		pyde.Revert("token:data_too_large")
	}
	operator := pyde.Caller()

	// (1) settle
	moveTokens(operator, to, amount)
	// (2) emit — discarded with the frame if the notify leg reverts
	TransferEvent{From: operator, To: to, Amount: amount}.Emit()

	// (3) notify. Args are filled by the token — never spoofable by the
	// sender. The receiver frame sees caller() = this contract.
	calldata := pyde.NewEncoder().Address(operator).Address(operator).U128(amount).Bytes(data).Finish()
	out, rc := pyde.Call(to, "on_token_received").Args(calldata).Exec()
	if rc == pyde.StatusOK && len(out) >= 4 {
		ack := pyde.NewDecoder(out).U32()
		if ack == ACK_TOKEN {
			return ACK_TOKEN
		}
	}
	// Wrong magic, decode mismatch, revert, no code at to, name-miss →
	// fallback (which cannot produce the ack): all collapse to the one
	// canonical rejection.
	pyde.Revert("token:bad_receiver")
	return 0
}

// TransferFrom spends a live allowance of (from, caller): expiry checked
// against the wave clock, amount decremented, balance moved. Emits Transfer
// only.
func TransferFrom(from pyde.Address, to pyde.Address, amount pyde.U128) {
	spender := pyde.Caller()
	spendAllowance(from, spender, amount)
	moveTokens(from, to, amount)
	TransferEvent{From: from, To: to, Amount: amount}.Emit()
}

// ─── Delegated spending ────────────────────────────────────────────────

// Approve sets the allowance to exactly amount with the maximum TTL
// auto-applied. "Unlimited forever" stays unrepresentable — even this grant
// self-destructs after ~1 year of waves.
func Approve(spender pyde.Address, amount pyde.U128) {
	owner := pyde.Caller()
	expiryWave := satAddU64(pyde.WaveId(), MAX_ALLOWANCE_TTL_WAVES)
	writeAllowance(owner, spender, allowance{amount: amount, expiryWave: expiryWave})
}

// IncreaseAllowance is a delta increase with an explicit expiry. An expired
// grant contributes 0 to the new amount.
func IncreaseAllowance(spender pyde.Address, amount pyde.U128, expiryWave uint64) {
	owner := pyde.Caller()
	now := pyde.WaveId()
	if expiryWave <= now || expiryWave > satAddU64(now, MAX_ALLOWANCE_TTL_WAVES) {
		pyde.Revert("token:invalid_expiry")
	}
	current := effectiveAllowance(owner, spender).amount
	next, ok := current.AddChecked(amount)
	if !ok {
		pyde.Revert("token:overflow")
	}
	writeAllowance(owner, spender, allowance{amount: next, expiryWave: expiryWave})
}

// DecreaseAllowance is a delta decrease, floored at zero. Expiry is unchanged
// for a live grant; an expired grant collapses to {0, 0}.
func DecreaseAllowance(spender pyde.Address, amount pyde.U128) {
	owner := pyde.Caller()
	current := effectiveAllowance(owner, spender)
	next := allowance{
		amount:     satSubU128(current.amount, amount),
		expiryWave: expiryIfLive(current),
	}
	writeAllowance(owner, spender, next)
}

// RevokeAllowance hard-zeros the grant. Writes {0, 0} — never sdelete.
func RevokeAllowance(spender pyde.Address) {
	owner := pyde.Caller()
	writeAllowance(owner, spender, allowance{})
}

// SetAllowanceExact is compare-and-set: change the grant to new_remaining
// ONLY if the current effective remaining equals expected_remaining.
func SetAllowanceExact(spender pyde.Address, expectedRemaining pyde.U128, newRemaining pyde.U128, expiryWave uint64) {
	owner := pyde.Caller()
	now := pyde.WaveId()
	if !effectiveAllowance(owner, spender).amount.Eq(expectedRemaining) {
		pyde.Revert("token:allowance_changed")
	}
	if newRemaining.IsZero() {
		writeAllowance(owner, spender, allowance{})
		return
	}
	if expiryWave <= now || expiryWave > satAddU64(now, MAX_ALLOWANCE_TTL_WAVES) {
		pyde.Revert("token:invalid_expiry")
	}
	writeAllowance(owner, spender, allowance{amount: newRemaining, expiryWave: expiryWave})
}

// ─── Supply & roles ────────────────────────────────────────────────────

// Mint — minter only. Reverts above max_supply (when capped). Emits the
// zero-sentinel Transfer.
func Mint(to pyde.Address, amount pyde.U128) {
	caller := pyde.Caller()
	if !caller.Equal(State.Minter.Get()) || caller == ZERO_ADDRESS {
		pyde.Revert("token:not_minter")
	}
	guardRecipient(to)
	supply := State.TotalSupply.Get()
	nextSupply, ok := supply.AddChecked(amount)
	if !ok {
		pyde.Revert("token:overflow")
	}
	cap := State.MaxSupply.Get()
	if !cap.IsZero() && nextSupply.Gt(cap) {
		pyde.Revert("token:cap_exceeded")
	}
	State.TotalSupply.Set(nextSupply)
	bal := State.Balances.Get(to)
	// Balance can't overflow if supply didn't: balance ≤ supply.
	State.Balances.Set(to, bal.Add(amount))
	TransferEvent{From: ZERO_ADDRESS, To: to, Amount: amount}.Emit()
}

// Burn burns the caller's own tokens (default config is burnable). Emits the
// zero-sentinel Transfer.
func Burn(amount pyde.U128) {
	from := pyde.Caller()
	burnTokens(from, amount)
}

// BurnFrom burns from from, spending a live allowance of (from, caller).
func BurnFrom(from pyde.Address, amount pyde.U128) {
	spender := pyde.Caller()
	spendAllowance(from, spender, amount)
	burnTokens(from, amount)
}

// SetMinter — manager only. Zero address = provable renounce; a renounced
// minter can never be re-seated once the manager is also renounced.
func SetMinter(holder pyde.Address) {
	caller := pyde.Caller()
	if !caller.Equal(State.Manager.Get()) || caller == ZERO_ADDRESS {
		pyde.Revert("token:not_manager")
	}
	previous := State.Minter.Get()
	State.Minter.Set(holder)
	RoleTransfer{Role: ROLE_MINTER, Holder: holder, Previous: previous}.Emit()
}

// SetManager — manager only. Renouncing the manager (zero address) freezes
// role governance permanently.
func SetManager(holder pyde.Address) {
	caller := pyde.Caller()
	if !caller.Equal(State.Manager.Get()) || caller == ZERO_ADDRESS {
		pyde.Revert("token:not_manager")
	}
	previous := State.Manager.Get()
	State.Manager.Set(holder)
	RoleTransfer{Role: ROLE_MANAGER, Holder: holder, Previous: previous}.Emit()
}

// ─── Internal helpers ──────────────────────────────────────────────────

// guardRecipient holds the recipient guards shared by every credit path:
// the zero address (burn must be explicit) and the token's own address.
func guardRecipient(to pyde.Address) {
	if to == ZERO_ADDRESS || to.Equal(pyde.Self()) {
		pyde.Revert("token:invalid_recipient")
	}
}

// moveTokens debits from, credits to, with the full guard set. from == to
// validates and no-ops (the two slots are the same slot).
func moveTokens(from pyde.Address, to pyde.Address, amount pyde.U128) {
	guardRecipient(to)
	fromBal := State.Balances.Get(from)
	if fromBal.Lt(amount) {
		pyde.Revert("token:insufficient_balance")
	}
	if from.Equal(to) {
		return
	}
	toBal := State.Balances.Get(to)
	toNext, ok := toBal.AddChecked(amount)
	if !ok {
		pyde.Revert("token:overflow")
	}
	State.Balances.Set(from, fromBal.Sub(amount))
	State.Balances.Set(to, toNext)
}

func burnTokens(from pyde.Address, amount pyde.U128) {
	bal := State.Balances.Get(from)
	if bal.Lt(amount) {
		pyde.Revert("token:insufficient_balance")
	}
	State.Balances.Set(from, bal.Sub(amount))
	supply := State.TotalSupply.Get()
	State.TotalSupply.Set(satSubU128(supply, amount))
	TransferEvent{From: from, To: ZERO_ADDRESS, Amount: amount}.Emit()
}

// effectiveAllowance is the stored grant if live, {0, 0} if expired or
// absent. Live means wave_id() <= expiry_wave.
func effectiveAllowance(owner pyde.Address, spender pyde.Address) allowance {
	stored := readAllowance(owner, spender)
	if !stored.amount.IsZero() && pyde.WaveId() <= stored.expiryWave {
		return stored
	}
	return allowance{}
}

// spendAllowance decrements a live allowance by amount, distinguishing
// "expired" from "too small" so integrators get the retriable signal.
func spendAllowance(owner pyde.Address, spender pyde.Address, amount pyde.U128) {
	stored := readAllowance(owner, spender)
	if !stored.amount.IsZero() && pyde.WaveId() > stored.expiryWave {
		pyde.Revert("token:allowance_expired")
	}
	live := effectiveAllowance(owner, spender)
	if live.amount.Lt(amount) {
		pyde.Revert("token:insufficient_allowance")
	}
	State.AllowanceAmounts.Set(owner, spender, live.amount.Sub(amount))
}

// writeAllowance writes + emits the absolute post-state — indexers
// reconstruct allowance state from the latest Approval per pair.
func writeAllowance(owner pyde.Address, spender pyde.Address, next allowance) {
	State.AllowanceAmounts.Set(owner, spender, next.amount)
	State.AllowanceExpiries.Set(owner, spender, next.expiryWave)
	Approval{Owner: owner, Spender: spender, Remaining: next.amount, ExpiryWave: next.expiryWave}.Emit()
}

// satAddU64 is a saturating u64 add (Rust u64::saturating_add).
func satAddU64(a uint64, b uint64) uint64 {
	sum := a + b
	if sum < a {
		return ^uint64(0)
	}
	return sum
}

// satSubU128 is a saturating u128 sub, floored at zero (Rust
// u128::saturating_sub).
func satSubU128(a pyde.U128, b pyde.U128) pyde.U128 {
	d, ok := a.SubChecked(b)
	if !ok {
		return pyde.U128{}
	}
	return d
}

// expiryIfLive mirrors Rust's `if current.amount == 0 { 0 } else {
// current.expiry_wave }`.
func expiryIfLive(current allowance) uint64 {
	if current.amount.IsZero() {
		return 0
	}
	return current.expiryWave
}

// main is required by TinyGo's wasm target; the chain never calls it.
func main() {}
