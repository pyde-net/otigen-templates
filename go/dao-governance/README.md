# dao-governance (Go)

A minimal proposal-and-vote DAO. Members create proposals that carry a
`(target, function, calldata)` payload, peers vote yes/no within a
deadline, and once voting closes any caller can fire `execute_proposal`
to cross-call the target with the recorded payload — provided the yes
votes cleared the quorum threshold snapshotted at proposal creation.
Membership itself is fully member-driven: `add_member` / `remove_member`
are self-call only, reachable exclusively through a passed proposal that
targets the DAO, so no single key can grow or prune the member set.

```sh
# Build the contract to build/contract.wasm (also generates pyde_gen.go).
otigen build

# Run the behaviour test vectors.
otigen test

# Deploy to a running devnet, then drive it.
otigen deploy
otigen call <address> is_member <member-address>
otigen call <address> proposal_count
```
