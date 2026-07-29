package main

import pyde "github.com/pyde-net/pyde-host/go"

// Keeps the SDK import referenced before you use pyde.* types in your own
// signatures. Delete once you do (e.g. a function takes a pyde.Address).
var _ = pyde.StatusOK

// Increment adds one to the counter and returns the new value.
//
// `State.Counter` is generated from the [state] schema in otigen.toml — you
// never derive a storage slot or touch borsh yourself.
func Increment() uint64 {
	n := State.Counter.Get() + 1
	State.Counter.Set(n)
	return n
}

// Get returns the current counter without mutating (a `view` function).
func Get() uint64 { return State.Counter.Get() }

// main is required by TinyGo's wasm target. The chain never calls it — it
// dispatches the //go:wasmexport entry points generated into pyde_gen.go.
func main() {}
