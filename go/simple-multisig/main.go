// simple-multisig — 2-of-3-style FALCON-512 multisig. Canonical Pyde example
// of in-contract post-quantum signature verification via pyde.FalconVerify.
//
// Three signers are registered at deploy time. Each is identified on-chain by
// Poseidon2(falcon_pubkey) — a 32-byte hash of the signer's FALCON-512 public
// key. Storing the hash instead of the full pubkey keeps state small; callers
// provide the full pubkey at execute time.
//
// To execute a transfer, anyone may submit one call passing up to three
// (pubkey, signature) pairs and a strictly-increasing nonce. Each pair is
// verified:
//
//   - The pubkey's Poseidon2 hash must equal one of the registered signer IDs
//     (UnknownSigner reverts).
//   - The same signer must not be counted twice (DuplicateSigner).
//   - falcon_verify(pk, action_digest, sig) must return true (BadSignature).
//
// Once the number of valid distinct sigs reaches threshold, the contract pays
// transfer(target, amount) and marks the action_digest as used (anti-replay).
//
// Domain-separated digest binding — the action_digest is computed on-chain as:
//
//	action_digest = Poseidon2(
//	    DOMAIN_TAG ||      // "PYDE-MULTISIG-V1"
//	    self_address ||    // 32 bytes — this contract's address
//	    chain_id ||        // 8 bytes  — little-endian u64
//	    target ||          // 32 bytes — recipient
//	    amount ||          // 16 bytes — little-endian u128
//	    nonce              // 16 bytes — little-endian u128
//	)
//
// binding every signature to (this contract, this chain, this intent).

package main

import pyde "github.com/pyde-net/pyde-host/go"

// nSigners is the fixed number of registered signers. Three keeps the contract
// small + lets us hard-code the per-signer storage layout.
const nSigners uint8 = 3

// domainTag is the domain-separation tag for the FALCON-signed digest. Bumping
// the suffix forces existing signatures to no longer validate, which is the
// intended migration path if the digest scheme changes.
var domainTag = []byte("PYDE-MULTISIG-V1")

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

// pubkeyHash returns Poseidon2(pubkey_bytes) — the on-chain "signer ID". 32
// bytes. Matches the test runner's @pubkey_hash:NAME DSL exactly.
func pubkeyHash(pk []byte) pyde.Bytes32 {
	return pyde.Poseidon2(pk)
}

// pay pushes amount PYDE from this contract's balance to `to`. Reverts the
// whole frame on a non-zero return code.
func pay(to pyde.Address, amount pyde.U128) {
	if !pyde.TryTransfer(to, amount) {
		pyde.Revert("TransferFailed")
	}
}

// falconVerify verifies a FALCON-512 signature via the chain's host fn.
func falconVerify(pk []byte, msg []byte, sig []byte) bool {
	return pyde.FalconVerify(pk, msg, sig)
}

// computeActionDigest recomputes the canonical action digest. See the file
// doc-comment for the encoding. The digest serves two roles simultaneously:
// the FALCON message every signer signs over, and the replay-protection
// storage key.
//
// Pre-allocated length: 16 (DOMAIN) + 32 (self) + 8 (chain) + 32 (target) + 16
// (amount) + 16 (nonce) = 120 bytes. We size the buffer exactly so the contract
// avoids any reallocation on the happy path.
func computeActionDigest(target pyde.Address, amount pyde.U128, nonce pyde.U128) pyde.Bytes32 {
	selfAddr := pyde.Self()
	chainID := pyde.ChainId()
	buf := make([]byte, 0, 120)
	buf = append(buf, domainTag...)
	buf = append(buf, selfAddr[:]...)
	var chainBytes [8]byte
	for i := 0; i < 8; i++ {
		chainBytes[i] = byte(chainID >> (8 * uint(i)))
	}
	buf = append(buf, chainBytes[:]...)
	buf = append(buf, target[:]...)
	amountBytes := amount.ToBytesLE()
	buf = append(buf, amountBytes[:]...)
	nonceBytes := nonce.ToBytesLE()
	buf = append(buf, nonceBytes[:]...)
	return pyde.Poseidon2(buf)
}

// ────────────────────────────────────────────────────────────────
// Entry points
// ────────────────────────────────────────────────────────────────

// Init configures the multisig once at deploy time.
//
// Reverts on duplicate signer hashes — a multisig registered with
// (alice, alice, bob) would silently degrade threshold = 2 to a 1-of-2 (alice
// alone covers two slots). That's not a multisig.
func Init(threshold uint8, signer0 pyde.Bytes32, signer1 pyde.Bytes32, signer2 pyde.Bytes32) {
	if threshold < 1 || threshold > nSigners {
		pyde.Revert("BadThreshold")
	}
	if signer0 == signer1 || signer0 == signer2 || signer1 == signer2 {
		pyde.Revert("DuplicateSignerInInit")
	}
	State.Threshold.Set(threshold)
	State.Signers.Set(0, signer0)
	State.Signers.Set(1, signer1)
	State.Signers.Set(2, signer2)
}

// Execute performs a transfer once threshold valid signatures are presented.
//
// Each signer slot consists of a (pubkey, signature) pair. An empty pubkey
// (len == 0) means "this slot is unused" — the caller may pass any combination
// as long as enough distinct signers verify.
//
// The on-chain action_digest binds every signature to (self_address, chain_id,
// target, amount, nonce).
func Execute(
	target pyde.Address,
	amount pyde.U128,
	nonce pyde.U128,
	pk1 []byte,
	sig1 []byte,
	pk2 []byte,
	sig2 []byte,
	pk3 []byte,
	sig3 []byte,
) {
	if target.IsZero() {
		pyde.Revert("multisig: execute to zero address")
	}

	// Recompute the digest on-chain so signatures are bound to (this contract,
	// this chain, this intent, this nonce). The caller never supplies the
	// digest — there's nothing for them to lie about.
	actionDigest := computeActionDigest(target, amount, nonce)

	if State.Used.Get(actionDigest) {
		pyde.Revert("AlreadyUsed")
	}

	threshold := State.Threshold.Get()
	signers := [3]pyde.Bytes32{
		State.Signers.Get(0),
		State.Signers.Get(1),
		State.Signers.Get(2),
	}

	type slot struct {
		pk  []byte
		sig []byte
	}
	slots := [3]slot{
		{pk1, sig1},
		{pk2, sig2},
		{pk3, sig3},
	}

	// Bitmap of which signer indices have already counted — prevents
	// double-counting the same signer across slots.
	var seenSigners uint8 = 0
	var valid uint8 = 0

	for _, sl := range slots {
		pk := sl.pk
		sig := sl.sig
		if len(pk) == 0 {
			continue
		}
		pkHash := pubkeyHash(pk)
		found := false
		var idx uint8
		for i := 0; i < len(signers); i++ {
			if signers[i] == pkHash {
				idx = uint8(i)
				found = true
				break
			}
		}
		if !found {
			pyde.Revert("UnknownSigner")
		}

		bit := uint8(1) << idx
		if seenSigners&bit != 0 {
			pyde.Revert("DuplicateSigner")
		}
		seenSigners |= bit

		if !falconVerify(pk, actionDigest[:], sig) {
			pyde.Revert("BadSignature")
		}
		valid++
	}

	if valid < threshold {
		pyde.Revert("InsufficientApprovals")
	}

	// Anti-replay: mark BEFORE the transfer so a re-entrant transfer can't
	// double-spend by re-calling execute().
	State.Used.Set(actionDigest, true)

	pay(target, amount)

	Executed{Target: target, Amount: amount, ActionDigest: actionDigest}.Emit()
}

// GetThreshold is a view: threshold required for execution.
func GetThreshold() uint8 {
	return State.Threshold.Get()
}

// IsUsed is a view: is this action digest already executed?
func IsUsed(actionDigest pyde.Bytes32) bool {
	return State.Used.Get(actionDigest)
}

// ActionDigest is a view: the canonical digest the multisig will recompute +
// bind sigs to for execute(target, amount, nonce). Off-chain wallets call this
// before collecting signatures so every signer signs the exact same bytes the
// chain will reject otherwise.
func ActionDigest(target pyde.Address, amount pyde.U128, nonce pyde.U128) pyde.Bytes32 {
	return computeActionDigest(target, amount, nonce)
}

// main is required by TinyGo's wasm target; the chain never calls it — it
// dispatches the //go:wasmexport entry points generated into pyde_gen.go.
func main() {}
