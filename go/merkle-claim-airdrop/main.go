package main

import pyde "github.com/pyde-net/pyde-host/go"

// A Merkle-tree airdrop: an off-chain process precomputes a tree whose leaves
// are Blake3("PYDE_LEAF" ‖ claimant ‖ amount_be), the admin commits the 32-byte
// root once, and claimants then prove inclusion with their sibling path to pull
// their allocation from the contract's balance. The whole allocation costs ONE
// 32-byte root on-chain; verification is O(log n) per claim.
//
// Tree encoding (match this off-chain when you build the tree):
//   Leaf:  Blake3("PYDE_LEAF" ‖ claimant_addr_32 ‖ amount_u128_be_16)
//   Node:  Blake3("PYDE_NODE" ‖ left_hash_32 ‖ right_hash_32)
//   Proof: [position_byte ‖ sibling_hash_32] repeated per level.
//          position 0 ⇒ current hash is the LEFT child (sibling on the right);
//          position 1 ⇒ current hash is the RIGHT child (sibling on the left).
//   Amount is BIG-ENDIAN u128 (16 bytes) — the standard Solidity-style
//   airdrop encoding, so community tree generators interoperate.

// Domain-separation tags keep a leaf hash from ever colliding with an internal
// node hash (which would let a forged "claim" verify at the wrong level).
var (
	leafTag = []byte("PYDE_LEAF")
	nodeTag = []byte("PYDE_NODE")
)

// maxProofBytes caps a proof at 32 levels × 33 bytes — trees up to 2^32 leaves,
// far beyond any realistic airdrop.
const maxProofBytes = 33 * 32

// ── merkle hashing ──────────────────────────────────────────────────

// amountBE encodes a u128 as 16 big-endian bytes (most-significant first).
func amountBE(a pyde.U128) [16]byte {
	var b [16]byte
	for i := 0; i < 8; i++ {
		b[7-i] = byte(a.Hi >> (8 * uint(i)))
		b[15-i] = byte(a.Lo >> (8 * uint(i)))
	}
	return b
}

// leafHash computes a leaf from (claimant, amount).
func leafHash(claimant pyde.Address, amount pyde.U128) pyde.Bytes32 {
	be := amountBE(amount)
	buf := make([]byte, 0, len(leafTag)+32+16)
	buf = append(buf, leafTag...)
	buf = append(buf, claimant[:]...)
	buf = append(buf, be[:]...)
	return pyde.Blake3(buf)
}

// nodeHash computes an internal node from its two children.
func nodeHash(left, right pyde.Bytes32) pyde.Bytes32 {
	buf := make([]byte, 0, len(nodeTag)+64)
	buf = append(buf, nodeTag...)
	buf = append(buf, left[:]...)
	buf = append(buf, right[:]...)
	return pyde.Blake3(buf)
}

// walkProof folds `leaf` upward through each proof step, returning the root the
// proof implies. `proof` length is validated as a multiple of 33 by the caller.
func walkProof(leaf pyde.Bytes32, proof []byte) pyde.Bytes32 {
	hash := leaf
	for i := 0; i < len(proof); i += 33 {
		var sibling pyde.Bytes32
		copy(sibling[:], proof[i+1:i+33])
		if proof[i] == 0 {
			hash = nodeHash(hash, sibling) // current is left
		} else {
			hash = nodeHash(sibling, hash) // current is right
		}
	}
	return hash
}

// pay pushes `amount` PYDE from this contract to `to`, reverting the whole
// frame (and rolling back the claim flag) if the contract is underfunded.
func pay(to pyde.Address, amount pyde.U128) {
	if !pyde.TryTransfer(to, amount) {
		pyde.Revert("merkle: TransferFailed")
	}
}

// ── entry points ────────────────────────────────────────────────────

// Init runs once at deploy (the [constructor]): records the caller as admin,
// the only address allowed to commit the root.
func Init() {
	if !State.Admin.Get().IsZero() {
		pyde.Revert("merkle: already initialized")
	}
	State.Admin.Set(pyde.Caller())
}

// Fund accepts PYDE into the contract's balance (a payable entry) and emits a
// Funded event for off-chain accounting. Refuses zero-value calls.
func Fund() {
	amount := pyde.TxValue()
	if amount.IsZero() {
		pyde.Revert("merkle: fund requires non-zero value")
	}
	Funded{From: pyde.Caller(), Amount: amount}.Emit()
}

// SetRoot commits the merkle root. Admin-only, and one-shot: once set, the root
// is locked.
func SetRoot(root pyde.Bytes32) {
	admin := State.Admin.Get()
	if admin.IsZero() {
		pyde.Revert("merkle: not initialized")
	}
	if !pyde.Caller().Equal(admin) {
		pyde.Revert("merkle: caller is not admin")
	}
	if State.RootSet.Get() {
		pyde.Revert("RootAlreadySet")
	}
	State.MerkleRoot.Set(root)
	State.RootSet.Set(true)
}

// Claim proves (caller, amount) ∈ tree by recomputing the root from the leaf
// plus the supplied path, then transfers `amount` PYDE to the claimant.
// Checks-effects-interactions: state + event precede the transfer.
func Claim(amount pyde.U128, proof []byte) {
	if !State.RootSet.Get() {
		pyde.Revert("RootNotSet")
	}
	claimant := pyde.Caller()
	if State.Claimed.Get(claimant) {
		pyde.Revert("AlreadyClaimed")
	}
	// Each step is 1 position byte + 32 sibling bytes. Empty proofs verify a
	// degenerate single-leaf tree directly.
	if len(proof)%33 != 0 {
		pyde.Revert("MalformedProof")
	}
	if len(proof) > maxProofBytes {
		pyde.Revert("ProofTooLong")
	}
	if !walkProof(leafHash(claimant, amount), proof).Equal(State.MerkleRoot.Get()) {
		pyde.Revert("InvalidProof")
	}

	State.Claimed.Set(claimant, true)
	newTotal, ok := State.TotalClaimed.Get().AddChecked(amount)
	if !ok {
		pyde.Revert("merkle: total claimed overflow")
	}
	State.TotalClaimed.Set(newTotal)
	// The generated event struct is ClaimEvent (suffixed to avoid colliding
	// with the Claim entry function — Go has no events:: namespace).
	ClaimEvent{Claimant: claimant, Amount: amount}.Emit()
	pay(claimant, amount)
}

// IsClaimed reports whether `addr` has already claimed.
func IsClaimed(addr pyde.Address) bool { return State.Claimed.Get(addr) }

// TotalClaimed is the cumulative amount claimed.
func TotalClaimed() pyde.U128 { return State.TotalClaimed.Get() }

// MerkleRootFirstByte returns -1 if the root is unset, else its first byte — a
// cheap probe for confirming set_root landed.
func MerkleRootFirstByte() int64 {
	if !State.RootSet.Get() {
		return -1
	}
	root := State.MerkleRoot.Get()
	return int64(root[0])
}

// main is required by TinyGo's wasm target; the chain dispatches the generated
// //go:wasmexport entry points, never main.
func main() {}
