# pyde-template — Go counter

A minimal Pyde smart contract in Go. You write plain typed Go (`main.go`)
plus `otigen.toml`; `otigen build` generates `pyde_gen.go` (the
`//go:wasmexport` dispatch + typed `State` accessors) and compiles to wasm.

```sh
otigen build          # generate + compile
otigen test           # run tests/
otigen devnet --rpc-listen 127.0.0.1:9933 &   # local chain
otigen deploy --from devnet-0 --password-stdin <<< devnet
otigen call <addr> increment --from devnet-0 --password-stdin <<< devnet
otigen call <addr> get
```

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
