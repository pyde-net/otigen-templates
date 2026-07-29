package main

import pyde "github.com/pyde-net/pyde-host/go"

// A fungible token in Go. Balances and allowances live in typed storage;
// `State.*` and the `Emit*` helpers are generated from otigen.toml into
// pyde_gen.go — you never derive a storage slot or hand-roll borsh.

// Init runs once at deploy (the [constructor] in otigen.toml). It records the
// deployer as owner and mints the initial supply to them.
func Init(initialSupply pyde.U128) {
	deployer := pyde.Caller()
	State.Owner.Set(deployer)
	State.TotalSupply.Set(initialSupply)
	State.Balances.Set(deployer, initialSupply)
	// Convention: a mint is a Transfer from the zero address.
	EmitTransfer(pyde.Address{}, deployer, initialSupply)
}

// Transfer moves `amount` from the caller to `to`, returning false (no revert)
// on an insufficient balance so callers can branch on the result.
func Transfer(to pyde.Address, amount pyde.U128) bool {
	return move(pyde.Caller(), to, amount)
}

// Approve authorises `spender` to move up to `amount` of the caller's balance.
func Approve(spender pyde.Address, amount pyde.U128) bool {
	owner := pyde.Caller()
	State.Allowances.Set(owner, spender, amount)
	EmitApproval(owner, spender, amount)
	return true
}

// TransferFrom moves `amount` from `from` to `to`, spending the caller's
// allowance. Returns false if the allowance or balance is short.
func TransferFrom(from pyde.Address, to pyde.Address, amount pyde.U128) bool {
	spender := pyde.Caller()
	allowed := State.Allowances.Get(from, spender)
	if allowed.Lt(amount) {
		return false
	}
	if !move(from, to, amount) {
		return false
	}
	State.Allowances.Set(from, spender, allowed.Sub(amount))
	return true
}

// Mint creates `amount` new tokens for `to`. Owner-only.
func Mint(to pyde.Address, amount pyde.U128) {
	pyde.Require(pyde.Caller().Equal(State.Owner.Get()), "token: caller is not the owner")
	State.TotalSupply.Set(State.TotalSupply.Get().Add(amount))
	State.Balances.Set(to, State.Balances.Get(to).Add(amount))
	EmitTransfer(pyde.Address{}, to, amount)
}

// BalanceOf returns `who`'s balance (a `view` — no state change).
func BalanceOf(who pyde.Address) pyde.U128 { return State.Balances.Get(who) }

// Allowance returns how much `spender` may still move from `owner`.
func Allowance(owner pyde.Address, spender pyde.Address) pyde.U128 {
	return State.Allowances.Get(owner, spender)
}

// TotalSupply returns the circulating supply.
func TotalSupply() pyde.U128 { return State.TotalSupply.Get() }

// Owner returns the mint authority set at deploy.
func Owner() pyde.Address { return State.Owner.Get() }

// move is the shared debit/credit path behind Transfer and TransferFrom.
func move(from pyde.Address, to pyde.Address, amount pyde.U128) bool {
	bal := State.Balances.Get(from)
	if bal.Lt(amount) {
		return false
	}
	State.Balances.Set(from, bal.Sub(amount))
	State.Balances.Set(to, State.Balances.Get(to).Add(amount))
	EmitTransfer(from, to, amount)
	return true
}

// main is required by TinyGo's wasm target; the chain never calls it — it
// dispatches the //go:wasmexport entry points generated into pyde_gen.go.
func main() {}
