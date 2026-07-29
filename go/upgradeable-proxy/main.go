package main

import (
	"unicode/utf8"

	pyde "github.com/pyde-net/pyde-host/go"
)

// upgradeable-proxy — a thin proxy whose logic contract is admin-swappable.
//
// Forward(fn, calldata) runs the logic contract's code in THIS contract's frame
// via a delegate-call: self_address stays the proxy, the caller is preserved,
// and the logic's storage accessors write to the PROXY's slots — so the shared
// `value` survives an upgrade. That state preservation is the whole point.
//
// Privileged fields are prefixed `proxy_` so a logic contract that declares its
// own `admin` / `logic` field can't accidentally clobber the proxy's slots
// (slots are Poseidon2(self_address ‖ field_name), shared under delegate-call).
// `value` stays unprefixed — it's the intended shared-state demonstration field.

// zeroAddress — 32-byte all-zero address, sentinel for an unset / invalid
// address. Used to reject inputs that would permanently brick the proxy
// (zero admin = no one can ever upgrade; zero logic = every forward
// delegate-calls into a non-existent contract).
var zeroAddress = pyde.Address{}

// Init runs once at deploy (the [constructor]).
func Init(initialLogic pyde.Address) {
	// Defence-in-depth re-init guard. The manifest already tags this function
	// as ["constructor"] so the chain rejects post-deploy calls — but the
	// in-source flag catches accidental re-runs in tests + makes the invariant
	// explicit at the source level.
	if !State.ProxyAdmin.Get().Equal(zeroAddress) {
		pyde.Revert("proxy: already initialized")
	}
	admin := pyde.Caller()
	if admin.Equal(zeroAddress) || initialLogic.Equal(zeroAddress) {
		pyde.Revert("proxy: init with zero address")
	}
	State.ProxyAdmin.Set(admin)
	State.ProxyLogic.Set(initialLogic)
	Initialized{Admin: admin, InitialLogic: initialLogic}.Emit()
}

// UpgradeTo is the admin-only logic-pointer swap. Doesn't touch `value` — the
// preserved state is the whole point.
func UpgradeTo(newLogic pyde.Address) {
	admin := State.ProxyAdmin.Get()
	caller := pyde.Caller()
	if !caller.Equal(admin) {
		pyde.Revert("proxy: caller is not admin")
	}
	if newLogic.Equal(zeroAddress) {
		pyde.Revert("proxy: upgrade to zero address")
	}
	oldLogic := State.ProxyLogic.Get()
	State.ProxyLogic.Set(newLogic)
	Upgraded{OldLogic: oldLogic, NewLogic: newLogic}.Emit()
}

// TransferAdmin rotates the admin role. Admin-only. Reverts on a zero-address
// new admin — use RenounceAdmin for that path so the irrevocable lock is loud
// rather than disguised as a transfer.
func TransferAdmin(newAdmin pyde.Address) {
	admin := State.ProxyAdmin.Get()
	caller := pyde.Caller()
	if !caller.Equal(admin) {
		pyde.Revert("proxy: caller is not admin")
	}
	if newAdmin.Equal(zeroAddress) {
		pyde.Revert("proxy: transfer to zero address; use renounce_admin")
	}
	oldAdmin := admin
	State.ProxyAdmin.Set(newAdmin)
	AdminTransferred{OldAdmin: oldAdmin, NewAdmin: newAdmin}.Emit()
}

// RenounceAdmin renounces the admin role — sets the admin slot to the zero
// address. After this call NO ONE can UpgradeTo or call any admin-gated entry;
// the logic pointer is frozen at its current value forever. Irreversible.
func RenounceAdmin() {
	admin := State.ProxyAdmin.Get()
	caller := pyde.Caller()
	if !caller.Equal(admin) {
		pyde.Revert("proxy: caller is not admin")
	}
	State.ProxyAdmin.Set(zeroAddress)
	AdminTransferred{OldAdmin: admin, NewAdmin: zeroAddress}.Emit()
}

// Forward delegate-calls logic.function(calldata) in this contract's frame.
// Returns the raw bytes the logic produced as its return payload — the caller
// decodes them per the function's documented return type.
func Forward(function string, calldata []byte) []byte {
	logic := State.ProxyLogic.Get()
	out, rc := pyde.DelegateCall(logic, function).Args(calldata).Exec()
	switch rc {
	case pyde.StatusOK:
		return out
	case pyde.ErrInvalidFunctionName:
		// Mirrors Rust's Err(CallError::InvalidFunction).
		pyde.Revert("proxy: logic has no such function")
	case pyde.ErrInsufficientBalance, pyde.ErrReentrancyBlocked, pyde.ErrValueTransferNotPayable:
		// Mirrors Rust's Err(_) over the non-Reverted call-error variants.
		pyde.Revert("proxy: delegate-call failed")
	default:
		// Rust's CallError::Reverted(payload): pass the logic's revert string
		// straight through to the proxy's caller so they see exactly what the
		// logic said, not a generic "proxy failed" message. Falls back to the
		// generic message when the payload isn't valid UTF-8.
		if utf8.Valid(out) {
			pyde.Revert(string(out))
		}
		pyde.Revert("proxy: delegate-call failed")
	}
	return nil
}

// GetAdmin returns the current admin (zero if renounced).
func GetAdmin() pyde.Address {
	return State.ProxyAdmin.Get()
}

// GetLogic returns the current logic pointer.
func GetLogic() pyde.Address {
	return State.ProxyLogic.Get()
}

// GetValue reads the shared `value` slot the logic contract writes through the
// proxy.
func GetValue() uint64 {
	return State.Value.Get()
}

// main is required by TinyGo's wasm target; the chain dispatches the generated
// //go:wasmexport entry points, never main.
func main() {}
