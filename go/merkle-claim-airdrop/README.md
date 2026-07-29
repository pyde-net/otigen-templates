# pyde-template — Go Merkle-claim airdrop

A gas-efficient airdrop: an off-chain process precomputes a Merkle tree of
`(claimant, amount)` allocations, the admin commits the 32-byte root once, and
claimants prove inclusion with a sibling path to pull their allocation. The
whole allocation costs ONE 32-byte root on-chain; each claim is `O(log n)`.

```sh
otigen build          # generate + compile
otigen test           # 17 tests — including the Rust reference's exact tree vectors
```

## Tree encoding (build your tree to match)

```
Leaf:  Blake3("PYDE_LEAF" ‖ claimant_addr_32 ‖ amount_u128_be_16)
Node:  Blake3("PYDE_NODE" ‖ left_hash_32 ‖ right_hash_32)
Proof: [position_byte ‖ sibling_hash_32] per level
       position 0 ⇒ running hash is the LEFT child (sibling on the right)
       position 1 ⇒ running hash is the RIGHT child (sibling on the left)
```

Amount is **big-endian** u128 — the standard Solidity-style airdrop encoding, so
community tree generators interoperate. The domain-separation tags keep a leaf
hash from ever colliding with an internal node hash.

## Live walk-through (devnet)

```sh
otigen devnet --rpc-listen 127.0.0.1:9933 &
otigen deploy --from devnet-0 --password-stdin <<< devnet          # runs init(): deployer = admin
otigen call <addr> fund --value 10000 --from devnet-0 --password-stdin <<< devnet
otigen call <addr> set_root 0x<root> --from devnet-0 --password-stdin <<< devnet
otigen call <addr> claim <amount> 0x<proof> --from devnet-0 --password-stdin <<< devnet
```

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
