# pyde-template — Go simple-multisig

A 3-signer FALCON-512 multisig in Go — the canonical Pyde example of
in-contract post-quantum signature verification. Three signer IDs
(`Poseidon2(falcon_pubkey)`) are registered at deploy; `execute` accepts up to
three `(pubkey, signature)` pairs plus a nonce, and once `threshold` distinct
FALCON-512 signatures verify against the on-chain-computed `action_digest` it
pays `transfer(target, amount)` and marks the digest used (anti-replay). The
digest binds every signature to `(self_address, chain_id, target, amount,
nonce)`, so a sig can't be replayed against a different intent, chain, or
contract. You write plain typed Go (`main.go`) plus `otigen.toml`; `otigen
build` generates `pyde_gen.go` (the `//go:wasmexport` dispatch, typed `State`
accessors, and event `Emit()` methods) and compiles to wasm.

```sh
otigen build          # generate pyde_gen.go + compile to wasm
otigen test           # run tests/

otigen devnet --rpc-listen 127.0.0.1:9933 &   # local chain
# Deploy with (threshold, signer0, signer1, signer2) constructor args:
otigen deploy --from devnet-0 --password-stdin --args 2 <hash0> <hash1> <hash2> <<< devnet
otigen call <addr> get_threshold
otigen call <addr> action_digest <target> 500 1
otigen call <addr> is_used <digest>
```
