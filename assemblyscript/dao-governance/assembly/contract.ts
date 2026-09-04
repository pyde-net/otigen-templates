// A proposal-and-vote DAO.
//
// Exercises the cross-call path end to end: members create proposals
// carrying a `(target, function, calldata)` payload; peers vote yes/no
// within a deadline; any caller can fire `execute_proposal(id)` once
// voting closes, if the proposal passed quorum. Execution cross-calls the
// target with the recorded payload and propagates the failure category
// back to the receipt if anything goes wrong.
//
// ## Lifecycle
//
//   1. **init**             — register the founding member set + config.
//   2. **propose**          — a member records (target, function,
//                             calldata) at the current wave timestamp.
//   3. **vote**             — members cast a single yes/no per proposal,
//                             guarded by `has_voted[id][voter]`.
//   4. **execute_proposal** — after `voting_period_secs` elapses, anyone
//                             fires the cross-call. Quorum check:
//                             `yes >= member_count * quorum_percent / 100`.

import {
  Address,
  Call,
  caller,
  selfAddress,
  waveTimestamp,
  equals32,
  newAddress,
  revertStr,
  ERR_INVALID_FUNCTION_NAME,
} from "@pyde-net/host/assembly";
import { storage } from "./generated/pyde.storage.generated";
import { events } from "./generated/pyde.events.generated";

// ─────────────────────────────────────────────────────────────────────
// Saturating u64 arithmetic
// ─────────────────────────────────────────────────────────────────────
//
// Rust's `saturating_*` have no AssemblyScript equivalent — u64 wraps
// silently — so the clamps are explicit here.

function satAdd(a: u64, b: u64): u64 {
  const sum = a + b;
  return sum < a ? u64.MAX_VALUE : sum;
}

function satMul(a: u64, b: u64): u64 {
  if (a == 0 || b == 0) {
    return 0;
  }
  const product = a * b;
  // Division is the cheap inverse check: if the product wrapped, dividing
  // it back will not return the original operand.
  return product / a != b ? u64.MAX_VALUE : product;
}

// ─────────────────────────────────────────────────────────────────────
// Authorization
// ─────────────────────────────────────────────────────────────────────

function requireMember(): Address {
  const who = caller();
  if (!storage.members.read(who)) {
    revertStr("dao: caller is not a member");
  }
  return who;
}

/// Gates `add_member` / `remove_member` to vote-only execution: only the
/// DAO itself, via a successful `execute_proposal` that targets itself,
/// can mutate the member set. Direct member calls revert, which blocks
/// the Sybil class where one compromised key grows the member set
/// unilaterally.
function requireSelfCall(): void {
  if (!equals32(caller(), selfAddress())) {
    revertStr("dao: membership changes require a passed proposal");
  }
}

/// The wave timestamp at which voting closes for a proposal. The stored
/// period is in seconds while wave timestamps are milliseconds, so it is
/// converted before comparison.
function deadlineOf(proposal_id: u64): u64 {
  const created = storage.proposal_created.read(proposal_id);
  return satAdd(created, satMul(storage.voting_period_secs.read(), 1000));
}

/// Revert unless the proposal exists and has not already executed.
function requireOpen(proposal_id: u64): void {
  if (equals32(storage.proposal_proposer.read(proposal_id), newAddress())) {
    revertStr("dao: proposal does not exist");
  }
  if (storage.proposal_executed.read(proposal_id)) {
    revertStr("dao: proposal already executed");
  }
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

// Constructor — registers the founding member set and writes the
// voting-period and quorum-percent config. The contract is fully
// member-driven thereafter; there is no admin role.
//
// Reverts on:
// - empty `initial_members`, which would brick the DAO since no one
//   could ever propose,
// - any zero-address entry in `initial_members`,
// - `quorum_percent` outside 1..=100,
// - `voting_period_secs == 0` — required so propose, vote, and execute
//   cannot all land in the same wave.
@entry
export function init(
  initial_members: Array<Address>,
  voting_period_secs: u64,
  quorum_percent: u64,
): void {
  if (quorum_percent == 0 || quorum_percent > 100) {
    revertStr("dao: quorum_percent must be 1..=100");
  }
  if (voting_period_secs == 0) {
    revertStr("dao: voting_period_secs must be > 0");
  }
  if (initial_members.length == 0) {
    revertStr("dao: initial_members must not be empty");
  }
  storage.voting_period_secs.write(voting_period_secs);
  storage.quorum_percent.write(quorum_percent);

  const joinTime = waveTimestamp();
  const zero = newAddress();
  let count: u64 = 0;
  for (let i = 0, n = initial_members.length; i < n; i++) {
    const m = unchecked(initial_members[i]);
    if (equals32(m, zero)) {
      revertStr("dao: zero address member");
    }
    // Duplicates in the founding set are counted once, so the quorum
    // denominator matches the number of distinct members.
    if (!storage.members.read(m)) {
      storage.members.write(m, true);
      storage.joined_at.write(m, joinTime);
      events.MemberAdded(m);
      const next = count + 1;
      if (next < count) {
        revertStr("dao: member count overflow");
      }
      count = next;
    }
  }
  storage.member_count.write(count);
}

/// Add a new member. Self-call only — invoked via a passed proposal that
/// targets the DAO itself with calldata `borsh(new_member)`. A direct
/// member call reverts.
///
/// Records `joined_at[new_member] = wave_timestamp` so the new member
/// cannot vote on proposals created before they joined.
@entry
export function add_member(new_member: Address): void {
  requireSelfCall();
  if (equals32(new_member, newAddress())) {
    revertStr("dao: zero address member");
  }
  if (storage.members.read(new_member)) {
    revertStr("dao: already a member");
  }
  storage.members.write(new_member, true);
  storage.joined_at.write(new_member, waveTimestamp());

  const prior = storage.member_count.read();
  const next = prior + 1;
  if (next < prior) {
    revertStr("dao: member count overflow");
  }
  storage.member_count.write(next);
  events.MemberAdded(new_member);
}

/// Remove a member. Self-call only, with the same gating as
/// `add_member`. Used to revoke a compromised key or eject a misbehaving
/// member through the standard proposal flow.
@entry
export function remove_member(member: Address): void {
  requireSelfCall();
  if (!storage.members.read(member)) {
    revertStr("dao: not a member");
  }
  storage.members.write(member, false);
  storage.joined_at.write(member, 0);

  const prior = storage.member_count.read();
  if (prior == 0) {
    revertStr("dao: member count underflow");
  }
  storage.member_count.write(prior - 1);
  events.MemberRemoved(member);
}

/// Create a proposal carrying a `(target, function, calldata)` payload.
/// Returns the proposal id, 1-indexed.
///
/// Snapshots `member_count` at creation time and stores it per-proposal,
/// so the quorum denominator is locked in and late joiners cannot dilute
/// the threshold up front. The `joined_at` check in `vote` then stops
/// late joiners from voting on this proposal at all.
///
/// Reverts on a zero-address target: such a proposal is always a
/// misconfiguration, since the cross-call would silently fail or burn
/// value.
@entry
export function propose(target: Address, functionName: string, calldata: StaticArray<u8>): u64 {
  const proposer = requireMember();
  if (equals32(target, newAddress())) {
    revertStr("dao: zero address target");
  }
  const prior = storage.proposal_count.read();
  const proposal_id = prior + 1;
  if (proposal_id < prior) {
    revertStr("dao: proposal count overflow");
  }

  storage.proposal_count.write(proposal_id);
  storage.proposal_target.write(proposal_id, target);
  storage.proposal_function.write(proposal_id, functionName);
  storage.proposal_calldata.write(proposal_id, calldata);
  storage.proposal_proposer.write(proposal_id, proposer);
  storage.proposal_created.write(proposal_id, waveTimestamp());
  // Snapshot the member count NOW, so the quorum denominator is fixed for
  // this proposal's lifetime and a later joiner cannot shift the
  // threshold.
  storage.proposal_member_count_snapshot.write(proposal_id, storage.member_count.read());

  events.ProposalCreated(proposal_id, proposer, target);
  return proposal_id;
}

/// Cast a vote. One per voter per proposal.
///
/// Members may only vote on proposals created at or after their
/// `joined_at` timestamp — a member added AFTER a proposal was created
/// cannot vote on it, which closes the late-joiner-stuffs-yes vector.
@entry
export function vote(proposal_id: u64, support: bool): void {
  const voter = requireMember();
  requireOpen(proposal_id);

  if (storage.joined_at.read(voter) > storage.proposal_created.read(proposal_id)) {
    revertStr("dao: voter joined after proposal was created");
  }
  if (waveTimestamp() > deadlineOf(proposal_id)) {
    revertStr("dao: voting window closed");
  }
  if (storage.has_voted.read(proposal_id, voter)) {
    revertStr("dao: already voted on this proposal");
  }
  storage.has_voted.write(proposal_id, voter, true);

  if (support) {
    const prior = storage.proposal_yes.read(proposal_id);
    const next = prior + 1;
    if (next < prior) {
      revertStr("dao: vote count overflow");
    }
    storage.proposal_yes.write(proposal_id, next);
  } else {
    const prior = storage.proposal_no.read(proposal_id);
    const next = prior + 1;
    if (next < prior) {
      revertStr("dao: vote count overflow");
    }
    storage.proposal_no.write(proposal_id, next);
  }
  events.Voted(proposal_id, voter, support);
}

/// Execute a passed proposal. Anyone may call. Cross-calls the recorded
/// target with the recorded calldata, returning true on success and
/// surfacing the failure category as a revert otherwise.
///
/// The quorum denominator is the snapshot taken at proposal creation, not
/// the current member count, so adds and removes between propose and
/// execute cannot shift the threshold under votes already cast.
@entry
export function execute_proposal(proposal_id: u64): bool {
  requireOpen(proposal_id);

  if (waveTimestamp() <= deadlineOf(proposal_id)) {
    revertStr("dao: voting window not yet closed");
  }

  // The threshold is the ceiling of `total * quorum_percent / 100`.
  const total = storage.proposal_member_count_snapshot.read(proposal_id);
  const required = satAdd(satMul(total, storage.quorum_percent.read()), 99) / 100;
  if (storage.proposal_yes.read(proposal_id) < required) {
    revertStr("dao: quorum not reached");
  }

  // Mark executed BEFORE the cross-call — reentrancy-friendly.
  storage.proposal_executed.write(proposal_id, true);

  const r = Call(
    storage.proposal_target.read(proposal_id),
    storage.proposal_function.read(proposal_id),
  )
    .args(storage.proposal_calldata.read(proposal_id))
    .exec();

  // The DAO does not care about the target's return type, only its
  // success shape, so any returned bytes are ignored. That matches the
  // Rust template, which decodes into `()` and treats a decode mismatch
  // as success precisely because the target did execute cleanly.
  //
  // The status is checked before the revert message: a refusal carries no
  // payload, and naming it is more useful than a generic failure.
  if (!r.ok) {
    if (r.status == ERR_INVALID_FUNCTION_NAME) {
      revertStr("dao: target doesn't expose that function");
    }
    const msg = r.revertMessage;
    if (msg.length > 0) {
      revertStr(msg);
    }
    revertStr("dao: target call failed");
  }

  // No `ProposalExecuted { success: false }` is emitted on the failure
  // paths above: they all revert, which rolls back every state change in
  // this frame including the events. Emitting there would never reach the
  // receipt, and leaving it out keeps the trace honest about what the
  // contract actually committed.
  events.ProposalExecuted(proposal_id, true);
  return true;
}

// ─────────────────────────────────────────────────────────────────────
// Views
// ─────────────────────────────────────────────────────────────────────

@entry
@view
export function is_member(addr: Address): bool {
  return storage.members.read(addr);
}

@entry
@view
export function member_count(): u64 {
  return storage.member_count.read();
}

@entry
@view
export function proposal_count(): u64 {
  return storage.proposal_count.read();
}

@entry
@view
export function get_proposal_yes(proposal_id: u64): u64 {
  return storage.proposal_yes.read(proposal_id);
}

@entry
@view
export function get_proposal_no(proposal_id: u64): u64 {
  return storage.proposal_no.read(proposal_id);
}

@entry
@view
export function get_proposal_executed(proposal_id: u64): bool {
  return storage.proposal_executed.read(proposal_id);
}

@entry
@view
export function has_voted_on(proposal_id: u64, voter: Address): bool {
  return storage.has_voted.read(proposal_id, voter);
}

@entry
@view
export function voting_period_secs(): u64 {
  return storage.voting_period_secs.read();
}
