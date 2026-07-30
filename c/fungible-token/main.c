// fungible-token — the PTS-F reference implementation (pts-f/1,
// PIP-0005), default extension configuration, ported to C.
//
// What this surface fixes, each deviation pointing at a documented
// loss class on other chains:
//
//   - Revert-only mutations. No boolean returns to mis-handle; failures
//     are canonical machine-readable token:* codes that propagate
//     verbatim through cross_call unwinds.
//   - Expiring, delta-only allowances. Every grant carries a mandatory
//     expiry wave (TTL-capped at ~1 year); increase/decrease deltas
//     replace raw overwrites, so the approve race is dead by
//     construction and "unlimited forever" is unrepresentable.
//   - Settle-then-notify deposits. transfer_call writes balances and
//     emits Transfer FIRST, then notifies the recipient, which must
//     return the ACK_TOKEN acknowledgement — a name-miss falling
//     through to a fallback cannot silently swallow tokens. Plain
//     transfer never invokes recipient code.
//   - Consent-visible control. Minter/manager roles are declared state,
//     rotated by the manager, renounced by provable zeroing.
//   - Parallel-execution-ready layout. A transfer writes exactly the two
//     parties' balance slots; total_supply is written only by
//     mint/burn. Disjoint transfers commute under Block-STM.
//
// Storage economics: balances and allowances are zeroed, never deleted —
// the chain has no gas refunds, so sdelete is strictly costlier than
// writing zero.
//
// The seam: otigen build generates pyde_gen.h from otigen.toml (typed
// storage accessors, event emitters, custom-type + vec codecs, and the
// () -> () entry-dispatch shims). This file #includes it and defines the
// __<fn>_impl bodies it forward-declares.

#include "pyde_gen.h"

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

// The surface this contract conforms to.
#define STANDARD "pts-f/1"
#define STANDARD_LEN 7

// Hard cap on allowance lifetime: ~1 year at 500 ms/wave. Even a maximal
// grant self-destructs.
#define MAX_ALLOWANCE_TTL_WAVES ((uint64_t)63072000)

// Max owners accepted by balance_of_batch.
#define MAX_BATCH ((uint32_t)256)

// Max bytes of data forwarded by transfer_call — bounds payload-driven
// gas griefing under no-refund economics.
#define MAX_CALL_DATA ((uint32_t)4096)

// Required return of a conformant receiver:
// u32::from_le_bytes(Blake3("pts/on_token_received/1")[0..4]).
#define ACK_TOKEN ((uint32_t)2266754145u) // LE bytes 61 ec 1b 87

// Extension flags reported by token_info() (PIP-0005 §5): this is the
// default configuration — bit 4 (transfer_call) + bit 5 (burnable).
#define EXTENSION_FLAGS ((uint32_t)((1u << 4) | (1u << 5)))

// The default gas budget for cross-calls: the engine forwards
// min(gas, parent_remaining), so a value far above any real budget means
// "forward all remaining" (mirrors the Go binding's forwardAllGas).
#define FORWARD_ALL_GAS ((int64_t)1 << 62)

// StatusOK is the success return of every i32-returning host fn.
#define STATUS_OK ((int32_t)0)

// The null address.
static const pyde_address ZERO_ADDRESS = {{0}};

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

// ─── Small numeric / value helpers ────────────────────────────────────

// pyde_str_lit wraps a string literal as a pyde_string (no runtime copy —
// view returns borrow the literal, torn down with the call frame).
static pyde_string pyde_str_lit(const char *s, uint32_t n) {
    pyde_string r;
    r.ptr = (uint8_t *)s;
    r.len = n;
    return r;
}

static uint64_t now_wave(void) { return (uint64_t)wave_id(); }

// checked_add_u128 mirrors Rust u128::checked_add: false on wrap.
static bool checked_add_u128(__uint128_t a, __uint128_t b, __uint128_t *out) {
    __uint128_t s = a + b;
    if (s < a) {
        return false;
    }
    *out = s;
    return true;
}

// sat_add_u64 is a saturating u64 add (Rust u64::saturating_add).
static uint64_t sat_add_u64(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    if (s < a) {
        return ~(uint64_t)0;
    }
    return s;
}

// sat_sub_u128 is a saturating u128 sub, floored at zero (Rust
// u128::saturating_sub).
static __uint128_t sat_sub_u128(__uint128_t a, __uint128_t b) {
    if (a < b) {
        return (__uint128_t)0;
    }
    return a - b;
}

// ─── Custom types (declared in otigen.toml [types.*]) ─────────────────

// Allowance is one allowance grant, as read/written through its two
// sibling storage slots (allowance_amounts + allowance_expiries, both
// keyed (owner, spender)). {0, 0} means "no allowance".
typedef struct {
    __uint128_t amount;
    uint64_t expiry_wave;
} Allowance;

static Allowance read_allowance(pyde_address owner, pyde_address spender) {
    Allowance a;
    a.amount = allowance_amounts_get(owner, spender);
    a.expiry_wave = allowance_expiries_get(owner, spender);
    return a;
}

// ─── Internal helpers ──────────────────────────────────────────────────

// guard_recipient holds the recipient guards shared by every credit path:
// the zero address (burn must be explicit) and the token's own address —
// the single largest measured stuck-token bucket on chains without this
// check.
static void guard_recipient(pyde_address to) {
    if (pyde_addr_eq(to, ZERO_ADDRESS) || pyde_addr_eq(to, pyde_self())) {
        pyde_revert_str("token:invalid_recipient");
    }
}

// move_tokens debits from, credits to, with the full guard set. from == to
// validates and no-ops (the two slots are the same slot).
static void move_tokens(pyde_address from, pyde_address to, __uint128_t amount) {
    guard_recipient(to);
    __uint128_t from_bal = balances_get(from);
    if (from_bal < amount) {
        pyde_revert_str("token:insufficient_balance");
    }
    if (pyde_addr_eq(from, to)) {
        return;
    }
    __uint128_t to_bal = balances_get(to);
    __uint128_t to_next;
    if (!checked_add_u128(to_bal, amount, &to_next)) {
        pyde_revert_str("token:overflow");
    }
    balances_set(from, from_bal - amount);
    balances_set(to, to_next);
}

static void burn_tokens(pyde_address from, __uint128_t amount) {
    __uint128_t bal = balances_get(from);
    if (bal < amount) {
        pyde_revert_str("token:insufficient_balance");
    }
    balances_set(from, bal - amount);
    __uint128_t supply = total_supply_get();
    total_supply_set(sat_sub_u128(supply, amount));
    emit_Transfer(from, ZERO_ADDRESS, amount);
}

// effective_allowance is the stored grant if live, {0, 0} if expired or
// absent. Live means wave_id() <= expiry_wave.
static Allowance effective_allowance(pyde_address owner, pyde_address spender) {
    Allowance stored = read_allowance(owner, spender);
    if (stored.amount > 0 && now_wave() <= stored.expiry_wave) {
        return stored;
    }
    Allowance zero = {(__uint128_t)0, 0};
    return zero;
}

// spend_allowance decrements a live allowance by amount, distinguishing
// "expired" from "too small" so integrators get the retriable signal.
static void spend_allowance(pyde_address owner, pyde_address spender, __uint128_t amount) {
    Allowance stored = read_allowance(owner, spender);
    if (stored.amount > 0 && now_wave() > stored.expiry_wave) {
        pyde_revert_str("token:allowance_expired");
    }
    Allowance live = effective_allowance(owner, spender);
    if (live.amount < amount) {
        pyde_revert_str("token:insufficient_allowance");
    }
    allowance_amounts_set(owner, spender, live.amount - amount);
}

// write_allowance writes + emits the absolute post-state — indexers
// reconstruct allowance state from the latest Approval per pair, no delta
// replay needed.
static void write_allowance(pyde_address owner, pyde_address spender, Allowance next) {
    allowance_amounts_set(owner, spender, next.amount);
    allowance_expiries_set(owner, spender, next.expiry_wave);
    emit_Approval(owner, spender, next.amount, next.expiry_wave);
}

// ─── Constructor ───────────────────────────────────────────────────────

// Set metadata, mint initial_supply to the deployer, and seat the deployer
// as both minter and manager. max_supply = 0 = uncapped.
//
// decimals is a display-only hint. The PTS default (and the value every
// example uses) is 9 — parity with native quanta, 1 PYDE = 10⁹.
void __init_impl(pyde_string name, pyde_string symbol, uint8_t decimals,
                 __uint128_t initial_supply, __uint128_t max_supply) {
    if (decimals > 18) {
        pyde_revert_str("token:invalid_decimals");
    }
    if (max_supply > 0 && initial_supply > max_supply) {
        pyde_revert_str("token:cap_exceeded");
    }
    pyde_address deployer = pyde_caller();

    token_name_set(name);
    token_symbol_set(symbol);
    token_decimals_set(decimals);
    total_supply_set(initial_supply);
    max_supply_set(max_supply);
    minter_set(deployer);
    manager_set(deployer);
    balances_set(deployer, initial_supply);

    // Mint sentinel: from = zero address. One event family carries all
    // supply accounting.
    emit_Transfer(ZERO_ADDRESS, deployer, initial_supply);
    emit_RoleTransfer(ROLE_MINTER, deployer, ZERO_ADDRESS);
    emit_RoleTransfer(ROLE_MANAGER, deployer, ZERO_ADDRESS);
}

// ─── Views ─────────────────────────────────────────────────────────────

// Runtime discovery handshake: which surface, which version.
pyde_string __standard_impl(void) {
    return pyde_str_lit(STANDARD, STANDARD_LEN);
}

pyde_string __name_impl(void) {
    return token_name_get();
}

pyde_string __symbol_impl(void) {
    return token_symbol_get();
}

uint8_t __decimals_impl(void) {
    return token_decimals_get();
}

__uint128_t __total_supply_impl(void) {
    return total_supply_get();
}

// 0 = uncapped.
__uint128_t __max_supply_impl(void) {
    return max_supply_get();
}

// A never-written slot reads as 0.
__uint128_t __balance_of_impl(pyde_address owner) {
    return balances_get(owner);
}

// Batched balance reads for routers/wallets — one call instead of an
// N-round-trip loop. Views are free off-chain, so the only bound is the
// anti-griefing cap.
pyde_vec_U128 __balance_of_batch_impl(pyde_vec_Address owners) {
    if (owners.len > MAX_BATCH) {
        pyde_revert_str("token:batch_too_large");
    }
    pyde_vec_U128 out;
    out.len = owners.len;
    out.ptr = (__uint128_t *)__pyde_alloc((owners.len == 0 ? 1u : owners.len) *
                                          (uint32_t)sizeof(__uint128_t));
    for (uint32_t i = 0; i < owners.len; i++) {
        out.ptr[i] = balances_get(owners.ptr[i]);
    }
    return out;
}

// Remaining spendable NOW: 0 once expired.
__uint128_t __allowance_impl(pyde_address owner, pyde_address spender) {
    return effective_allowance(owner, spender).amount;
}

// The raw expiry wave of the stored grant (0 = no grant).
uint64_t __allowance_expiry_impl(pyde_address owner, pyde_address spender) {
    return allowance_expiries_get(owner, spender);
}

// The whole metadata surface in one free call.
TokenInfo __token_info_impl(void) {
    TokenInfo info;
    info.name = token_name_get();
    info.symbol = token_symbol_get();
    info.decimals = token_decimals_get();
    info.total_supply = total_supply_get();
    info.max_supply = max_supply_get();
    info.minter = minter_get();
    info.extension_flags = EXTENSION_FLAGS;
    return info;
}

// Zero address = renounced.
pyde_address __minter_impl(void) {
    return minter_get();
}

pyde_address __manager_impl(void) {
    return manager_get();
}

// ─── Transfers ─────────────────────────────────────────────────────────

// Move amount from the caller to to. Writes exactly two balance slots;
// NEVER invokes code on to (mandatory hooks are the costliest exploit
// mechanism in token history — notification is the opt-in transfer_call
// path).
void __transfer_impl(pyde_address to, __uint128_t amount) {
    pyde_address from = pyde_caller();
    move_tokens(from, to, amount);
    emit_Transfer(from, to, amount);
}

// Settle-then-notify (PIP-0005 §6): (1) full balance settlement,
// (2) Transfer emission, (3) notify the recipient, which must return
// ACK_TOKEN. A recipient revert — or a missing/wrong acknowledgement,
// including a name-miss swallowed by a fallback — reverts the ENTIRE
// operation atomically (token:bad_receiver).
//
// This one atomic message replaces approve-then-pull for deposits: no
// standing authority is ever created.
uint32_t __transfer_call_impl(pyde_address to, __uint128_t amount, pyde_vec_U8 data) {
    if (data.len > MAX_CALL_DATA) {
        pyde_revert_str("token:data_too_large");
    }
    pyde_address operator = pyde_caller();

    // (1) settle
    move_tokens(operator, to, amount);
    // (2) emit — discarded with the frame if the notify leg reverts
    emit_Transfer(operator, to, amount);

    // (3) notify. Args are filled by the token — never spoofable by the
    // sender. The receiver frame sees caller() = this contract. Raw
    // cross_call: the generated dispatch only wraps this contract's own
    // exports, so the outbound notify is hand-marshalled.
    pyde_enc enc = pyde_enc_new();
    pyde_enc_address(&enc, operator);
    pyde_enc_address(&enc, operator);
    pyde_enc_u128(&enc, amount);
    __encodeVecU8(&enc, data);
    pyde_bytes calldata = pyde_enc_finish(&enc);

    uint8_t value[16] = {0}; // 16-byte LE u128 zero — no endowment
    uint8_t out[64];
    int32_t out_len = (int32_t)sizeof(out);
    int32_t rc = cross_call(to.bytes,
                            (const uint8_t *)"on_token_received", 17,
                            calldata.ptr, (int32_t)calldata.len,
                            value, FORWARD_ALL_GAS,
                            out, &out_len);
    if (rc == STATUS_OK && out_len >= 4) {
        pyde_dec dec = pyde_dec_new(out, (uint32_t)out_len);
        uint32_t ack = pyde_dec_u32(&dec);
        if (ack == ACK_TOKEN) {
            return ACK_TOKEN;
        }
    }
    // Wrong magic, decode mismatch, revert, no code at to, name-miss →
    // fallback (which cannot produce the ack): all collapse to the one
    // canonical rejection.
    pyde_revert_str("token:bad_receiver");
    return 0; // unreachable — revert is noreturn
}

// Spend a live allowance of (from, caller): expiry checked against the wave
// clock, amount decremented, balance moved. Emits Transfer only.
void __transfer_from_impl(pyde_address from, pyde_address to, __uint128_t amount) {
    pyde_address spender = pyde_caller();
    spend_allowance(from, spender, amount);
    move_tokens(from, to, amount);
    emit_Transfer(from, to, amount);
}

// ─── Delegated spending ────────────────────────────────────────────────

// Compatibility form: set the allowance to exactly amount with the maximum
// TTL auto-applied. "Unlimited forever" stays unrepresentable — even this
// grant self-destructs after ~1 year of waves.
void __approve_impl(pyde_address spender, __uint128_t amount) {
    pyde_address owner = pyde_caller();
    uint64_t expiry_wave = sat_add_u64(now_wave(), MAX_ALLOWANCE_TTL_WAVES);
    Allowance next = {amount, expiry_wave};
    write_allowance(owner, spender, next);
}

// Delta increase with an explicit expiry. Deltas kill the approve
// front-running race by construction. An expired grant contributes 0 to
// the new amount.
void __increase_allowance_impl(pyde_address spender, __uint128_t amount, uint64_t expiry_wave) {
    pyde_address owner = pyde_caller();
    uint64_t now = now_wave();
    if (expiry_wave <= now || expiry_wave > sat_add_u64(now, MAX_ALLOWANCE_TTL_WAVES)) {
        pyde_revert_str("token:invalid_expiry");
    }
    __uint128_t current = effective_allowance(owner, spender).amount;
    __uint128_t next_amount;
    if (!checked_add_u128(current, amount, &next_amount)) {
        pyde_revert_str("token:overflow");
    }
    Allowance next = {next_amount, expiry_wave};
    write_allowance(owner, spender, next);
}

// Delta decrease, floored at zero. Expiry is unchanged for a live grant;
// an expired grant collapses to {0, 0}.
void __decrease_allowance_impl(pyde_address spender, __uint128_t amount) {
    pyde_address owner = pyde_caller();
    Allowance current = effective_allowance(owner, spender);
    Allowance next;
    next.amount = sat_sub_u128(current.amount, amount);
    next.expiry_wave = (current.amount == 0) ? 0 : current.expiry_wave;
    write_allowance(owner, spender, next);
}

// Hard-zero the grant. Writes {0, 0} — never sdelete (no gas refunds
// exist; deletion is strictly costlier than zeroing).
void __revoke_allowance_impl(pyde_address spender) {
    pyde_address owner = pyde_caller();
    Allowance zero = {(__uint128_t)0, 0};
    write_allowance(owner, spender, zero);
}

// Compare-and-set: change the grant to new_remaining ONLY if the current
// effective remaining equals expected_remaining — closes the
// delta-accumulation footgun. Under commit-reveal the check can fail at
// reveal time if state moved; token:allowance_changed is a retriable
// condition, not corruption.
void __set_allowance_exact_impl(pyde_address spender, __uint128_t expected_remaining,
                                __uint128_t new_remaining, uint64_t expiry_wave) {
    pyde_address owner = pyde_caller();
    uint64_t now = now_wave();
    if (effective_allowance(owner, spender).amount != expected_remaining) {
        pyde_revert_str("token:allowance_changed");
    }
    if (new_remaining == 0) {
        Allowance zero = {(__uint128_t)0, 0};
        write_allowance(owner, spender, zero);
        return;
    }
    if (expiry_wave <= now || expiry_wave > sat_add_u64(now, MAX_ALLOWANCE_TTL_WAVES)) {
        pyde_revert_str("token:invalid_expiry");
    }
    Allowance next = {new_remaining, expiry_wave};
    write_allowance(owner, spender, next);
}

// ─── Supply & roles ────────────────────────────────────────────────────

// Minter only. Reverts above max_supply (when capped). Emits the
// zero-sentinel Transfer.
void __mint_impl(pyde_address to, __uint128_t amount) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, minter_get()) || pyde_addr_eq(caller, ZERO_ADDRESS)) {
        pyde_revert_str("token:not_minter");
    }
    guard_recipient(to);
    __uint128_t supply = total_supply_get();
    __uint128_t next_supply;
    if (!checked_add_u128(supply, amount, &next_supply)) {
        pyde_revert_str("token:overflow");
    }
    __uint128_t cap = max_supply_get();
    if (cap > 0 && next_supply > cap) {
        pyde_revert_str("token:cap_exceeded");
    }
    total_supply_set(next_supply);
    __uint128_t bal = balances_get(to);
    // Balance can't overflow if supply didn't: balance ≤ supply.
    balances_set(to, bal + amount);
    emit_Transfer(ZERO_ADDRESS, to, amount);
}

// Burn the caller's own tokens (default config is burnable). Emits the
// zero-sentinel Transfer.
void __burn_impl(__uint128_t amount) {
    pyde_address from = pyde_caller();
    burn_tokens(from, amount);
}

// Burn from from, spending a live allowance of (from, caller).
void __burn_from_impl(pyde_address from, __uint128_t amount) {
    pyde_address spender = pyde_caller();
    spend_allowance(from, spender, amount);
    burn_tokens(from, amount);
}

// Manager only. Zero address = provable renounce; a renounced minter can
// never be re-seated once the manager is also renounced.
void __set_minter_impl(pyde_address holder) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, manager_get()) || pyde_addr_eq(caller, ZERO_ADDRESS)) {
        pyde_revert_str("token:not_manager");
    }
    pyde_address previous = minter_get();
    minter_set(holder);
    emit_RoleTransfer(ROLE_MINTER, holder, previous);
}

// Manager only. Renouncing the manager (zero address) freezes role
// governance permanently.
void __set_manager_impl(pyde_address holder) {
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, manager_get()) || pyde_addr_eq(caller, ZERO_ADDRESS)) {
        pyde_revert_str("token:not_manager");
    }
    pyde_address previous = manager_get();
    manager_set(holder);
    emit_RoleTransfer(ROLE_MANAGER, holder, previous);
}
