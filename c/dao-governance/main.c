// dao-governance — proposal-and-vote DAO (C port).
//
// Members create proposals carrying a (target, function, calldata) payload;
// peers vote yes/no within a deadline; any caller can fire
// execute_proposal(id) after voting closes if the proposal passed quorum.
// Execution cross-calls the target with the recorded payload + propagates the
// failure category back to the receipt if anything goes wrong.
//
// Lifecycle:
//   1. init             — register the founding member set + config.
//   2. propose          — a member records (target, function, calldata) at the
//                         current wave timestamp.
//   3. vote             — members cast a single yes/no per proposal, guarded by
//                         has_voted[id][voter].
//   4. execute_proposal — after voting_period_secs elapses, anyone fires the
//                         cross-call. Quorum check:
//                         yes >= member_count * quorum_percent / 100.
//
// You write only typed inner functions (`__<fn>_impl`) plus otigen.toml.
// `otigen build` generates pyde_gen.h: the () -> () export shims, typed storage
// accessors, event emitters, and the vec(address) codec. Nothing here derives a
// storage slot, packs a topic, or touches a calldata pointer — the generated
// header owns all of that.

#include "pyde_gen.h"

// ─────────────────────────────────────────────────────────────────────
// Saturating / checked u64 helpers (C has no built-in equivalents).
// ─────────────────────────────────────────────────────────────────────

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    if (s < a) {
        return ~(uint64_t)0;
    }
    return s;
}

static uint64_t saturating_mul(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    uint64_t p = a * b;
    if (p / a != b) {
        return ~(uint64_t)0;
    }
    return p;
}

// 32-byte all-zero address — guards against silently registering the zero
// address as a quorum-participating member or letting a proposal target the
// zero address by accident.
static bool addr_is_zero(pyde_address a) {
    for (int i = 0; i < 32; i++) {
        if (a.bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// UTF-8 validity check — mirrors Rust's `core::str::from_utf8` / Go's
// `utf8.Valid` for the cross-call revert-payload fallback: rejects overlong
// encodings, surrogates, and out-of-range code points.
static bool utf8_valid(const uint8_t *s, int32_t n) {
    int32_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        int32_t extra;
        uint32_t cp, lo, hi;
        if (c < 0x80u) {
            i++;
            continue;
        } else if ((c & 0xE0u) == 0xC0u) {
            extra = 1; cp = c & 0x1Fu; lo = 0x80u; hi = 0x7FFu;
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2; cp = c & 0x0Fu; lo = 0x800u; hi = 0xFFFFu;
        } else if ((c & 0xF8u) == 0xF0u) {
            extra = 3; cp = c & 0x07u; lo = 0x10000u; hi = 0x10FFFFu;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (int32_t j = 1; j <= extra; j++) {
            uint8_t cc = s[i + j];
            if ((cc & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
        }
        if (cp < lo || cp > hi) {
            return false;
        }
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Authorization
// ─────────────────────────────────────────────────────────────────────

static pyde_address require_member(void) {
    pyde_address caller = pyde_caller();
    if (!members_get(caller)) {
        pyde_revert_str("dao: caller is not a member");
    }
    return caller;
}

// Gates `add_member` / `remove_member` to vote-only execution: only the DAO
// itself (via a successful `execute_proposal` that targets itself) can mutate
// the member set. Direct member calls revert, blocking the Sybil-attack class
// where a single compromised key can grow the member set unilaterally.
static void require_self_call(void) {
    if (!pyde_addr_eq(pyde_caller(), pyde_self())) {
        pyde_revert_str("dao: membership changes require a passed proposal");
    }
}

// ─────────────────────────────────────────────────────────────────────
// Entry points
// ─────────────────────────────────────────────────────────────────────

// Constructor — registers the founding member set + writes the voting-period
// and quorum-percent config. The contract itself is fully member-driven
// thereafter — there is no admin role.
void __init_impl(pyde_vec_Address initial_members, uint64_t voting_period_secs, uint64_t quorum_percent) {
    if (quorum_percent == 0 || quorum_percent > 100) {
        pyde_revert_str("dao: quorum_percent must be 1..=100");
    }
    if (voting_period_secs == 0) {
        pyde_revert_str("dao: voting_period_secs must be > 0");
    }
    if (initial_members.len == 0) {
        pyde_revert_str("dao: initial_members must not be empty");
    }
    voting_period_secs_set(voting_period_secs);
    quorum_percent_set(quorum_percent);
    uint64_t join_time = (uint64_t)wave_timestamp();
    uint64_t count = 0;
    for (uint32_t i = 0; i < initial_members.len; i++) {
        pyde_address m = initial_members.ptr[i];
        if (addr_is_zero(m)) {
            pyde_revert_str("dao: zero address member");
        }
        if (!members_get(m)) {
            members_set(m, true);
            joined_at_set(m, join_time);
            emit_MemberAdded(m);
            uint64_t next = count + 1;
            if (next < count) {
                pyde_revert_str("dao: member count overflow");
            }
            count = next;
        }
    }
    member_count_set(count);
}

// Add a new member. Self-call only — invoked via a passed proposal that targets
// the DAO itself with calldata borsh(new_member). A direct member call reverts.
// Records joined_at[new_member] = wave_timestamp so the new member can't vote on
// proposals created before they joined.
void __add_member_impl(pyde_address new_member) {
    require_self_call();
    if (addr_is_zero(new_member)) {
        pyde_revert_str("dao: zero address member");
    }
    if (members_get(new_member)) {
        pyde_revert_str("dao: already a member");
    }
    members_set(new_member, true);
    joined_at_set(new_member, (uint64_t)wave_timestamp());
    uint64_t prior = member_count_get();
    uint64_t next = prior + 1;
    if (next < prior) {
        pyde_revert_str("dao: member count overflow");
    }
    member_count_set(next);
    emit_MemberAdded(new_member);
}

// Remove a member. Self-call only (same gating as `add_member`). Used to revoke
// a compromised key or eject a misbehaving member via the standard proposal
// flow. Reverts if `member` isn't currently in the set.
void __remove_member_impl(pyde_address member) {
    require_self_call();
    if (!members_get(member)) {
        pyde_revert_str("dao: not a member");
    }
    members_set(member, false);
    joined_at_set(member, 0);
    uint64_t prior = member_count_get();
    if (prior == 0) {
        pyde_revert_str("dao: member count underflow");
    }
    member_count_set(prior - 1);
    emit_MemberRemoved(member);
}

// Create a new proposal carrying a (target, function, calldata) payload.
// Returns the proposal id (1-indexed).
//
// Snapshots member_count at proposal-creation time and stores it per-proposal
// so the quorum denominator is locked in: late joiners after this point can't
// dilute the quorum threshold up-front. The per-member joined_at check in
// vote() then prevents late joiners from casting votes on this proposal at all.
uint64_t __propose_impl(pyde_address target, pyde_string function, pyde_bytes calldata) {
    pyde_address proposer = require_member();
    if (addr_is_zero(target)) {
        pyde_revert_str("dao: zero address target");
    }
    uint64_t prior = proposal_count_get();
    uint64_t proposal_id = prior + 1;
    if (proposal_id < prior) {
        pyde_revert_str("dao: proposal count overflow");
    }
    proposal_count_set(proposal_id);
    proposal_target_set(proposal_id, target);
    proposal_function_set(proposal_id, function);
    proposal_calldata_set(proposal_id, calldata);
    proposal_proposer_set(proposal_id, proposer);
    uint64_t now = (uint64_t)wave_timestamp();
    proposal_created_set(proposal_id, now);
    // Snapshot the member count NOW so the quorum denominator is fixed for the
    // lifetime of this proposal — a member joining later can't shift the
    // threshold up.
    proposal_member_count_snapshot_set(proposal_id, member_count_get());
    emit_ProposalCreated(proposal_id, proposer, target);
    return proposal_id;
}

// Cast a vote on a proposal. One per voter per proposal.
//
// Members are only eligible to vote on proposals created at or after their
// joined_at timestamp — a member added AFTER a proposal was created cannot vote
// on it. Prevents the late-joiner-stuffs-yes vector.
void __vote_impl(uint64_t proposal_id, bool support) {
    pyde_address voter = require_member();
    if (addr_is_zero(proposal_proposer_get(proposal_id))) {
        pyde_revert_str("dao: proposal does not exist");
    }
    if (proposal_executed_get(proposal_id)) {
        pyde_revert_str("dao: proposal already executed");
    }
    // Voting eligibility: caller must have been a member at or before this
    // proposal was created. Closes the "join AFTER proposal then vote yes"
    // vector.
    uint64_t created = proposal_created_get(proposal_id);
    uint64_t voter_joined_at = joined_at_get(voter);
    if (voter_joined_at > created) {
        pyde_revert_str("dao: voter joined after proposal was created");
    }
    // Voting window check. `init` rejects period=0, so this branch always
    // evaluates the window — but we keep the saturating add anti-overflow guard
    // around created + period for u64-wraparound safety at large timestamps.
    uint64_t period = voting_period_secs_get();
    // voting_period_secs is seconds; created / now are wave_timestamp
    // milliseconds — convert the period before comparing.
    uint64_t deadline = saturating_add(created, saturating_mul(period, 1000));
    uint64_t now = (uint64_t)wave_timestamp();
    if (now > deadline) {
        pyde_revert_str("dao: voting window closed");
    }
    if (has_voted_get(proposal_id, voter)) {
        pyde_revert_str("dao: already voted on this proposal");
    }
    has_voted_set(proposal_id, voter, true);
    if (support) {
        uint64_t prior = proposal_yes_get(proposal_id);
        uint64_t next = prior + 1;
        if (next < prior) {
            pyde_revert_str("dao: vote count overflow");
        }
        proposal_yes_set(proposal_id, next);
    } else {
        uint64_t prior = proposal_no_get(proposal_id);
        uint64_t next = prior + 1;
        if (next < prior) {
            pyde_revert_str("dao: vote count overflow");
        }
        proposal_no_set(proposal_id, next);
    }
    emit_Voted(proposal_id, voter, support);
}

// Execute a passed proposal. Anyone can call. Cross-calls the recorded target
// with the recorded calldata. Returns true on successful cross-call, surfaces
// the failure category as a revert message otherwise.
//
// Quorum denominator uses the snapshot recorded at proposal creation, NOT the
// current member count — so adds/removes between propose and execute can't
// shift the threshold under the votes that were already cast.
bool __execute_proposal_impl(uint64_t proposal_id) {
    if (addr_is_zero(proposal_proposer_get(proposal_id))) {
        pyde_revert_str("dao: proposal does not exist");
    }
    if (proposal_executed_get(proposal_id)) {
        pyde_revert_str("dao: proposal already executed");
    }
    // Voting window must have closed. `init` rejects period=0 so this branch
    // always runs; the saturating add guards against u64 wraparound at huge
    // timestamps.
    uint64_t created = proposal_created_get(proposal_id);
    uint64_t period = voting_period_secs_get();
    // voting_period_secs is seconds; created / now are wave_timestamp
    // milliseconds — convert the period before comparing.
    uint64_t deadline = saturating_add(created, saturating_mul(period, 1000));
    uint64_t now = (uint64_t)wave_timestamp();
    if (now <= deadline) {
        pyde_revert_str("dao: voting window not yet closed");
    }
    // Quorum check on yes-vote count. Use the snapshot taken at proposal
    // creation as the denominator — fresh members joining between propose and
    // execute don't shift the threshold.
    uint64_t yes = proposal_yes_get(proposal_id);
    uint64_t total = proposal_member_count_snapshot_get(proposal_id);
    uint64_t quorum_percent = quorum_percent_get();
    // Use saturating_mul to avoid overflow on tiny DAOs with large member
    // counts. The threshold is the ceiling of total * quorum_percent / 100.
    uint64_t required = saturating_add(saturating_mul(total, quorum_percent), 99) / 100;
    if (yes < required) {
        pyde_revert_str("dao: quorum not reached");
    }
    // Mark executed BEFORE the cross-call — reentrancy-friendly.
    proposal_executed_set(proposal_id, true);

    pyde_address target = proposal_target_get(proposal_id);
    pyde_string function = proposal_function_get(proposal_id);
    pyde_bytes calldata = proposal_calldata_get(proposal_id);

    // Cross-call into the proposal's target. The DAO doesn't care about the
    // target's return type, only success/failure shape — a zero-byte return
    // decodes cleanly; if the target returns non-empty data we ignore it.
    //
    // We do NOT emit ProposalExecuted{success: false} in the error arms below:
    // those branches all revert, which rolls back every state change (including
    // the events) in this frame.
    //
    // Cap the return buffer at 4 KB to match Rust's DEFAULT_RETURN_BUFFER_BYTES,
    // so a target returning more than that fails identically across languages
    // (the DAO ignores the return value; only the success/revert boundary
    // matters). gas_limit far above any real budget = forward all remaining.
    uint8_t value[16];
    for (int i = 0; i < 16; i++) {
        value[i] = 0;
    }
    uint8_t out[4096];
    int32_t out_len = 4096;
    int32_t rc = cross_call(
        target.bytes,
        function.ptr, (int32_t)function.len,
        calldata.ptr, (int32_t)calldata.len,
        value,
        (int64_t)1 << 62,
        out, &out_len);

    bool success;
    if (rc == 0) {
        // Callee executed cleanly. Mirrors Rust's Ok(()) plus the DecodeError
        // arm (a non-() return still counts as success).
        success = true;
    } else if (rc == -13 /* ErrInvalidFunctionName */) {
        pyde_revert_str("dao: target doesn't expose that function");
    } else if (rc == -3 /* ErrInsufficientBalance */ ||
               rc == -9 /* ErrReentrancyBlocked */ ||
               rc == -12 /* ErrValueTransferNotPayable */) {
        // Rust's Err(_) catch-all over the non-Reverted CallError variants.
        pyde_revert_str("dao: target call failed");
    } else {
        // CallError::Reverted arm (chain status -10 and any other code). The
        // host writes the callee's revert payload into out — mirror Rust:
        // surface the target's utf8 revert message, falling back to
        // "dao: target reverted" if it isn't valid utf8.
        int32_t n = out_len;
        if (n < 0) {
            n = 0;
        }
        if (n > 4096) {
            n = 4096;
        }
        if (utf8_valid(out, n)) {
            revert(out, n);
        }
        pyde_revert_str("dao: target reverted");
    }

    emit_ProposalExecuted(proposal_id, success);
    return success;
}

// ─────────────────────────────────────────────────────────────────────
// Views
// ─────────────────────────────────────────────────────────────────────

bool __is_member_impl(pyde_address addr) {
    return members_get(addr);
}

uint64_t __member_count_impl(void) {
    return member_count_get();
}

uint64_t __proposal_count_impl(void) {
    return proposal_count_get();
}

uint64_t __get_proposal_yes_impl(uint64_t proposal_id) {
    return proposal_yes_get(proposal_id);
}

uint64_t __get_proposal_no_impl(uint64_t proposal_id) {
    return proposal_no_get(proposal_id);
}

bool __get_proposal_executed_impl(uint64_t proposal_id) {
    return proposal_executed_get(proposal_id);
}

bool __has_voted_on_impl(uint64_t proposal_id, pyde_address voter) {
    return has_voted_get(proposal_id, voter);
}

uint64_t __voting_period_secs_impl(void) {
    return voting_period_secs_get();
}
