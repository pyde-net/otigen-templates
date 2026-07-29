# otigen-templates

Starter templates for [Pyde](https://pyde.network) contracts, organised by
language. `otigen new <name> --lang <lang> --from <template>` clones the
matching directory here and substitutes the project name — templates are hosted
(not baked into the `otigen` binary) so they can be updated without a CLI
release.

## Layout

```
<lang>/<template>/     e.g. go/fungible-token/
manifest.json         catalog: sentinel, SDK pins, template list
```

## Sentinel substitution

Every template is authored against a placeholder name that `otigen new` rewrites
to the user's project name:

| sentinel        | rewritten to        | appears in            |
| --------------- | ------------------- | --------------------- |
| `pyde-template` | `<name>` (kebab)    | `otigen.toml` name    |
| `pyde_template` | `<name>` (snake)    | `go.mod` module path  |

## Go templates

| template               | what it shows                                             |
| ---------------------- | --------------------------------------------------------- |
| `counter`              | minimal contract — one u64 scalar                         |
| `fungible-token`       | balances (map1), allowances (map2), events, owner-gating  |
| `factory`              | `pyde.New(...)` child instantiation + cross-calls         |
| `merkle-claim-airdrop` | off-chain Merkle commitment, on-chain proof-of-inclusion  |
| `upgradeable-proxy`    | delegate-call proxy, state preserved across upgrades      |

Each Go template pins the published SDK (`github.com/pyde-net/pyde-host/go`),
ships its generated `pyde_gen.go`, and builds with `otigen build`.

## Local development

Point `otigen` at a working copy instead of the published repo:

```sh
OTIGEN_TEMPLATES_DIR=/path/to/otigen-templates otigen new my-app --lang go --from fungible-token
```
