//! `dao-governance` — proposal-and-vote DAO.
//!
//! Exercises `pyde::call::execute<T, CallError>` end-to-end: members
//! create proposals carrying a `(target, function, calldata)` payload;
//! peers vote yes/no within a deadline; any caller can fire
//! `execute_proposal(id)` after voting closes if the proposal passed
//! quorum. Execution cross-calls the target with the recorded
//! payload + propagates the failure category back to the receipt if
//! anything goes wrong.
//!
//! ## Lifecycle
//!
//!   1. **init**       — register the founding member set + config.
//!   2. **propose**    — a member records (target, function, calldata)
//!                       at the current wave timestamp.
//!   3. **vote**       — members cast a single yes/no per proposal,
//!                       guarded by `has_voted[id][voter]`.
//!   4. **execute_proposal** — after `voting_period_secs` elapses,
//!                       anyone fires the cross-call. Quorum check:
//!                       `yes >= member_count * quorum_percent / 100`.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use borsh::to_vec;
use core::panic::PanicInfo;
use pyde_host as pyde;
use pyde_host::call::CallError;
use pyde_host::Address;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

pyde::declare_storage!();
pyde::declare_events!();

/// 32-byte all-zero address — guards against silently registering
/// the zero address as a quorum-participating member or letting a
/// proposal target the zero address by accident.
const ZERO_ADDRESS: Address = [0u8; 32];

// ─────────────────────────────────────────────────────────────────────
// Authorization
// ─────────────────────────────────────────────────────────────────────

fn require_member() -> Address {
    let caller = pyde::ctx::caller();
    if !storage::members().read(&caller) {
        pyde::revert("dao: caller is not a member");
    }
    caller
}

/// Gates `add_member` / `remove_member` to vote-only execution: only
/// the DAO itself (via a successful `execute_proposal` that targets
/// itself) can mutate the member set. Direct member calls revert,
/// blocking the Sybil-attack class where a single compromised key
/// can grow the member set unilaterally.
fn require_self_call() {
    if pyde::ctx::caller() != pyde::ctx::self_address() {
        pyde::revert("dao: membership changes require a passed proposal");
    }
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

/// Constructor — registers the founding member set + writes the
/// voting-period and quorum-percent config. The contract itself is
/// fully member-driven thereafter — there is no admin role.
///
/// Reverts on:
/// - empty `initial_members` (would brick the DAO — no one could
///   ever propose),
/// - any zero-address entry in `initial_members`,
/// - `quorum_percent` outside `1..=100`,
/// - `voting_period_secs == 0` — required for production safety so
///   propose → vote → execute can't all land in the same wave.
#[pyde::entry]
fn init(initial_members: Vec<Address>, voting_period_secs: u64, quorum_percent: u64) {
    if quorum_percent == 0 || quorum_percent > 100 {
        pyde::revert("dao: quorum_percent must be 1..=100");
    }
    if voting_period_secs == 0 {
        pyde::revert("dao: voting_period_secs must be > 0");
    }
    if initial_members.is_empty() {
        pyde::revert("dao: initial_members must not be empty");
    }
    storage::voting_period_secs().write(voting_period_secs);
    storage::quorum_percent().write(quorum_percent);
    let join_time = pyde::ctx::wave_timestamp();
    let mut count: u64 = 0;
    for m in initial_members.iter() {
        if *m == ZERO_ADDRESS {
            pyde::revert("dao: zero address member");
        }
        if !storage::members().read(m) {
            storage::members().write(m, true);
            storage::joined_at().write(m, join_time);
            events::MemberAdded { member: *m }.emit();
            count = count
                .checked_add(1)
                .unwrap_or_else(|| pyde::revert("dao: member count overflow"));
        }
    }
    storage::member_count().write(count);
}

/// Add a new member. Self-call only — invoked via a passed proposal
/// that targets the DAO itself with calldata `borsh(new_member)`.
/// A direct member call reverts with `dao: membership changes
/// require a passed proposal`. Records `joined_at[new_member] =
/// wave_timestamp` so the new member can't vote on proposals created
/// before they joined.
#[pyde::entry]
fn add_member(new_member: Address) {
    require_self_call();
    if new_member == ZERO_ADDRESS {
        pyde::revert("dao: zero address member");
    }
    if storage::members().read(&new_member) {
        pyde::revert("dao: already a member");
    }
    storage::members().write(&new_member, true);
    storage::joined_at().write(&new_member, pyde::ctx::wave_timestamp());
    let prior = storage::member_count().read();
    let next = prior
        .checked_add(1)
        .unwrap_or_else(|| pyde::revert("dao: member count overflow"));
    storage::member_count().write(next);
    events::MemberAdded { member: new_member }.emit();
}

/// Remove a member. Self-call only (same gating as `add_member`).
/// Used to revoke a compromised key or eject a misbehaving member
/// via the standard proposal flow. Reverts if `member` isn't
/// currently in the set.
#[pyde::entry]
fn remove_member(member: Address) {
    require_self_call();
    if !storage::members().read(&member) {
        pyde::revert("dao: not a member");
    }
    storage::members().write(&member, false);
    storage::joined_at().write(&member, 0);
    let prior = storage::member_count().read();
    if prior == 0 {
        pyde::revert("dao: member count underflow");
    }
    storage::member_count().write(prior - 1);
    events::MemberRemoved { member }.emit();
}

/// Create a new proposal carrying a `(target, function, calldata)`
/// payload. Returns the proposal id (1-indexed).
///
/// Snapshots `member_count` at proposal-creation time and stores it
/// per-proposal so the quorum denominator is locked in: late joiners
/// after this point can't dilute the quorum threshold up-front. The
/// per-member `joined_at` check in `vote()` then prevents late
/// joiners from casting votes on this proposal at all.
///
/// Reverts on zero-address target — proposals to the zero address
/// are always a misconfiguration (the cross-call would silently
/// fail or burn value).
#[pyde::entry]
fn propose(target: Address, function: String, calldata: Vec<u8>) -> u64 {
    let proposer = require_member();
    if target == ZERO_ADDRESS {
        pyde::revert("dao: zero address target");
    }
    let proposal_id = storage::proposal_count()
        .read()
        .checked_add(1)
        .unwrap_or_else(|| pyde::revert("dao: proposal count overflow"));
    storage::proposal_count().write(proposal_id);
    storage::proposal_target().write(&proposal_id, target);
    storage::proposal_function().write(&proposal_id, function);
    storage::proposal_calldata().write(&proposal_id, calldata);
    storage::proposal_proposer().write(&proposal_id, proposer);
    let now = pyde::ctx::wave_timestamp();
    storage::proposal_created().write(&proposal_id, now);
    // Snapshot the member count NOW so the quorum denominator is
    // fixed for the lifetime of this proposal — a member joining
    // later can't shift the threshold up.
    storage::proposal_member_count_snapshot()
        .write(&proposal_id, storage::member_count().read());
    events::ProposalCreated { proposal_id, proposer, target }.emit();
    proposal_id
}

/// Cast a vote on a proposal. One per voter per proposal.
///
/// Members are only eligible to vote on proposals created at or
/// after their `joined_at` timestamp — a member added AFTER a
/// proposal was created cannot vote on it. Prevents the
/// late-joiner-stuffs-yes vector.
#[pyde::entry]
fn vote(proposal_id: u64, support: bool) {
    let voter = require_member();
    if storage::proposal_proposer().read(&proposal_id) == [0u8; 32] {
        pyde::revert("dao: proposal does not exist");
    }
    if storage::proposal_executed().read(&proposal_id) {
        pyde::revert("dao: proposal already executed");
    }
    // Voting eligibility: caller must have been a member at or
    // before this proposal was created. Closes the
    // "join AFTER proposal then vote yes" vector.
    let created = storage::proposal_created().read(&proposal_id);
    let voter_joined_at = storage::joined_at().read(&voter);
    if voter_joined_at > created {
        pyde::revert("dao: voter joined after proposal was created");
    }
    // Voting window check. `init` rejects period=0, so this branch
    // always evaluates the window — but we keep the saturating add
    // anti-overflow guard around `created + period` for u64-wraparound
    // safety at large timestamps.
    let period = storage::voting_period_secs().read();
    // `voting_period_secs` is seconds; `created` / `now` are wave_timestamp
    // milliseconds — convert the period before comparing.
    let deadline = created.saturating_add(period.saturating_mul(1000));
    let now = pyde::ctx::wave_timestamp();
    if now > deadline {
        pyde::revert("dao: voting window closed");
    }
    if storage::has_voted().read(&proposal_id, &voter) {
        pyde::revert("dao: already voted on this proposal");
    }
    storage::has_voted().write(&proposal_id, &voter, true);
    if support {
        let prior = storage::proposal_yes().read(&proposal_id);
        let next = prior
            .checked_add(1)
            .unwrap_or_else(|| pyde::revert("dao: vote count overflow"));
        storage::proposal_yes().write(&proposal_id, next);
    } else {
        let prior = storage::proposal_no().read(&proposal_id);
        let next = prior
            .checked_add(1)
            .unwrap_or_else(|| pyde::revert("dao: vote count overflow"));
        storage::proposal_no().write(&proposal_id, next);
    }
    events::Voted { proposal_id, voter, support }.emit();
}

/// Execute a passed proposal. Anyone can call. Cross-calls the
/// recorded target with the recorded calldata. Returns true on
/// successful cross-call, surfaces the failure category as a
/// revert message otherwise.
///
/// Quorum denominator uses the snapshot recorded at proposal
/// creation, NOT the current member count — so adds/removes
/// between propose and execute can't shift the threshold under
/// the votes that were already cast.
#[pyde::entry]
fn execute_proposal(proposal_id: u64) -> bool {
    if storage::proposal_proposer().read(&proposal_id) == [0u8; 32] {
        pyde::revert("dao: proposal does not exist");
    }
    if storage::proposal_executed().read(&proposal_id) {
        pyde::revert("dao: proposal already executed");
    }
    // Voting window must have closed. `init` rejects period=0 so
    // this branch always runs; the saturating add guards against
    // u64 wraparound at huge timestamps.
    let created = storage::proposal_created().read(&proposal_id);
    let period = storage::voting_period_secs().read();
    // `voting_period_secs` is seconds; `created` / `now` are wave_timestamp
    // milliseconds — convert the period before comparing.
    let deadline = created.saturating_add(period.saturating_mul(1000));
    let now = pyde::ctx::wave_timestamp();
    if now <= deadline {
        pyde::revert("dao: voting window not yet closed");
    }
    // Quorum check on yes-vote count. Use the snapshot taken at
    // proposal creation as the denominator — fresh members joining
    // between propose and execute don't shift the threshold.
    let yes = storage::proposal_yes().read(&proposal_id);
    let total = storage::proposal_member_count_snapshot().read(&proposal_id);
    let quorum_percent = storage::quorum_percent().read();
    // Use saturating_mul to avoid overflow on tiny DAOs with large
    // member counts. The threshold is the ceiling of
    // `total * quorum_percent / 100`.
    let required = total
        .saturating_mul(quorum_percent)
        .saturating_add(99)
        .saturating_div(100);
    if yes < required {
        pyde::revert("dao: quorum not reached");
    }
    // Mark executed BEFORE the cross-call — reentrancy-friendly.
    storage::proposal_executed().write(&proposal_id, true);

    let target = storage::proposal_target().read(&proposal_id);
    let function = storage::proposal_function().read(&proposal_id);
    let calldata = storage::proposal_calldata().read(&proposal_id);

    // Cross-call into the proposal's target. The DAO doesn't
    // care about the target's return type, only success/failure
    // shape — `()` decodes a zero-byte return cleanly; if the
    // target returns non-empty data we ignore it.
    //
    // We do NOT emit `ProposalExecuted { success: false }` in the
    // error arms below: those branches all `pyde::revert`, which
    // rolls back every state change (including the events) in this
    // frame. The duplicated emits would never reach the receipt
    // anyway; removing them keeps the trace honest about what the
    // contract actually committed.
    let success = match pyde::call::execute::<()>(&target, &function, &calldata) {
        Ok(()) => true,
        Err(CallError::Reverted(bytes)) => {
            let msg = core::str::from_utf8(&bytes).unwrap_or("dao: target reverted");
            pyde::revert(msg);
        }
        Err(CallError::InvalidFunction) => {
            pyde::revert("dao: target doesn't expose that function");
        }
        Err(CallError::DecodeError) => {
            // Non-fatal — the target returned bytes that aren't ().
            // Treat as success since the target did execute cleanly;
            // the DAO doesn't need the return value.
            true
        }
        Err(_) => {
            pyde::revert("dao: target call failed");
        }
    };

    events::ProposalExecuted { proposal_id, success }.emit();
    success
}

// ─────────────────────────────────────────────────────────────────────
// Views
// ─────────────────────────────────────────────────────────────────────

#[pyde::entry]
fn is_member(addr: Address) -> bool {
    storage::members().read(&addr)
}

#[pyde::entry]
fn member_count() -> u64 {
    storage::member_count().read()
}

#[pyde::entry]
fn proposal_count() -> u64 {
    storage::proposal_count().read()
}

#[pyde::entry]
fn get_proposal_yes(proposal_id: u64) -> u64 {
    storage::proposal_yes().read(&proposal_id)
}

#[pyde::entry]
fn get_proposal_no(proposal_id: u64) -> u64 {
    storage::proposal_no().read(&proposal_id)
}

#[pyde::entry]
fn get_proposal_executed(proposal_id: u64) -> bool {
    storage::proposal_executed().read(&proposal_id)
}

#[pyde::entry]
fn has_voted_on(proposal_id: u64, voter: Address) -> bool {
    storage::has_voted().read(&proposal_id, &voter)
}

#[pyde::entry]
fn voting_period_secs() -> u64 {
    storage::voting_period_secs().read()
}

// Suppress unused-import warning when `to_vec` isn't exercised
// directly in tests — kept here as the borsh re-export pattern for
// downstream contracts that build calldata for their own
// `pyde::call::execute` calls.
#[allow(dead_code)]
fn _borsh_used() {
    let _ = to_vec::<u64>(&0u64);
}
