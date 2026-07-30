# factory (C)

A factory in C: mints child contracts (auto / u64 / string salts) and drives
them by key, name, or address — via the `instantiate` and `cross_call` host
fns. You write only typed inner functions; `otigen build` generates the rest.

> Rust authors get the macro substrate (`#[pyde::entry]`, `declare_storage!`,
> `pyde::prepare(...).instantiate()`); C gets the same ergonomics through
> **generated code**. From `otigen.toml`, `otigen build` writes `pyde_gen.h`
> with the borsh runtime, typed storage accessors, and the `() -> ()`
> entry-dispatch shims. Your `main.c` includes it and defines the
> `__<fn>_impl` bodies — the factory `instantiate` and child `cross_call` are
> raw host fns (declared in `include/pyde/host.h`), hand-marshalled there.

For depth (full ABI, error codes, gas table), see <https://book.pyde.network>.

---

## What a factory is

The template is ANY contract you have already deployed — there is no dedicated
"template" contract to ship. Deploy a contract (the built-in `counter`
template is a perfect fit — it has an `increment() -> u64` and no required
constructor), copy its address, and pass it to `create`. Each child is a
first-class contract with its own address and isolated storage, sharing the
template's already-cached code (nothing is copied or recompiled).

A child's address is a pure function of `(factory, template, salt)`, so it is
predictable off-chain before the child exists. This template shows the THREE
ways to choose the salt and the THREE ways to reach a child:

| create           | salt                              |
| ---------------- | --------------------------------- |
| `create`         | AUTO — the `next_key` counter     |
| `create_with_key`| an explicit `uint64` (user id …)  |
| `create_named`   | a `string` (a name, a pair …)     |

| interact    | reach a child                     |
| ----------- | --------------------------------- |
| `bump`      | BY u64 KEY — look up, then call   |
| `bump_named`| BY STRING NAME — look up, call    |
| `bump_at`   | BY ADDRESS — call directly        |

---

## Project layout

```
factory/
├── Makefile               # build / test / deploy / inspect / verify
├── otigen.toml            # metadata + [state] + [functions]
├── main.c                 # YOUR CONTRACT CODE — the __<fn>_impl bodies
├── pyde_gen.h             # GENERATED (committed) — regenerated each build
├── include/pyde/host.h    # the raw pyde::* host-fn declarations
└── tests/
    └── contract.test.toml # behaviour tests (Foundry-shape TOML)
```

`pyde_gen.h` is generated from `otigen.toml` on every `otigen build` and
committed like a `.pb.go` — never edit it by hand; edit the manifest or your
`__<fn>_impl` bodies.

---

## Prerequisites

C needs a clang with the **wasm32** backend AND **wasm-ld**. Apple's bundled
clang lacks both. Install LLVM + lld via Homebrew:

```bash
brew install llvm lld
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"
```

On Linux: `apt install clang lld` ships a wasm32-capable clang + wasm-ld out
of the box. `make check-tools` verifies it.

---

## Quick start

```bash
make build       # otigen build → generates pyde_gen.h, clang → ./artifacts/factory.bundle/
make test        # otigen test (runs tests/contract.test.toml)
make test-vvvv   # otigen test -vvvv (gas + events + traces + storage diffs)
make deploy      # otigen deploy --network devnet
make help        # list all targets
```

---

## Live walk-through: create → bump

The bundled tests cover the factory's self-contained read paths (a fresh
factory has minted nothing; unknown keys/names map to the zero address). The
full `create → bump` flow needs a deployed TEMPLATE — any contract with an
`increment() -> u64` and no required constructor (the built-in `counter`
template fits) — so it's a live, two-contract flow:

```bash
# 1. Deploy a template (e.g. the counter example) and copy its address.
otigen deploy --network devnet          # → 0xTEMPLATE...

# 2. Deploy this factory.
otigen deploy --network devnet          # → 0xFACTORY...

# 3. Mint a child at the auto salt, then drive it.
otigen call 0xFACTORY create   0xTEMPLATE --network devnet   # → 0xCHILD (key 0)
otigen call 0xFACTORY bump     0          --network devnet   # → 1
otigen call 0xFACTORY child_of 0          --network devnet   # → 0xCHILD
```

The child's address is `child_address(factory, template, salt)` and is
predictable off-chain before it exists.

---

## The `instantiate` seam

`create*` calls the raw `instantiate` host fn (HOST_FN_ABI_SPEC §7.12): the
salt is `Poseidon2(borsh(key))` (a u64's 8 LE bytes) or `Poseidon2(borsh(name))`
(a string's u32 length prefix + UTF-8), no constructor args, no endowment, all
remaining gas forwarded (a NEGATIVE gas limit means forward-all, unlike
`cross_call`). The status codes map to reverts identically to the Rust and Go
templates: `-44` → `exists`, `-43` → `template-not-found`, and any other
failure bubbles the child constructor's revert message (or `instantiate-failed`).

`bump*` calls the raw `cross_call` host fn into the child's `increment`,
forwarding the callee's revert message on failure (or `child-call-failed`).

Full ABI: <https://book.pyde.network/companion/HOST_FN_ABI_SPEC>.

---

## License

Pick one and edit this section. Pyde itself has no preference.
