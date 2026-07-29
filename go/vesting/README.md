# vesting (Go)

Linear-with-cliff token vesting — the canonical Pyde time-locked allocation. At
deploy time you `configure(beneficiary, total, start, cliff, duration)` and fund
the contract with at least `total` PYDE; from then on anyone can call `release()`
over time to forward the vested-but-unreleased portion to the beneficiary. The
schedule is evaluated against the wave timestamp: nothing before `start + cliff`,
everything after `start + duration`, and `total * (t - start) / duration`
linearly in between.

```sh
# Compile to build/contract.wasm and regenerate pyde_gen.go
otigen build

# Run the behaviour tests in tests/contract.test.toml
otigen test

# Deploy and drive it on devnet
otigen deploy --args '<beneficiary> 1000000000 1000000000 100 1000'
otigen call <address> fund --value 1000000000
otigen call <address> release
otigen call <address> releasable
```
