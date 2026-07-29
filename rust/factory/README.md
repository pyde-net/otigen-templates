# factory-rust — a contract that deploys contracts

A **factory** mints fresh instances of a *template* at runtime with
`pyde::instantiate`. The template is **any contract you have already
deployed** — the factory takes its address as an argument, instantiates
it with **no constructor arguments**, and drives it by cross-call. There
is no bundled template: bring your own.

**What a template needs:** no required constructor (so it can be
instantiated with empty init calldata), and — to be driven by `bump` — an
`increment() -> u64`. The built-in `counter` template
(`otigen new mycounter --from counter`) fits exactly.

## Try it on a devnet

```bash
otigen devnet            # in one terminal

# 1. Deploy a template — any no-constructor contract with increment().
otigen new mycounter --from counter
cd mycounter && otigen deploy --network devnet     # note the deployed ADDRESS
cd ..

# 2. Deploy this factory.
otigen deploy --network devnet

# 3. Mint children — three ways to choose the salt:
otigen call factory-rust create           <counter-address>              --network devnet   # AUTO salt
otigen call factory-rust create_with_key  <counter-address> 5            --network devnet   # u64 salt
otigen call factory-rust create_named     <counter-address> "eth/usdc"   --network devnet   # string salt

# 4. Drive a child — three ways to reach it:
otigen call factory-rust bump       0             --network devnet   # by u64 key
otigen call factory-rust bump_named "eth/usdc"    --network devnet   # by string name
otigen call factory-rust bump_at    <child-address> --network devnet # by raw address
```

`create` returns the child's address (and emits an `Instantiated`
event). `child_of(key)` / `child_of_name(name)` look a child up;
`next_key()` / `created()` report the factory's state.

## Why it works

A child's address is `Poseidon2("pyde-child:" ‖ factory ‖ template ‖
salt)` — a pure function of the factory, the template, and the salt. So
the same salt always maps to the same child (a repeat reverts `exists`),
and any child's address is predictable off-chain before it is minted.
Children share the template's cached code but each gets its own address
and its own isolated storage.

## Structure

- `src/lib.rs` — the factory: `create` / `create_with_key` /
  `create_named`, `child_of` / `child_of_name` / `next_key` / `created`,
  and `bump` / `bump_named` / `bump_at`.
- `otigen.toml` — state schema (a u64 registry + a string registry + the
  auto-salt counter) and the function surface.
- `tests/contract.test.toml` — the factory's self-contained read paths.
  The live create → bump flow against a real template is the walk-through
  above.
