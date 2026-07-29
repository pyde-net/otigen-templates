# pyde-template — Go fungible token

A fungible token in Go: balances, allowances (`approve` / `transfer_from`),
owner-gated `mint`, and `Transfer` / `Approval` events. You write plain typed
Go (`main.go`) plus `otigen.toml`; `otigen build` generates `pyde_gen.go` (the
`//go:wasmexport` dispatch, typed `State` accessors, and `Emit*` helpers) and
compiles to wasm.

```sh
otigen build          # generate + compile
otigen test           # run tests/

otigen devnet --rpc-listen 127.0.0.1:9933 &   # local chain
# Deploy with an initial supply (constructor arg):
otigen deploy --from devnet-0 --password-stdin --args 1000000000 <<< devnet
otigen call <addr> total_supply
otigen call <addr> transfer <recipient> 250 --from devnet-0 --password-stdin <<< devnet
otigen call <addr> balance_of <recipient>
```

Amounts are `uint128` in the token's smallest unit — there are no decimals in
storage; a UI divides by whatever precision you advertise off-chain.

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
