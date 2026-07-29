// A factory: mint and drive child contracts via pyde.New(template).
//
// A factory creates fresh instances of a TEMPLATE — and the template is
// just ANY contract you have already deployed. Deploy a contract, copy
// its address, and pass that address to Create. Each child is a
// first-class contract — its own address, its own isolated storage —
// sharing the template's already-cached code, so nothing is copied or
// recompiled.
//
// The template is instantiated with NO constructor arguments, so it
// works with any contract that has no required constructor (the
// built-in `counter` template is a perfect fit). To drive a child,
// this factory cross-calls its `increment() -> u64`, so the template
// should expose that.
//
// A child's address is a pure function of (factory, template, salt),
// so it is predictable off-chain before the child exists. This example
// shows the THREE ways to choose that salt and the THREE ways to then
// reach a child:
//
//	CREATE — choose the salt
//	  * Create          — AUTO. The factory keeps a next_key counter and
//	                       salts each child with it, so a caller manages
//	                       no salt at all.
//	  * CreateWithKey   — an explicit u64 salt (a user id, a nonce).
//	  * CreateNamed     — a string salt (a name, a market pair).
//
//	INTERACT — reach a child
//	  * Bump       — BY u64 KEY: look it up in the registry, call it.
//	  * BumpNamed  — BY STRING NAME: look it up, call it.
//	  * BumpAt     — BY ADDRESS: call any child directly, no lookup.
package main

import (
	"unicode/utf8"

	pyde "github.com/pyde-net/pyde-host/go"
)

// saltOfU64 is the Go equivalent of pyde::Salt::of(&key): Poseidon2 of the
// borsh encoding of the u64 (8 little-endian bytes).
func saltOfU64(key uint64) pyde.Bytes32 {
	return pyde.Poseidon2(pyde.NewEncoder().U64(key).Finish())
}

// saltOfName is the Go equivalent of pyde::Salt::of(&name): Poseidon2 of the
// borsh encoding of the string (u32 length prefix + UTF-8).
func saltOfName(name string) pyde.Bytes32 {
	return pyde.Poseidon2(pyde.NewEncoder().String(name).Finish())
}

// ─── create ─────────────────────────────────────────────────────────

// Create mints a child with an AUTO salt — the factory's next_key counter.
// The caller manages nothing: each call takes the next slot. The child is
// recorded in the u64 registry and returned.
func Create(template pyde.Address) pyde.Address {
	key := State.NextKey.Get()
	child := mint(template, saltOfU64(key))
	State.Children.Set(key, child)
	State.NextKey.Set(key + 1)
	record()
	return child
}

// CreateWithKey mints a child at an explicit u64 salt key (e.g. a user id).
// The address is (factory, template, Salt::of(key)), so the same key always
// targets the same child — a second call reverts "exists".
func CreateWithKey(template pyde.Address, key uint64) pyde.Address {
	child := mint(template, saltOfU64(key))
	State.Children.Set(key, child)
	record()
	return child
}

// CreateNamed mints a child at a string salt (e.g. a market pair "ETH/USDC").
// Salt::of borsh-encodes the string and hashes it, so any string is a valid,
// deterministic identity. Recorded in the name registry.
func CreateNamed(template pyde.Address, name string) pyde.Address {
	child := mint(template, saltOfName(name))
	State.Named.Set(name, child)
	record()
	return child
}

// ─── look up ────────────────────────────────────────────────────────

// ChildOf returns a child by its u64 key — the factory's getPair. Zero
// address if that key was never created.
func ChildOf(key uint64) pyde.Address {
	return State.Children.Get(key)
}

// ChildOfName returns a child by its string name. Zero address if never
// created.
func ChildOfName(name string) pyde.Address {
	return State.Named.Get(name)
}

// NextKey is the key Create will use next.
func NextKey() uint64 {
	return State.NextKey.Get()
}

// Created is the total number of children this factory has minted.
func Created() uint64 {
	return State.Created.Get()
}

// ─── interact ───────────────────────────────────────────────────────

// Bump drives a child BY u64 KEY: pull it from the registry, then call its
// increment via a cross-call. Returns the child's new value.
func Bump(key uint64) uint64 {
	return increment(State.Children.Get(key))
}

// BumpNamed drives a child BY STRING NAME.
func BumpNamed(name string) uint64 {
	return increment(State.Named.Get(name))
}

// BumpAt drives a child BY ADDRESS — no lookup. This is how a contract talks
// to a contract it (or anyone) created: by address, sharing the template's
// ABI, no typed handle required.
func BumpAt(child pyde.Address) uint64 {
	return increment(child)
}

// ─── internals ──────────────────────────────────────────────────────

// mint instantiates template at salt with NO constructor args — works with
// any contract that has no required constructor. Surfaces the engine's
// outcomes as clean reverts: a repeat identity is "exists", an unknown
// template is "template-not-found", and any other failure is
// "instantiate-failed".
func mint(template pyde.Address, salt pyde.Bytes32) pyde.Address {
	child, data, rc := pyde.New(template).Salt(salt).Instantiate()
	if rc != pyde.StatusOK {
		switch rc {
		case pyde.ErrChildAddressTaken:
			pyde.Revert("exists")
		case pyde.ErrTemplateNotContract:
			pyde.Revert("template-not-found")
		default:
			// Bubble the child constructor's revert message when present,
			// mirroring Rust's e.revert_message().unwrap_or("instantiate-failed").
			if len(data) > 0 && utf8.Valid(data) {
				pyde.Revert(string(data))
			}
			pyde.Revert("instantiate-failed")
		}
	}
	return child
}

// increment cross-calls a child's increment (no args) and returns the new
// value.
func increment(child pyde.Address) uint64 {
	out, rc := pyde.Call(child, "increment").Exec()
	if rc != pyde.StatusOK {
		// Forward the callee's revert message when present, mirroring
		// Rust's e.revert_message().unwrap_or("child-call-failed").
		if len(out) > 0 && utf8.Valid(out) {
			pyde.Revert(string(out))
		}
		pyde.Revert("child-call-failed")
	}
	return pyde.NewDecoder(out).U64()
}

// record ticks the minted-children counter.
func record() {
	n := State.Created.Get()
	State.Created.Set(n + 1)
}

// main is required by TinyGo's wasm target; the chain dispatches the generated
// //go:wasmexport entry points, never main.
func main() {}
