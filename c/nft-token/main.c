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
//
// You write only the typed __<fn>_impl bodies below; `otigen build` reads
// otigen.toml and generates pyde_gen.h — the typed storage accessors, event
// emitters, borsh runtime, and () -> () entry-dispatch shims — around them.

#include "pyde_gen.h"

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

// The surface this contract conforms to.
#define STANDARD "pts-n/1"

// Max bytes of `data` forwarded by transfer_call.
#define MAX_CALL_DATA 4096u

// Required return of a conformant receiver:
// u32::from_le_bytes(Blake3("pts/on_nft_received/1")[0..4]).
#define ACK_NFT 1452691067u // LE bytes 7b 4e 96 56

// 32-byte role identifiers carried by RoleTransfer (PIP-0005 §3):
// Blake3("minter") / Blake3("manager").
static const pyde_bytes32 ROLE_MINTER = {{
    0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
    0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
    0xe5, 0xdd,
}};
static const pyde_bytes32 ROLE_MANAGER = {{
    0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
    0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
    0x35, 0xc1,
}};

// "No owner" / "cleared approval" sentinel.
static const pyde_address ZERO_ADDRESS = {{0}};

static bool is_zero(pyde_address a) {
    return pyde_addr_eq(a, ZERO_ADDRESS);
}

// A pyde_string view over a NUL-terminated C string literal (borrowed —
// never mutated by the encoder).
static pyde_string str_from(const char *s, uint32_t n) {
    pyde_string r;
    r.ptr = (uint8_t *)s;
    r.len = n;
    return r;
}

// ─── Internal helpers (forward decls) ─────────────────────────────────

static void require_exists(uint64_t token_id);
static void guard_recipient(pyde_address to);
static bool is_authorized(pyde_address owner, pyde_address spender, uint64_t token_id);
static void move_token(pyde_address caller, pyde_address from, pyde_address to, uint64_t token_id);

// ─── Constructor ───────────────────────────────────────────────────────

// Metadata + roles to the deployer. max_supply = 0 = uncapped.
void __init_impl(pyde_string name, pyde_string symbol, uint64_t max_supply) {
    pyde_address deployer = pyde_caller();
    token_name_set(name);
    token_symbol_set(symbol);
    max_supply_set(max_supply);
    minter_set(deployer);
    manager_set(deployer);
    emit_RoleTransfer(ROLE_MINTER, deployer, ZERO_ADDRESS);
    emit_RoleTransfer(ROLE_MANAGER, deployer, ZERO_ADDRESS);
}

// ─── Views ─────────────────────────────────────────────────────────────

pyde_string __standard_impl(void) {
    return str_from(STANDARD, (uint32_t)(sizeof(STANDARD) - 1));
}

pyde_string __name_impl(void) {
    return token_name_get();
}

pyde_string __symbol_impl(void) {
    return token_symbol_get();
}

// Live count (decremented by burn); ids themselves are monotonic.
uint64_t __total_supply_impl(void) {
    return total_supply_get();
}

// 0 = uncapped.
uint64_t __max_supply_impl(void) {
    return max_supply_get();
}

pyde_address __owner_of_impl(uint64_t token_id) {
    pyde_address owner = owners_get(token_id);
    if (is_zero(owner)) {
        pyde_revert_str("token:nonexistent");
    }
    return owner;
}

// Count owned by `owner`. Consistent with pts-f: a never-holder reads as 0,
// no special-casing.
uint64_t __balance_of_impl(pyde_address owner) {
    return balances_get(owner);
}

pyde_address __get_approved_impl(uint64_t token_id) {
    require_exists(token_id);
    return token_approvals_get(token_id);
}

bool __is_approved_for_all_impl(pyde_address owner, pyde_address operator) {
    return operator_approvals_get(owner, operator);
}

pyde_string __token_uri_impl(uint64_t token_id) {
    require_exists(token_id);
    return token_uris_get(token_id);
}

pyde_address __minter_impl(void) {
    return minter_get();
}

pyde_address __manager_impl(void) {
    return manager_get();
}

// ─── Transfers ─────────────────────────────────────────────────────────

// Move `token_id` from `from` to `to`. Caller must be the owner, the per-id
// approved address, or an approved operator. NEVER invokes recipient code.
void __transfer_from_impl(pyde_address from, pyde_address to, uint64_t token_id) {
    pyde_address caller = pyde_caller();
    move_token(caller, from, to, token_id);
    emit_Transfer(from, to, token_id);
}

// Settle-then-notify (PIP-0005 §12): ownership fully moved and Transfer
// emitted FIRST, then the recipient's on_nft_received(operator, from, id, data)
// must return ACK_NFT — a revert, a wrong value, or a fallback-swallowed
// name-miss unwinds the entire operation (token:bad_receiver).
uint32_t __transfer_call_impl(pyde_address to, uint64_t token_id, pyde_vec_U8 data) {
    if (data.len > MAX_CALL_DATA) {
        pyde_revert_str("token:data_too_large");
    }
    pyde_address operator = pyde_caller();
    pyde_address from = owners_get(token_id);

    // (1) settle — the caller must be authorized for the id
    move_token(operator, from, to, token_id);
    // (2) emit — discarded with the frame if the notify leg reverts
    emit_Transfer(from, to, token_id);

    // (3) notify. Args filled by the token — never spoofable.
    // borsh tuple: (operator, from, token_id, data).
    pyde_enc __e = pyde_enc_new();
    pyde_enc_address(&__e, operator);
    pyde_enc_address(&__e, from);
    pyde_enc_u64(&__e, token_id);
    __encodeVecU8(&__e, data);
    pyde_bytes calldata = pyde_enc_finish(&__e);

    // Cross-call the receiver: no value attached (16 zero LE bytes), all
    // remaining gas forwarded (i64::MAX). Return buffer sized like the Rust
    // wrapper's DEFAULT_RETURN_BUFFER_BYTES.
    uint8_t value16[16] = {0};
    uint8_t *retbuf = (uint8_t *)__pyde_alloc(4096);
    int32_t retlen = 4096;
    int32_t status = cross_call(
        to.bytes,
        (const uint8_t *)"on_nft_received", 15,
        calldata.ptr, (int32_t)calldata.len,
        value16,
        (int64_t)0x7fffffffffffffffLL,
        retbuf,
        &retlen);
    if (status == 0 && retlen >= 4) {
        pyde_dec __rd = pyde_dec_new(retbuf, (uint32_t)retlen);
        if (pyde_dec_u32(&__rd) == ACK_NFT) {
            return ACK_NFT;
        }
    }
    pyde_revert_str("token:bad_receiver");
}

// ─── Approvals ─────────────────────────────────────────────────────────

// Per-id approval; the zero address clears it. Caller must be the owner or an
// approved operator.
void __approve_impl(pyde_address to, uint64_t token_id) {
    pyde_address owner = owners_get(token_id);
    if (is_zero(owner)) {
        pyde_revert_str("token:nonexistent");
    }
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, owner) && !operator_approvals_get(owner, caller)) {
        pyde_revert_str("token:not_authorized");
    }
    token_approvals_set(token_id, to);
    emit_Approval(owner, to, token_id);
}

// Collection-wide operator grant for the caller's tokens.
void __set_approval_for_all_impl(pyde_address operator, bool approved) {
    pyde_address owner = pyde_caller();
    if (is_zero(operator) || pyde_addr_eq(operator, owner)) {
        pyde_revert_str("token:invalid_operator");
    }
    operator_approvals_set(owner, operator, approved);
    emit_ApprovalForAll(owner, operator, approved);
}

// ─── Supply & roles ────────────────────────────────────────────────────

// Minter only. Assigns the next monotonic id (starting at 1; never recycled),
// stores the metadata on-chain, emits the zero-sentinel Transfer. Returns the
// fresh id.
uint64_t __mint_impl(pyde_address to, pyde_string uri) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, minter_get()) || is_zero(caller)) {
        pyde_revert_str("token:not_minter");
    }
    guard_recipient(to);

    uint64_t supply = total_supply_get();
    uint64_t cap = max_supply_get();
    if (cap > 0 && supply >= cap) {
        pyde_revert_str("token:cap_exceeded");
    }

    uint64_t next_id = next_id_get();
    if (next_id == 0xFFFFFFFFFFFFFFFFULL) {
        pyde_revert_str("token:overflow");
    }
    uint64_t token_id = next_id + 1;
    next_id_set(token_id);
    total_supply_set(supply + 1);
    owners_set(token_id, to);
    uint64_t bal = balances_get(to);
    balances_set(to, bal + 1);
    token_uris_set(token_id, uri);

    emit_Transfer(ZERO_ADDRESS, to, token_id);
    return token_id;
}

// Owner or authorized. The id is retired forever: ownership zeroed (never
// deleted), live count decremented, monotonic next_id untouched.
void __burn_impl(uint64_t token_id) {
    pyde_address caller = pyde_caller();
    pyde_address owner = owners_get(token_id);
    if (is_zero(owner)) {
        pyde_revert_str("token:nonexistent");
    }
    if (!is_authorized(owner, caller, token_id)) {
        pyde_revert_str("token:not_authorized");
    }
    owners_set(token_id, ZERO_ADDRESS);
    token_approvals_set(token_id, ZERO_ADDRESS);
    uint64_t bal = balances_get(owner);
    if (bal > 0) {
        bal--;
    }
    balances_set(owner, bal);
    uint64_t supply = total_supply_get();
    if (supply > 0) {
        supply--;
    }
    total_supply_set(supply);
    emit_Transfer(owner, ZERO_ADDRESS, token_id);
}

// Manager only. Zero address = provable renounce.
void __set_minter_impl(pyde_address holder) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, manager_get()) || is_zero(caller)) {
        pyde_revert_str("token:not_manager");
    }
    pyde_address previous = minter_get();
    minter_set(holder);
    emit_RoleTransfer(ROLE_MINTER, holder, previous);
}

// Manager only. Renouncing freezes role governance permanently.
void __set_manager_impl(pyde_address holder) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, manager_get()) || is_zero(caller)) {
        pyde_revert_str("token:not_manager");
    }
    pyde_address previous = manager_get();
    manager_set(holder);
    emit_RoleTransfer(ROLE_MANAGER, holder, previous);
}

// ─── Internal helpers ──────────────────────────────────────────────────

static void require_exists(uint64_t token_id) {
    if (is_zero(owners_get(token_id))) {
        pyde_revert_str("token:nonexistent");
    }
}

// Recipient guards shared by every credit path: the zero address and the
// token's own contract address.
static void guard_recipient(pyde_address to) {
    if (is_zero(to) || pyde_addr_eq(to, pyde_self())) {
        pyde_revert_str("token:invalid_recipient");
    }
}

// Whether `spender` may move `token_id`: owner, per-id approved, or an
// approved operator.
static bool is_authorized(pyde_address owner, pyde_address spender, uint64_t token_id) {
    if (pyde_addr_eq(spender, owner)) {
        return true;
    }
    pyde_address approved = token_approvals_get(token_id);
    if (pyde_addr_eq(approved, spender) && !is_zero(approved)) {
        return true;
    }
    return operator_approvals_get(owner, spender);
}

// Ownership move with the full guard set. Clears the per-id approval
// atomically (same slot family — no extra conflict surface).
static void move_token(pyde_address caller, pyde_address from, pyde_address to, uint64_t token_id) {
    pyde_address owner = owners_get(token_id);
    if (is_zero(owner)) {
        pyde_revert_str("token:nonexistent");
    }
    if (!pyde_addr_eq(owner, from)) {
        pyde_revert_str("token:not_owner");
    }
    guard_recipient(to);
    if (!is_authorized(owner, caller, token_id)) {
        pyde_revert_str("token:not_authorized");
    }
    token_approvals_set(token_id, ZERO_ADDRESS);
    uint64_t from_bal = balances_get(from);
    if (from_bal > 0) {
        from_bal--;
    }
    balances_set(from, from_bal);
    uint64_t to_bal = balances_get(to);
    if (to_bal == 0xFFFFFFFFFFFFFFFFULL) {
        pyde_revert_str("token:overflow");
    }
    balances_set(to, to_bal + 1);
    owners_set(token_id, to);
}
