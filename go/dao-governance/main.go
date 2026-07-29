// dao-governance — proposal-and-vote DAO.
//
// Members create proposals carrying a (target, function, calldata)
// payload; peers vote yes/no within a deadline; any caller can fire
// ExecuteProposal(id) after voting closes if the proposal passed
// quorum. Execution cross-calls the target with the recorded payload
// and propagates the failure category back to the receipt if anything
// goes wrong.
//
// Lifecycle:
//   1. Init             — register the founding member set + config.
//   2. Propose          — a member records (target, function, calldata)
//                         at the current wave timestamp.
//   3. Vote             — members cast a single yes/no per proposal,
//                         guarded by has_voted[id][voter].
//   4. ExecuteProposal  — after voting_period_secs elapses, anyone fires
//                         the cross-call. Quorum check:
//                         yes >= member_count * quorum_percent / 100.

package main

import (
	"unicode/utf8"

	pyde "github.com/pyde-net/pyde-host/go"
)

// ─────────────────────────────────────────────────────────────────────
// Saturating / checked u64 helpers (Go has no built-in equivalents).
// ─────────────────────────────────────────────────────────────────────

func saturatingAdd(a, b uint64) uint64 {
	s := a + b
	if s < a {
		return ^uint64(0)
	}
	return s
}

func saturatingMul(a, b uint64) uint64 {
	if a == 0 || b == 0 {
		return 0
	}
	p := a * b
	if p/a != b {
		return ^uint64(0)
	}
	return p
}

// ─────────────────────────────────────────────────────────────────────
// Authorization
// ─────────────────────────────────────────────────────────────────────

func requireMember() pyde.Address {
	caller := pyde.Caller()
	if !State.Members.Get(caller) {
		pyde.Revert("dao: caller is not a member")
	}
	return caller
}

// requireSelfCall gates AddMember / RemoveMember to vote-only execution:
// only the DAO itself (via a successful ExecuteProposal that targets
// itself) can mutate the member set. Direct member calls revert,
// blocking the Sybil-attack class where a single compromised key can
// grow the member set unilaterally.
func requireSelfCall() {
	if !pyde.Caller().Equal(pyde.Self()) {
		pyde.Revert("dao: membership changes require a passed proposal")
	}
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

// Init — constructor. Registers the founding member set + writes the
// voting-period and quorum-percent config. The contract itself is fully
// member-driven thereafter — there is no admin role.
func Init(initialMembers []pyde.Address, votingPeriodSecs uint64, quorumPercent uint64) {
	if quorumPercent == 0 || quorumPercent > 100 {
		pyde.Revert("dao: quorum_percent must be 1..=100")
	}
	if votingPeriodSecs == 0 {
		pyde.Revert("dao: voting_period_secs must be > 0")
	}
	if len(initialMembers) == 0 {
		pyde.Revert("dao: initial_members must not be empty")
	}
	State.VotingPeriodSecs.Set(votingPeriodSecs)
	State.QuorumPercent.Set(quorumPercent)
	joinTime := pyde.WaveTimestamp()
	var count uint64 = 0
	for _, m := range initialMembers {
		if m.IsZero() {
			pyde.Revert("dao: zero address member")
		}
		if !State.Members.Get(m) {
			State.Members.Set(m, true)
			State.JoinedAt.Set(m, joinTime)
			MemberAdded{Member: m}.Emit()
			next := count + 1
			if next < count {
				pyde.Revert("dao: member count overflow")
			}
			count = next
		}
	}
	State.MemberCount.Set(count)
}

// AddMember — self-call only, invoked via a passed proposal that targets
// the DAO itself with calldata borsh(new_member). A direct member call
// reverts. Records joined_at[new_member] = wave_timestamp so the new
// member can't vote on proposals created before they joined.
func AddMember(newMember pyde.Address) {
	requireSelfCall()
	if newMember.IsZero() {
		pyde.Revert("dao: zero address member")
	}
	if State.Members.Get(newMember) {
		pyde.Revert("dao: already a member")
	}
	State.Members.Set(newMember, true)
	State.JoinedAt.Set(newMember, pyde.WaveTimestamp())
	prior := State.MemberCount.Get()
	next := prior + 1
	if next < prior {
		pyde.Revert("dao: member count overflow")
	}
	State.MemberCount.Set(next)
	MemberAdded{Member: newMember}.Emit()
}

// RemoveMember — self-call only (same gating as AddMember). Used to
// revoke a compromised key or eject a misbehaving member via the
// standard proposal flow. Reverts if member isn't currently in the set.
func RemoveMember(member pyde.Address) {
	requireSelfCall()
	if !State.Members.Get(member) {
		pyde.Revert("dao: not a member")
	}
	State.Members.Set(member, false)
	State.JoinedAt.Set(member, 0)
	prior := State.MemberCount.Get()
	if prior == 0 {
		pyde.Revert("dao: member count underflow")
	}
	State.MemberCount.Set(prior - 1)
	MemberRemoved{Member: member}.Emit()
}

// Propose — create a new proposal carrying a (target, function,
// calldata) payload. Returns the proposal id (1-indexed).
//
// Snapshots member_count at proposal-creation time so the quorum
// denominator is locked in: late joiners after this point can't dilute
// the quorum threshold up-front. The per-member joined_at check in Vote
// then prevents late joiners from casting votes on this proposal at all.
func Propose(target pyde.Address, function string, calldata []byte) uint64 {
	proposer := requireMember()
	if target.IsZero() {
		pyde.Revert("dao: zero address target")
	}
	prior := State.ProposalCount.Get()
	proposalID := prior + 1
	if proposalID < prior {
		pyde.Revert("dao: proposal count overflow")
	}
	State.ProposalCount.Set(proposalID)
	State.ProposalTarget.Set(proposalID, target)
	State.ProposalFunction.Set(proposalID, function)
	State.ProposalCalldata.Set(proposalID, calldata)
	State.ProposalProposer.Set(proposalID, proposer)
	now := pyde.WaveTimestamp()
	State.ProposalCreated.Set(proposalID, now)
	// Snapshot the member count NOW so the quorum denominator is fixed
	// for the lifetime of this proposal — a member joining later can't
	// shift the threshold up.
	State.ProposalMemberCountSnapshot.Set(proposalID, State.MemberCount.Get())
	ProposalCreated{ProposalId: proposalID, Proposer: proposer, Target: target}.Emit()
	return proposalID
}

// Vote — cast a vote on a proposal. One per voter per proposal.
//
// Members are only eligible to vote on proposals created at or after
// their joined_at timestamp — a member added AFTER a proposal was
// created cannot vote on it. Prevents the late-joiner-stuffs-yes vector.
func Vote(proposalID uint64, support bool) {
	voter := requireMember()
	if State.ProposalProposer.Get(proposalID).IsZero() {
		pyde.Revert("dao: proposal does not exist")
	}
	if State.ProposalExecuted.Get(proposalID) {
		pyde.Revert("dao: proposal already executed")
	}
	// Voting eligibility: caller must have been a member at or before
	// this proposal was created. Closes the "join AFTER proposal then
	// vote yes" vector.
	created := State.ProposalCreated.Get(proposalID)
	voterJoinedAt := State.JoinedAt.Get(voter)
	if voterJoinedAt > created {
		pyde.Revert("dao: voter joined after proposal was created")
	}
	// Voting window check. Init rejects period=0, so this branch always
	// evaluates the window — but we keep the saturating add anti-overflow
	// guard around created + period for u64-wraparound safety at large
	// timestamps.
	period := State.VotingPeriodSecs.Get()
	// voting_period_secs is seconds; created / now are wave_timestamp
	// milliseconds — convert the period before comparing.
	deadline := saturatingAdd(created, saturatingMul(period, 1000))
	now := pyde.WaveTimestamp()
	if now > deadline {
		pyde.Revert("dao: voting window closed")
	}
	if State.HasVoted.Get(proposalID, voter) {
		pyde.Revert("dao: already voted on this proposal")
	}
	State.HasVoted.Set(proposalID, voter, true)
	if support {
		prior := State.ProposalYes.Get(proposalID)
		next := prior + 1
		if next < prior {
			pyde.Revert("dao: vote count overflow")
		}
		State.ProposalYes.Set(proposalID, next)
	} else {
		prior := State.ProposalNo.Get(proposalID)
		next := prior + 1
		if next < prior {
			pyde.Revert("dao: vote count overflow")
		}
		State.ProposalNo.Set(proposalID, next)
	}
	Voted{ProposalId: proposalID, Voter: voter, Support: support}.Emit()
}

// ExecuteProposal — execute a passed proposal. Anyone can call.
// Cross-calls the recorded target with the recorded calldata. Returns
// true on successful cross-call, surfaces the failure category as a
// revert message otherwise.
//
// Quorum denominator uses the snapshot recorded at proposal creation,
// NOT the current member count — so adds/removes between propose and
// execute can't shift the threshold under the votes already cast.
func ExecuteProposal(proposalID uint64) bool {
	if State.ProposalProposer.Get(proposalID).IsZero() {
		pyde.Revert("dao: proposal does not exist")
	}
	if State.ProposalExecuted.Get(proposalID) {
		pyde.Revert("dao: proposal already executed")
	}
	// Voting window must have closed. Init rejects period=0 so this
	// branch always runs; the saturating add guards against u64
	// wraparound at huge timestamps.
	created := State.ProposalCreated.Get(proposalID)
	period := State.VotingPeriodSecs.Get()
	// voting_period_secs is seconds; created / now are wave_timestamp
	// milliseconds — convert the period before comparing.
	deadline := saturatingAdd(created, saturatingMul(period, 1000))
	now := pyde.WaveTimestamp()
	if now <= deadline {
		pyde.Revert("dao: voting window not yet closed")
	}
	// Quorum check on yes-vote count. Use the snapshot taken at proposal
	// creation as the denominator — fresh members joining between propose
	// and execute don't shift the threshold.
	yes := State.ProposalYes.Get(proposalID)
	total := State.ProposalMemberCountSnapshot.Get(proposalID)
	quorumPercent := State.QuorumPercent.Get()
	// Use saturating_mul to avoid overflow on tiny DAOs with large member
	// counts. The threshold is the ceiling of total * quorum_percent / 100.
	required := saturatingAdd(saturatingMul(total, quorumPercent), 99) / 100
	if yes < required {
		pyde.Revert("dao: quorum not reached")
	}
	// Mark executed BEFORE the cross-call — reentrancy-friendly.
	State.ProposalExecuted.Set(proposalID, true)

	target := State.ProposalTarget.Get(proposalID)
	function := State.ProposalFunction.Get(proposalID)
	calldata := State.ProposalCalldata.Get(proposalID)

	// Cross-call into the proposal's target. The DAO doesn't care about
	// the target's return type, only success/failure shape — a zero-byte
	// return decodes cleanly; if the target returns non-empty data we
	// ignore it.
	//
	// We do NOT emit ProposalExecuted{Success: false} in the error arms
	// below: those branches all revert, which rolls back every state
	// change (including the events) in this frame.
	// Cap the return buffer at 4 KB to match Rust's DEFAULT_RETURN_BUFFER_BYTES,
	// so a target returning more than that fails identically in both languages
	// (the DAO ignores the return value; only the success/revert boundary matters).
	out, rc := pyde.Call(target, function).Args(calldata).ReturnCap(4096).Exec()
	var success bool
	switch rc {
	case pyde.StatusOK:
		// Callee executed cleanly. Mirrors Rust's Ok(()) plus the
		// DecodeError arm (a non-() return still counts as success).
		success = true
	case pyde.ErrInvalidFunctionName:
		pyde.Revert("dao: target doesn't expose that function")
		return false
	case pyde.ErrInsufficientBalance, pyde.ErrReentrancyBlocked, pyde.ErrValueTransferNotPayable:
		// Rust's Err(_) catch-all over the non-Reverted CallError variants.
		pyde.Revert("dao: target call failed")
		return false
	default:
		// CallError::Reverted arm (chain status -10 and any other code).
		// The Go SDK (alpha.12) returns the callee's revert payload from
		// Exec — mirror Rust: surface the target's utf8 revert message,
		// falling back to "dao: target reverted" if it isn't valid utf8.
		if utf8.Valid(out) {
			pyde.Revert(string(out))
		}
		pyde.Revert("dao: target reverted")
		return false
	}

	ProposalExecuted{ProposalId: proposalID, Success: success}.Emit()
	return success
}

// ─────────────────────────────────────────────────────────────────────
// Views
// ─────────────────────────────────────────────────────────────────────

func IsMember(addr pyde.Address) bool {
	return State.Members.Get(addr)
}

func MemberCount() uint64 {
	return State.MemberCount.Get()
}

func ProposalCount() uint64 {
	return State.ProposalCount.Get()
}

func GetProposalYes(proposalID uint64) uint64 {
	return State.ProposalYes.Get(proposalID)
}

func GetProposalNo(proposalID uint64) uint64 {
	return State.ProposalNo.Get(proposalID)
}

func GetProposalExecuted(proposalID uint64) bool {
	return State.ProposalExecuted.Get(proposalID)
}

func HasVotedOn(proposalID uint64, voter pyde.Address) bool {
	return State.HasVoted.Get(proposalID, voter)
}

func VotingPeriodSecs() uint64 {
	return State.VotingPeriodSecs.Get()
}

func main() {}
