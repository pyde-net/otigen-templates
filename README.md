# otigen-templates

Starter templates for [Pyde](https://pyde.network) contracts, hosted here (not
baked into the `otigen` binary) so they can be updated without a CLI release.
`otigen new <name> --lang <lang> --from <template>` clones the matching
directory and substitutes the project name.

## Layout

```
<lang>/<template>/      source templates — rust/, go/, assemblyscript/, c/
standards/<id>/         config-only "form" tokens (pts-f/1, pts-n/1) — language-immaterial
manifest.json           catalog: sentinel, SDK pins, template list (drives the picker)
```

## Sentinel substitution

Every template is authored against a placeholder name that `otigen new`
rewrites to the user's project name:

| sentinel        | rewritten to     | appears in                       |
| --------------- | ---------------- | -------------------------------- |
| `pyde-template` | `<name>` (kebab) | `otigen.toml` name, Makefile     |
| `pyde_template` | `<name>` (snake) | `Cargo.toml`/`go.mod` crate name |

## Templates

| template               | rust | go | as | c | what it shows                                            |
| ---------------------- | :--: | :-: | :-: | :-: | ------------------------------------------------------ |
| `counter`              |  ✓   | ✓  | ✓  | ✓ | minimal contract — one u64 scalar                       |
| `fungible-token`       |  ✓   | ✓  |    |   | balances, allowances, transfer_call, role-gated supply  |
| `nft-token`            |  ✓   | ✓  |    |   | per-id owners, transfer_call, on-chain token URIs       |
| `simple-multisig`      |  ✓   | ✓  |    |   | FALCON-512 verify + replay protection                   |
| `upgradeable-proxy`    |  ✓   | ✓  |    |   | delegate-call proxy, state preserved across upgrades    |
| `merkle-claim-airdrop` |  ✓   | ✓  |    |   | off-chain commitment, on-chain proof-of-inclusion       |
| `vesting`              |  ✓   | ✓  |    |   | linear vesting with cliff (wave_timestamp)              |
| `dao-governance`       |  ✓   | ✓  |    |   | FALCON-signed votes + time phases + committed execution |
| `factory`              |  ✓   | ✓  |    |   | deploys child contracts via instantiate                 |
| `standards/token`      | config-only |||| PTS-F fungible token — edit otigen.toml, build generates it |
| `standards/nft`        | config-only |||| PTS-N NFT collection — edit otigen.toml, build generates it |

The Rust and Go source templates are line-by-line equivalent — same logic, same
tests (the `tests/contract.test.toml` vectors are shared and pass in both).

SDK pins: Rust `pyde-host` from crates.io, Go `github.com/pyde-net/pyde-host/go`,
AS `@pyde-net/host` — versions in `manifest.json`.

## Local development

Point `otigen` at a working copy instead of the published repo:

```sh
OTIGEN_TEMPLATES_DIR=/path/to/otigen-templates otigen new my-app --lang go --from fungible-token
```
