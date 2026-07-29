# pyde-template — Go NFT (pts-n/1)

The PTS-N reference NFT in Go: per-id owners, role-gated `mint` that assigns
monotonic never-recycled ids, on-chain `token_uri` metadata, per-id `approve`
and collection-wide `set_approval_for_all`, `burn`, and a settle-then-notify
`transfer_call` that moves ownership and emits `Transfer` before the recipient's
`on_nft_received` must return `ACK_NFT` (or the whole operation unwinds). You
write plain typed Go (`main.go`) plus `otigen.toml`; `otigen build` generates
`pyde_gen.go` (the `//go:wasmexport` dispatch, typed `State` accessors, and
event structs with `.Emit()`) and compiles to wasm.

```sh
otigen build          # generate pyde_gen.go + compile to wasm
otigen test           # run tests/

otigen devnet --rpc-listen 127.0.0.1:9933 &   # local chain
# Deploy — constructor args are name, symbol, max_supply (0 = uncapped):
otigen deploy --from devnet-0 --password-stdin --args "Pyde Relics" RELIC 0 <<< devnet
otigen call <addr> mint <recipient> ipfs://relic-1 --from devnet-0 --password-stdin <<< devnet
otigen call <addr> owner_of 1
otigen call <addr> balance_of <recipient>
```

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
