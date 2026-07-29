# pyde-template — Go upgradeable proxy

A thin proxy whose logic contract is admin-swappable. `forward(fn, calldata)`
runs the logic's code in THIS contract's frame via a **delegate-call**: the
logic's storage writes land in the proxy's slots, so shared state (`value`)
survives when the admin swaps in a new implementation. That's the whole point.

```sh
otigen build          # generate + compile
otigen test           # 16 admin / upgrade / gating tests
```

## Live walk-through (devnet)

The full `forward` flow needs a deployed logic contract (any contract that
writes a `value` slot). Deploy a logic v1, deploy this proxy pointing at it,
drive state through `forward`, then `upgrade_to` a v2 and confirm `get_value`
is preserved.

```sh
otigen devnet --rpc-listen 127.0.0.1:9933 &
# proxy init() records the deployer as admin + the initial logic address:
otigen deploy --from devnet-0 --password-stdin --args <logic_v1> <<< devnet
otigen call <proxy> forward set_value 0x<calldata> --from devnet-0 --password-stdin <<< devnet
otigen call <proxy> get_value
otigen call <proxy> upgrade_to <logic_v2> --from devnet-0 --password-stdin <<< devnet
otigen call <proxy> get_value   # unchanged across the upgrade
```

Admin controls: `upgrade_to`, `transfer_admin`, `renounce_admin` (irreversible —
freezes the logic pointer forever).

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
