package main

import pyde "github.com/pyde-net/pyde-host/go"

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

// Init runs once at deploy (the [constructor]): records the caller as admin and
// the first logic pointer. Rejects a zero admin or logic — either would brick
// the proxy.
func Init(initialLogic pyde.Address) {
	if !State.ProxyAdmin.Get().IsZero() {
		pyde.Revert("proxy: already initialized")
	}
	admin := pyde.Caller()
	if admin.IsZero() || initialLogic.IsZero() {
		pyde.Revert("proxy: init with zero address")
	}
	State.ProxyAdmin.Set(admin)
	State.ProxyLogic.Set(initialLogic)
	EmitInitialized(admin, initialLogic)
}

// UpgradeTo swaps the logic pointer. Admin-only. Doesn't touch `value` — the
// preserved state is the whole point.
func UpgradeTo(newLogic pyde.Address) {
	requireAdmin()
	if newLogic.IsZero() {
		pyde.Revert("proxy: upgrade to zero address")
	}
	oldLogic := State.ProxyLogic.Get()
	State.ProxyLogic.Set(newLogic)
	EmitUpgraded(oldLogic, newLogic)
}

// TransferAdmin rotates the admin role. Admin-only. Reverts on a zero-address
// new admin — use RenounceAdmin for that so the irrevocable lock is loud.
func TransferAdmin(newAdmin pyde.Address) {
	admin := requireAdmin()
	if newAdmin.IsZero() {
		pyde.Revert("proxy: transfer to zero address; use renounce_admin")
	}
	State.ProxyAdmin.Set(newAdmin)
	EmitAdminTransferred(admin, newAdmin)
}

// RenounceAdmin sets the admin to the zero address, freezing the logic pointer
// forever. Irreversible.
func RenounceAdmin() {
	admin := requireAdmin()
	State.ProxyAdmin.Set(pyde.Address{})
	EmitAdminTransferred(admin, pyde.Address{})
}

// Forward delegate-calls logic.function(calldata) in this contract's frame and
// returns the logic's raw return bytes verbatim — the caller decodes them per
// the logic function's documented return type.
func Forward(function string, calldata []byte) []byte {
	logic := State.ProxyLogic.Get()
	out, rc := pyde.DelegateCall(logic, function).Args(calldata).Exec()
	if rc != pyde.StatusOK {
		// The Go delegate-call surfaces a status code, not the logic's revert
		// payload, so we map the common cases to clear proxy-level messages.
		if rc == pyde.ErrInvalidFunctionName {
			pyde.Revert("proxy: logic has no such function")
		}
		pyde.Revert("proxy: delegate-call failed")
	}
	return out
}

// GetAdmin returns the current admin (zero if renounced).
func GetAdmin() pyde.Address { return State.ProxyAdmin.Get() }

// GetLogic returns the current logic pointer.
func GetLogic() pyde.Address { return State.ProxyLogic.Get() }

// GetValue reads the shared `value` slot the logic contract writes through the
// proxy — it survives upgrades.
func GetValue() uint64 { return State.Value.Get() }

// requireAdmin reverts unless the caller is the recorded admin, returning that
// admin address for the event payload.
func requireAdmin() pyde.Address {
	admin := State.ProxyAdmin.Get()
	if !pyde.Caller().Equal(admin) {
		pyde.Revert("proxy: caller is not admin")
	}
	return admin
}

// main is required by TinyGo's wasm target; the chain dispatches the generated
// //go:wasmexport entry points, never main.
func main() {}
