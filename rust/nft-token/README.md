# nft-token — the PTS-N reference implementation

The canonical `pts-n/1` non-fungible token
([PIP-0005 §12](https://github.com/pyde-net/pips/blob/main/pip-0005-pyde-token-standard.md)),
hand-written on the macro substrate in its default configuration.
When `type = "token"` manifest generation lands in otigen, this is
what the generator emits for `standard = "pts-n/1"`.

Same philosophy as [`fungible-token`](../fungible-token/), per-id
state:

| Design choice | Why |
|---|---|
| Mutations revert-only with canonical `token:*` codes | no boolean ambiguity (`mint` returns the fresh id — a creation, not a status) |
| `transfer_call` settles, emits, then notifies; receiver must return `ACK_NFT` | the `safeTransferFrom` reentrancy genre has no half-updated window; a fallback can't swallow an NFT |
| `next_id` monotonic and separate from the live count | burned ids are never recycled |
| Per-id owner slots | transfers of distinct ids never conflict under parallel execution |
| On-chain `token_uris` (16 KB values) | real metadata JSON without off-chain rot |
| Roles renounced by provable zeroing | consent-visible control |
| **No royalties** in the spec | an interface cannot enforce economics against adversarial marketplaces — marketplace policy |

Honest v1 cost: sequential id minting is a hot slot that serializes
mass mints; a pre-partitioned id-range extension is sketched for a
future `pts-n/2`.

## Run

```bash
make test          # 12-test behaviour suite via otigen test
bash examples/nft-token/tests/run_e2e.sh   # live devnet loop
```
