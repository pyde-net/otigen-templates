# pyde-template — Go factory

A factory that mints child contracts via `pyde.New(template)` and drives them
with cross-calls. A **template** is any contract you've already deployed; each
child is a first-class contract (its own address and storage) that shares the
template's cached code — nothing is copied or recompiled.

```sh
otigen build          # generate + compile
otigen test           # self-contained read-path tests
```

## Live create → bump walk-through (devnet)

The full flow needs a deployed template with `increment() -> uint64` and no
required constructor — the built-in **counter** template fits.

```sh
otigen devnet --rpc-listen 127.0.0.1:9933 &

# 1. Deploy a template (the counter) and copy its address.
otigen new my-counter --lang go --from counter && cd my-counter
otigen build && otigen deploy --from devnet-0 --password-stdin <<< devnet
#   → TEMPLATE=<address>

# 2. Deploy this factory.
cd .. && otigen build && otigen deploy --from devnet-0 --password-stdin <<< devnet
#   → FACTORY=<address>

# 3. Mint a child (AUTO salt), then drive it.
otigen call $FACTORY create $TEMPLATE  --from devnet-0 --password-stdin <<< devnet
otigen call $FACTORY bump 0            --from devnet-0 --password-stdin <<< devnet   # → 1
otigen call $FACTORY child_of 0        # → the child's address
```

Salt choices: `create` (auto counter), `create_with_key <u64>`, `create_named
<string>`. Reach a child: `bump <key>`, `bump_named <name>`, `bump_at <address>`.
A child's address is a pure function of `(factory, template, salt)`, so a repeat
create with the same salt reverts `exists`.

`pyde_gen.go` is generated — regenerated on every `otigen build`. Don't edit it.
