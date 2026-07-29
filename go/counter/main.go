package main

import pyde "github.com/pyde-net/pyde-host/go"

// Keeps the SDK import referenced even though this minimal contract does not
// yet use a pyde.* type in its own signatures.
var _ = pyde.StatusOK

// Increment adds one to the counter and returns the new value.
//
// Ports the Rust `increment()` entry:
//
//	let next = storage::counter().read().wrapping_add(1);
//	storage::counter().write(next);
//	next
//
// Go's uint64 addition wraps on overflow, matching Rust's `wrapping_add(1)`.
// `State.Counter` is generated from the [state] schema in otigen.toml — you
// never derive a storage slot or touch borsh yourself.
func Increment() uint64 {
	next := State.Counter.Get() + 1
	State.Counter.Set(next)
	return next
}

// Get reads the current counter without mutating it (a `view` function).
//
// Ports the Rust `get()` entry: `storage::counter().read()`.
func Get() uint64 {
	return State.Counter.Get()
}

// main is required by TinyGo's wasm target. The chain never calls it — it
// dispatches the //go:wasmexport entry points generated into pyde_gen.go.
func main() {}
