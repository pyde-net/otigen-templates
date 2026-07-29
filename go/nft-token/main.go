// nft-token — the PTS-N reference implementation (pts-n/1, PIP-0005 §12),
// default configuration.
//
// Same philosophy as the fungible reference, per-id state:
//
//   - Revert-only mutations with canonical token:* codes (mint returns the
//     fresh id — a creation, not a status).
//   - Settle-then-notify transfer_call: ownership fully moved and Transfer
//     emitted before the recipient is notified; the receiver must return
//     ACK_NFT or everything unwinds — the safeTransferFrom reentrancy genre
//     has no half-updated window to hit, and a fallback cannot silently
//     swallow an NFT.
//   - Consent-visible roles renounced by provable zeroing.
//   - Per-id owner slots: transfers of distinct ids never conflict under
//     parallel execution. next_id is monotonic and separate from the live
//     count, so burned ids never recycle.
//   - On-chain metadata: token_uris values run to 16 KB — real metadata JSON
//     on-chain, no off-chain rot required.
//
// token_id 0 is reserved (mint starts at 1), so a zero-address read on an
// owner slot is unambiguous "nonexistent". Storage writes zero rather than
// deleting — the chain has no gas refunds.

package main

import pyde "github.com/pyde-net/pyde-host/go"

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

// The surface this contract conforms to.
const standardName = "pts-n/1"

// Max bytes of data forwarded by transfer_call.
const maxCallData = 4096

// Required return of a conformant receiver:
// u32::from_le_bytes(Blake3("pts/on_nft_received/1")[0..4]).
const ackNFT uint32 = 1452691067 // LE bytes 7b 4e 96 56

// 32-byte role identifiers carried by RoleTransfer (PIP-0005 §3):
// Blake3("minter") / Blake3("manager").
var roleMinter = pyde.Bytes32{
	0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
	0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
	0xe5, 0xdd,
}
var roleManager = pyde.Bytes32{
	0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
	0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
	0x35, 0xc1,
}

// "No owner" / "cleared approval" sentinel.
var zeroAddress = pyde.Address{}

// ─── Constructor ───────────────────────────────────────────────────────

// Init records metadata + roles to the deployer. max_supply = 0 = uncapped.
func Init(name string, symbol string, maxSupply uint64) {
	deployer := pyde.Caller()
	State.TokenName.Set(name)
	State.TokenSymbol.Set(symbol)
	State.MaxSupply.Set(maxSupply)
	State.Minter.Set(deployer)
	State.Manager.Set(deployer)
	RoleTransfer{
		Role:     roleMinter,
		Holder:   deployer,
		Previous: zeroAddress,
	}.Emit()
	RoleTransfer{
		Role:     roleManager,
		Holder:   deployer,
		Previous: zeroAddress,
	}.Emit()
}

// ─── Views ─────────────────────────────────────────────────────────────

func Standard() string {
	return standardName
}

func Name() string {
	return State.TokenName.Get()
}

func Symbol() string {
	return State.TokenSymbol.Get()
}

// TotalSupply is the live count (decremented by burn); ids themselves are
// monotonic.
func TotalSupply() uint64 {
	return State.TotalSupply.Get()
}

// MaxSupply — 0 = uncapped.
func MaxSupply() uint64 {
	return State.MaxSupply.Get()
}

func OwnerOf(tokenId uint64) pyde.Address {
	owner := State.Owners.Get(tokenId)
	if owner.IsZero() {
		pyde.Revert("token:nonexistent")
		return pyde.Address{}
	}
	return owner
}

// BalanceOf is the count owned by owner. Consistent with pts-f: a
// never-holder reads as 0, no special-casing.
func BalanceOf(owner pyde.Address) uint64 {
	return State.Balances.Get(owner)
}

func GetApproved(tokenId uint64) pyde.Address {
	requireExists(tokenId)
	return State.TokenApprovals.Get(tokenId)
}

func IsApprovedForAll(owner pyde.Address, operator pyde.Address) bool {
	return State.OperatorApprovals.Get(owner, operator)
}

func TokenUri(tokenId uint64) string {
	requireExists(tokenId)
	return State.TokenUris.Get(tokenId)
}

func Minter() pyde.Address {
	return State.Minter.Get()
}

func Manager() pyde.Address {
	return State.Manager.Get()
}

// ─── Transfers ─────────────────────────────────────────────────────────

// TransferFrom moves token_id from from to to. Caller must be the owner, the
// per-id approved address, or an approved operator. NEVER invokes recipient
// code.
func TransferFrom(from pyde.Address, to pyde.Address, tokenId uint64) {
	caller := pyde.Caller()
	moveToken(caller, from, to, tokenId)
	Transfer{From: from, To: to, TokenId: tokenId}.Emit()
}

// TransferCall is settle-then-notify (PIP-0005 §12): ownership fully moved and
// Transfer emitted FIRST, then the recipient's
// on_nft_received(operator, from, id, data) must return ACK_NFT — a revert, a
// wrong value, or a fallback-swallowed name-miss unwinds the entire operation
// (token:bad_receiver).
func TransferCall(to pyde.Address, tokenId uint64, data []byte) uint32 {
	if len(data) > maxCallData {
		pyde.Revert("token:data_too_large")
		return 0
	}
	operator := pyde.Caller()
	from := State.Owners.Get(tokenId)

	// (1) settle — the caller must be authorized for the id
	moveToken(operator, from, to, tokenId)
	// (2) emit — discarded with the frame if the notify leg reverts
	Transfer{From: from, To: to, TokenId: tokenId}.Emit()

	// (3) notify. Args filled by the token — never spoofable.
	calldata := pyde.NewEncoder().
		Address(operator).
		Address(from).
		U64(tokenId).
		Bytes(data).
		Finish()
	out, rc := pyde.Call(to, "on_nft_received").Args(calldata).Exec()
	if rc == pyde.StatusOK && len(out) >= 4 {
		if pyde.NewDecoder(out).U32() == ackNFT {
			return ackNFT
		}
	}
	pyde.Revert("token:bad_receiver")
	return 0
}

// ─── Approvals ─────────────────────────────────────────────────────────

// Approve is a per-id approval; the zero address clears it. Caller must be the
// owner or an approved operator.
func Approve(to pyde.Address, tokenId uint64) {
	owner := State.Owners.Get(tokenId)
	if owner.IsZero() {
		pyde.Revert("token:nonexistent")
	}
	caller := pyde.Caller()
	if !caller.Equal(owner) && !State.OperatorApprovals.Get(owner, caller) {
		pyde.Revert("token:not_authorized")
	}
	State.TokenApprovals.Set(tokenId, to)
	Approval{
		Owner:    owner,
		Approved: to,
		TokenId:  tokenId,
	}.Emit()
}

// SetApprovalForAll is a collection-wide operator grant for the caller's
// tokens.
func SetApprovalForAll(operator pyde.Address, approved bool) {
	owner := pyde.Caller()
	if operator.IsZero() || operator.Equal(owner) {
		pyde.Revert("token:invalid_operator")
	}
	State.OperatorApprovals.Set(owner, operator, approved)
	ApprovalForAll{
		Owner:    owner,
		Operator: operator,
		Approved: approved,
	}.Emit()
}

// ─── Supply & roles ────────────────────────────────────────────────────

// Mint is minter only. Assigns the next monotonic id (starting at 1; never
// recycled), stores the metadata on-chain, emits the zero-sentinel Transfer.
// Returns the fresh id.
func Mint(to pyde.Address, uri string) uint64 {
	caller := pyde.Caller()
	if !caller.Equal(State.Minter.Get()) || caller.IsZero() {
		pyde.Revert("token:not_minter")
	}
	guardRecipient(to)

	supply := State.TotalSupply.Get()
	cap := State.MaxSupply.Get()
	if cap > 0 && supply >= cap {
		pyde.Revert("token:cap_exceeded")
	}

	nextId := State.NextId.Get()
	if nextId == ^uint64(0) {
		pyde.Revert("token:overflow")
		return 0
	}
	tokenId := nextId + 1
	State.NextId.Set(tokenId)
	State.TotalSupply.Set(supply + 1)
	State.Owners.Set(tokenId, to)
	bal := State.Balances.Get(to)
	State.Balances.Set(to, bal+1)
	State.TokenUris.Set(tokenId, uri)

	Transfer{
		From:    zeroAddress,
		To:      to,
		TokenId: tokenId,
	}.Emit()
	return tokenId
}

// Burn — owner or authorized. The id is retired forever: ownership zeroed
// (never deleted), live count decremented, monotonic next_id untouched.
func Burn(tokenId uint64) {
	caller := pyde.Caller()
	owner := State.Owners.Get(tokenId)
	if owner.IsZero() {
		pyde.Revert("token:nonexistent")
	}
	if !isAuthorized(owner, caller, tokenId) {
		pyde.Revert("token:not_authorized")
	}
	State.Owners.Set(tokenId, zeroAddress)
	State.TokenApprovals.Set(tokenId, zeroAddress)
	bal := State.Balances.Get(owner)
	if bal > 0 {
		bal--
	}
	State.Balances.Set(owner, bal)
	supply := State.TotalSupply.Get()
	if supply > 0 {
		supply--
	}
	State.TotalSupply.Set(supply)
	Transfer{
		From:    owner,
		To:      zeroAddress,
		TokenId: tokenId,
	}.Emit()
}

// SetMinter is manager only. Zero address = provable renounce.
func SetMinter(holder pyde.Address) {
	caller := pyde.Caller()
	if !caller.Equal(State.Manager.Get()) || caller.IsZero() {
		pyde.Revert("token:not_manager")
	}
	previous := State.Minter.Get()
	State.Minter.Set(holder)
	RoleTransfer{
		Role:     roleMinter,
		Holder:   holder,
		Previous: previous,
	}.Emit()
}

// SetManager is manager only. Renouncing freezes role governance permanently.
func SetManager(holder pyde.Address) {
	caller := pyde.Caller()
	if !caller.Equal(State.Manager.Get()) || caller.IsZero() {
		pyde.Revert("token:not_manager")
	}
	previous := State.Manager.Get()
	State.Manager.Set(holder)
	RoleTransfer{
		Role:     roleManager,
		Holder:   holder,
		Previous: previous,
	}.Emit()
}

// ─── Internal helpers ──────────────────────────────────────────────────

func requireExists(tokenId uint64) {
	if State.Owners.Get(tokenId).IsZero() {
		pyde.Revert("token:nonexistent")
	}
}

// guardRecipient carries the credit-path guards shared by every credit path:
// the zero address and the token's own contract address.
func guardRecipient(to pyde.Address) {
	if to.IsZero() || to.Equal(pyde.Self()) {
		pyde.Revert("token:invalid_recipient")
	}
}

// isAuthorized reports whether spender may move token_id: owner, per-id
// approved, or an approved operator.
func isAuthorized(owner pyde.Address, spender pyde.Address, tokenId uint64) bool {
	if spender.Equal(owner) {
		return true
	}
	approved := State.TokenApprovals.Get(tokenId)
	if approved.Equal(spender) && !approved.IsZero() {
		return true
	}
	return State.OperatorApprovals.Get(owner, spender)
}

// moveToken is the ownership move with the full guard set. Clears the per-id
// approval atomically (same slot family — no extra conflict surface).
func moveToken(caller pyde.Address, from pyde.Address, to pyde.Address, tokenId uint64) {
	owner := State.Owners.Get(tokenId)
	if owner.IsZero() {
		pyde.Revert("token:nonexistent")
	}
	if !owner.Equal(from) {
		pyde.Revert("token:not_owner")
	}
	guardRecipient(to)
	if !isAuthorized(owner, caller, tokenId) {
		pyde.Revert("token:not_authorized")
	}
	State.TokenApprovals.Set(tokenId, zeroAddress)
	fromBal := State.Balances.Get(from)
	if fromBal > 0 {
		fromBal--
	}
	State.Balances.Set(from, fromBal)
	toBal := State.Balances.Get(to)
	if toBal == ^uint64(0) {
		pyde.Revert("token:overflow")
		return
	}
	State.Balances.Set(to, toBal+1)
	State.Owners.Set(tokenId, to)
}

// main is required by TinyGo's wasm target; the chain never calls it.
func main() {}
