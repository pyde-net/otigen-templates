# pyde-template — Go fungible token (PTS-F, `pts-f/1`)

The PIP-0005 `pts-f/1` reference token in Go, default extension config:
revert-only mutations with canonical `token:*` codes, delta-only allowances
with a mandatory TTL-capped expiry, settle-then-notify `transfer_call` that
requires an `ACK_TOKEN` acknowledgement, role-gated `mint` / `burn`, and
manager-rotated minter/manager roles renounced by provable zeroing. You write
plain typed Go (`main.go`) plus `otigen.toml`; `otigen build` generates
`pyde_gen.go` (the `//go:wasmexport` dispatch, typed `State` accessors, borsh
codecs, and the `Emit`-able event structs) and compiles to wasm.

```sh
otigen build          # generate pyde_gen.go + compile to build/contract.wasm
otigen test           # run tests/contract.test.toml (27 behaviour vectors)

otigen devnet --rpc-listen 127.0.0.1:9933 &   # local chain
# Deploy: init(name, symbol, decimals, initial_supply, max_supply); max_supply 0 = uncapped.
otigen deploy --from devnet-0 --password-stdin --args "Pyde Demo" PYDM 9 1000000 0 <<< devnet
otigen call <addr> total_supply
otigen call <addr> transfer <recipient> 250 --from devnet-0 --password-stdin <<< devnet
otigen call <addr> balance_of <recipient>
```

Amounts are `uint128` in the token's smallest unit; `decimals` is a display-only
hint (PTS default 9 = native quanta parity). `pyde_gen.go` is generated —
regenerated on every `otigen build`. Don't edit it.
