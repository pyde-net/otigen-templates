# fungible-token-config — config-only pts-f/1 token

The **zero-source** counterpart to [`../fungible-token`](../fungible-token).
The author writes only `otigen.toml` (`type = "token"`, `standard = "pts-f/1"`,
a `[token]` block) — **no `src/`**. `otigen build` generates the full pts-f/1
contract, compiles it, and bundles it.

```
otigen build          # generate → compile → ./artifacts/fungible-token-config.bundle
./run_e2e.sh          # build + assert the generated ABI == the reference surface
```

## What it proves

The generated ABI surface is **byte-identical to the hand-written
`fungible-token` reference except the `init` constructor** — config-only bakes
the init arguments (name/symbol/decimals/supply from `[token]`) into an
argument-free constructor, while the reference takes them as init args. Every
other function, all events, the state schema, and the custom types are
identical. This is the config→source drift guarantee that makes token
verification a rebuild-and-byte-compare (PIP-0005 §13).

Everything except this manifest is generated and git-ignored; regenerate with
`./run_e2e.sh`.
