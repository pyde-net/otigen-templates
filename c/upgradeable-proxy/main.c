// upgradeable-proxy — a thin proxy whose logic contract is admin-swappable.
//
// forward(fn, calldata) runs the logic contract's code in THIS contract's
// frame via a delegate-call: self_address stays the proxy, the caller is
// preserved, and the logic's storage accessors write to the PROXY's slots —
// so the shared `value` survives an upgrade. That state preservation is the
// whole point.
//
// Privileged fields are prefixed `proxy_` so a logic contract that declares
// its own `admin` / `logic` field can't accidentally clobber the proxy's
// slots (slots are Poseidon2(self_address ‖ field_name), shared under
// delegate-call). `value` stays unprefixed — it's the intended shared-state
// demonstration field.
//
// You write only the typed __<fn>_impl bodies below; `otigen build` reads
// otigen.toml and generates pyde_gen.h — the typed storage accessors, event
// emitters, borsh runtime, and () -> () entry-dispatch shims — around them.
// The raw host fn `delegate_call` is declared in include/pyde/host.h, in
// scope via pyde_gen.h.

#include "pyde_gen.h"

// zero_address — 32-byte all-zero address, sentinel for an unset / invalid
// address. Used to reject inputs that would permanently brick the proxy
// (zero admin = no one can ever upgrade; zero logic = every forward
// delegate-calls into a non-existent contract).
static const pyde_address ZERO_ADDRESS = {{0}};

// ── delegate-call plumbing (mirrors pyde-host's execute_delegate_raw) ──
//
// The canonical Rust wrapper sizes a fixed return buffer, forwards all
// remaining gas, then maps the ABI status code onto CallError. There's no
// per-language C SDK, so we call the raw `delegate_call` host fn directly and
// reproduce that mapping byte-for-byte.

// DEFAULT_RETURN_BUFFER_BYTES from pyde-host::call.
#define DEFAULT_RETURN_BUFFER_BYTES 4096u
// FORWARD_ALL_GAS = i64::MAX — the engine forwards min(gas, remaining), so
// i64::MAX is the canonical "use all remaining".
#define FORWARD_ALL_GAS ((int64_t)0x7fffffffffffffffLL)

// ABI status codes (HOST_FN_ABI_SPEC §4) as mapped by err_from_status.
#define STATUS_OK 0
#define ERR_INSUFFICIENT_BALANCE (-3)
#define ERR_REENTRANCY_BLOCKED (-9)
#define ERR_VALUE_TRANSFER_NOT_PAYABLE (-12)
#define ERR_INVALID_FUNCTION_NAME (-13)

// Strict UTF-8 validation — the C analogue of core::str::from_utf8, so the
// logic's revert payload is forwarded verbatim only when it's a valid string
// (rejects overlong encodings and UTF-16 surrogates, like Rust).
static bool utf8_valid(const uint8_t *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t b = s[i];
        if (b < 0x80u) {
            i += 1;
        } else if ((b & 0xE0u) == 0xC0u) {
            if (b < 0xC2u) {
                return false; // overlong
            }
            if (i + 1 >= n || (s[i + 1] & 0xC0u) != 0x80u) {
                return false;
            }
            i += 2;
        } else if ((b & 0xF0u) == 0xE0u) {
            if (i + 2 >= n) {
                return false;
            }
            uint8_t c1 = s[i + 1], c2 = s[i + 2];
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b == 0xE0u && c1 < 0xA0u) {
                return false; // overlong
            }
            if (b == 0xEDu && c1 > 0x9Fu) {
                return false; // UTF-16 surrogate
            }
            i += 3;
        } else if ((b & 0xF8u) == 0xF0u) {
            if (b > 0xF4u) {
                return false; // > U+10FFFF
            }
            if (i + 3 >= n) {
                return false;
            }
            uint8_t c1 = s[i + 1], c2 = s[i + 2], c3 = s[i + 3];
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
                return false;
            }
            if (b == 0xF0u && c1 < 0x90u) {
                return false; // overlong
            }
            if (b == 0xF4u && c1 > 0x8Fu) {
                return false; // > U+10FFFF
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

// ── Constructor ───────────────────────────────────────────────────────

// Init runs once at deploy (the [constructor]).
void __init_impl(pyde_address initial_logic) {
    // Defence-in-depth re-init guard. The manifest already tags this function
    // as ["constructor"] so the chain rejects post-deploy calls — but the
    // in-source flag catches accidental re-runs in tests + makes the invariant
    // explicit at the source level.
    if (!pyde_addr_eq(proxy_admin_get(), ZERO_ADDRESS)) {
        pyde_revert_str("proxy: already initialized");
    }
    pyde_address admin = pyde_caller();
    if (pyde_addr_eq(admin, ZERO_ADDRESS) || pyde_addr_eq(initial_logic, ZERO_ADDRESS)) {
        pyde_revert_str("proxy: init with zero address");
    }
    proxy_admin_set(admin);
    proxy_logic_set(initial_logic);
    emit_Initialized(admin, initial_logic);
}

// ── Admin operations ────────────────────────────────────────────────────

// upgrade_to is the admin-only logic-pointer swap. Doesn't touch `value` —
// the preserved state is the whole point.
void __upgrade_to_impl(pyde_address new_logic) {
    pyde_address admin = proxy_admin_get();
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, admin)) {
        pyde_revert_str("proxy: caller is not admin");
    }
    if (pyde_addr_eq(new_logic, ZERO_ADDRESS)) {
        pyde_revert_str("proxy: upgrade to zero address");
    }
    pyde_address old_logic = proxy_logic_get();
    proxy_logic_set(new_logic);
    emit_Upgraded(old_logic, new_logic);
}

// transfer_admin rotates the admin role. Admin-only. Reverts on a
// zero-address new admin — use renounce_admin for that path so the
// irrevocable lock is loud rather than disguised as a transfer.
void __transfer_admin_impl(pyde_address new_admin) {
    pyde_address admin = proxy_admin_get();
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, admin)) {
        pyde_revert_str("proxy: caller is not admin");
    }
    if (pyde_addr_eq(new_admin, ZERO_ADDRESS)) {
        pyde_revert_str("proxy: transfer to zero address; use renounce_admin");
    }
    pyde_address old_admin = admin;
    proxy_admin_set(new_admin);
    emit_AdminTransferred(old_admin, new_admin);
}

// renounce_admin renounces the admin role — sets the admin slot to the zero
// address. After this call NO ONE can upgrade_to or call any admin-gated
// entry; the logic pointer is frozen at its current value forever.
// Irreversible.
void __renounce_admin_impl(void) {
    pyde_address admin = proxy_admin_get();
    pyde_address caller = pyde_caller();
    if (!pyde_addr_eq(caller, admin)) {
        pyde_revert_str("proxy: caller is not admin");
    }
    proxy_admin_set(ZERO_ADDRESS);
    emit_AdminTransferred(admin, ZERO_ADDRESS);
}

// ── Dispatcher ────────────────────────────────────────────────────────

// forward delegate-calls logic.function(calldata) in this contract's frame.
// Returns the raw bytes the logic produced as its return payload — the caller
// decodes them per the function's documented return type.
//
// Uses the raw `delegate_call` host fn (not a typed decode) because the proxy
// is a type-erased forwarder — it doesn't know the logic's return shape and
// can't borsh-decode it. The logic's bytes are handed back verbatim.
pyde_bytes __forward_impl(pyde_string function, pyde_bytes calldata) {
    pyde_address logic = proxy_logic_get();

    // Fixed return buffer + forward-all gas, matching execute_delegate_raw.
    // The host writes the actual return length back into out_len regardless
    // of success/failure (the in/out length convention).
    uint8_t *buf = (uint8_t *)__pyde_alloc(DEFAULT_RETURN_BUFFER_BYTES);
    int32_t out_len = (int32_t)DEFAULT_RETURN_BUFFER_BYTES;
    int32_t status = delegate_call(
        logic.bytes,
        function.ptr, (int32_t)function.len,
        calldata.ptr, (int32_t)calldata.len,
        FORWARD_ALL_GAS,
        buf, &out_len);

    uint32_t actual = out_len < 0 ? 0u : (uint32_t)out_len;

    if (status == STATUS_OK) {
        // A success status with a length past the buffer is Rust's
        // CallError::ReturnDataTooLarge → the Err(_) generic path.
        if (actual > DEFAULT_RETURN_BUFFER_BYTES) {
            pyde_revert_str("proxy: delegate-call failed");
        }
        pyde_bytes r;
        r.ptr = buf;
        r.len = actual;
        return r;
    }

    // Err(CallError::InvalidFunction).
    if (status == ERR_INVALID_FUNCTION_NAME) {
        pyde_revert_str("proxy: logic has no such function");
    }
    // Err(_) over the non-Reverted call-error variants.
    if (status == ERR_INSUFFICIENT_BALANCE ||
        status == ERR_REENTRANCY_BLOCKED ||
        status == ERR_VALUE_TRANSFER_NOT_PAYABLE) {
        pyde_revert_str("proxy: delegate-call failed");
    }
    // CallError::Reverted(payload): the chain wrote the logic's revert payload
    // into our buffer. Pass its revert string straight through to the proxy's
    // caller so they see exactly what the logic said, not a generic "proxy
    // failed" message. Falls back to the generic message when the payload
    // isn't valid UTF-8.
    uint32_t copy = actual < DEFAULT_RETURN_BUFFER_BYTES ? actual : DEFAULT_RETURN_BUFFER_BYTES;
    if (utf8_valid(buf, copy)) {
        revert(buf, (int32_t)copy);
    }
    pyde_revert_str("proxy: delegate-call failed");
}

// ── Views ─────────────────────────────────────────────────────────────

// get_admin returns the current admin (zero if renounced).
pyde_address __get_admin_impl(void) {
    return proxy_admin_get();
}

// get_logic returns the current logic pointer.
pyde_address __get_logic_impl(void) {
    return proxy_logic_get();
}

// get_value reads the shared `value` slot the logic contract writes through
// the proxy.
uint64_t __get_value_impl(void) {
    return value_get();
}
