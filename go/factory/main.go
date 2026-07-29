package main

import pyde "github.com/pyde-net/pyde-host/go"

// A factory: mint and drive child contracts via pyde.New(template).
//
// A TEMPLATE is just ANY contract you have already deployed — deploy one,
// copy its address, and pass it to Create. Each child is a first-class
// contract (its own address, its own isolated storage) that shares the
// template's already-cached code, so nothing is copied or recompiled. The
// template is instantiated with NO constructor args, so any contract without
// a required constructor works (the built-in `counter` template is a perfect
// fit — it exposes `increment() -> uint64`, which the Bump* functions call).
//
// A child's address is a pure function of (factory, template, salt), so this
// example shows the THREE ways to choose that salt and the THREE ways to then
// reach a child:
//
//   CREATE — choose the salt
//     Create          AUTO   — the factory's next_key counter; caller manages nothing
//     CreateWithKey   u64    — an explicit key (a user id, a nonce)
//     CreateNamed     string — a name (a market pair "ETH/USDC")
//
//   INTERACT — reach a child
//     Bump       by u64 key   — look it up in the registry, then cross-call it
//     BumpNamed  by string    — look it up, then cross-call it
//     BumpAt     by address   — call any child directly, no lookup

// saltU64 derives a child salt from a u64 key. Deterministic: the same key
// always maps to the same child, so a repeat Create reverts "exists".
func saltU64(key uint64) pyde.Bytes32 {
	return pyde.Blake3(pyde.NewEncoder().U64(key).Finish())
}

// saltName derives a child salt from a string name.
func saltName(name string) pyde.Bytes32 {
	return pyde.Blake3(pyde.NewEncoder().String(name).Finish())
}

// ── create ──────────────────────────────────────────────────────────

// Create mints a child with an AUTO salt (the factory's next_key counter),
// records it in the u64 registry, and returns its address.
func Create(template pyde.Address) pyde.Address {
	key := State.NextKey.Get()
	child := mint(template, saltU64(key))
	State.Children.Set(key, child)
	State.NextKey.Set(key + 1)
	return child
}

// CreateWithKey mints a child at an explicit u64 salt key. The same key always
// targets the same child, so a second call with it reverts "exists".
func CreateWithKey(template pyde.Address, key uint64) pyde.Address {
	child := mint(template, saltU64(key))
	State.Children.Set(key, child)
	return child
}

// CreateNamed mints a child at a string salt (e.g. a market pair "ETH/USDC").
func CreateNamed(template pyde.Address, name string) pyde.Address {
	child := mint(template, saltName(name))
	State.Named.Set(name, child)
	return child
}

// ── look up ─────────────────────────────────────────────────────────

// ChildOf returns the child at a u64 key (zero address if never created).
func ChildOf(key uint64) pyde.Address { return State.Children.Get(key) }

// ChildOfName returns the child at a string name.
func ChildOfName(name string) pyde.Address { return State.Named.Get(name) }

// NextKey is the key Create will use next.
func NextKey() uint64 { return State.NextKey.Get() }

// Created is the total number of children this factory has minted.
func Created() uint64 { return State.Created.Get() }

// ── interact ────────────────────────────────────────────────────────

// Bump drives a child BY u64 KEY: pull it from the registry, then cross-call
// its increment(). Returns the child's new value.
func Bump(key uint64) uint64 { return increment(State.Children.Get(key)) }

// BumpNamed drives a child BY STRING NAME.
func BumpNamed(name string) uint64 { return increment(State.Named.Get(name)) }

// BumpAt drives a child BY ADDRESS — no lookup. This is how a contract talks
// to a contract it (or anyone) created: by address, sharing the template's ABI.
func BumpAt(child pyde.Address) uint64 { return increment(child) }

// ── internals ───────────────────────────────────────────────────────

// mint instantiates template at salt with no constructor args, tallies the
// child, and surfaces the engine's outcomes as clean reverts.
func mint(template pyde.Address, salt pyde.Bytes32) pyde.Address {
	child, rc := pyde.New(template).Salt(salt).Instantiate()
	if rc != pyde.StatusOK {
		switch rc {
		case pyde.ErrChildAddressTaken:
			pyde.Revert("exists")
		case pyde.ErrTemplateNotContract:
			pyde.Revert("template-not-found")
		default:
			pyde.Revert("instantiate-failed")
		}
	}
	State.Created.Set(State.Created.Get() + 1)
	EmitChildCreated(template, child)
	return child
}

// increment cross-calls a child's increment() -> uint64 and returns the value.
func increment(child pyde.Address) uint64 {
	out, rc := pyde.Call(child, "increment").Exec()
	if rc != pyde.StatusOK {
		pyde.Revert("child-call-failed")
	}
	return pyde.NewDecoder(out).U64()
}

// main is required by TinyGo's wasm target; the chain dispatches the generated
// //go:wasmexport entry points, never main.
func main() {}
