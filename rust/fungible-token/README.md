# fungible-token — the PTS-F reference implementation

The canonical `pts-f/1` fungible token
([PIP-0005](https://github.com/pyde-net/pips/blob/main/pip-0005-pyde-token-standard.md)),
hand-written on the macro substrate in its default extension
configuration. When `type = "token"` manifest generation lands in
otigen, this contract is what the generator emits for this
configuration — until then it is the normative reference and the
conformance-vector source.

What the surface fixes (each deviation points at a documented loss
class on other chains — see the
[Token Standard companion](https://book.pyde.network/companion/TOKEN_STANDARD)):

| Design choice | Kills |
|---|---|
| Mutations return nothing; failures are canonical `token:*` codes | the inconsistent-boolean-return wrapper tax |
| Delta-only allowances with mandatory TTL-capped expiry | the standing-approval drain economy + the approve race |
| `approve(spender, amount)` sugar auto-applies the max TTL | porting friction, without readmitting "unlimited forever" |
| `transfer_call` settles, emits, then notifies — receiver must return the `ACK_TOKEN` acknowledgement | tokens stranded in unaware contracts AND mid-update hook reentrancy |
| Recipient guards: zero address + the token's own address | the largest measured stuck-token bucket |
| Roles renounced by provable zeroing; zero-sentinel `Transfer` family | invisible issuer powers; multi-event supply accounting |
| Per-holder balance slots; supply written only by mint/burn | hot-cell serialization under parallel execution |

## Layout

- `otigen.toml` — the full pts-f/1 surface: 14 views, 13 mutations,
  3 events, typed state schema (a grant = two sibling
  `(owner, spender)`-keyed slots).
- `src/lib.rs` — the implementation. Protocol constants
  (`ACK_TOKEN`, `ROLE_*`, `MAX_ALLOWANCE_TTL_WAVES`) are pinned
  literals with their derivations in comments.
- `tests/contract.test.toml` — 27 tests pinning the revert codes,
  events, expiry time-travel (per-call `wave_id` overrides), the CAS
  primitive, and role rotation.
- `tests/run_e2e.sh` + `tests/fungible_e2e.py` — live-devnet loop:
  constructor args over the wire, exact string returns, batch views.

`transfer_call`'s happy path needs a real receiver contract — the
`token-vault` example is the reference receiver and carries that
end-to-end coverage.

## Run

```bash
make test          # behaviour suite via otigen test
make test-vvvv     # + gas, events, traces, storage diffs
bash examples/fungible-token/tests/run_e2e.sh   # live devnet loop
```
