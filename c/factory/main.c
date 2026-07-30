// factory — mint and drive child contracts via the `instantiate` host fn.
//
// A factory creates fresh instances of a TEMPLATE — and the template is
// just ANY contract you have already deployed. Deploy a contract, copy
// its address, and pass that address to `create`. Each child is a
// first-class contract — its own address, its own isolated storage —
// sharing the template's already-cached code, so nothing is copied or
// recompiled.
//
// The template is instantiated with NO constructor arguments, so it
// works with any contract that has no required constructor (the
// built-in `counter` template is a perfect fit). To drive a child,
// this factory cross-calls its `increment() -> u64`, so the template
// should expose that.
//
// A child's address is a pure function of (factory, template, salt),
// so it is predictable off-chain before the child exists. This example
// shows the THREE ways to choose that salt and the THREE ways to then
// reach a child:
//
//   CREATE — choose the salt
//     * create          — AUTO. The factory keeps a `next_key` counter
//                          and salts each child with it, so a caller
//                          manages no salt at all.
//     * create_with_key — an explicit u64 salt (a user id, a nonce).
//     * create_named    — a string salt (a name, a market pair).
//
//   INTERACT — reach a child
//     * bump       — BY u64 KEY: look it up in the registry, call it.
//     * bump_named — BY STRING NAME: look it up, call it.
//     * bump_at    — BY ADDRESS: call any child directly, no lookup.
//
// You write only the typed __<fn>_impl bodies below; `otigen build` reads
// otigen.toml and generates pyde_gen.h — the typed storage accessors,
// borsh runtime, and () -> () entry-dispatch shims — around them. The
// factory `instantiate` and the child `cross_call` are raw host fns
// (declared in include/pyde/host.h, in scope via pyde_gen.h),
// hand-marshalled here.

#include "pyde_gen.h"

// ─── raw host-fn conventions ───────────────────────────────────────────

// DEFAULT_RETURN_BUFFER_BYTES — the return/revert-payload buffer size the
// Rust/Go wrappers use for both cross_call and instantiate.
#define RETURN_BUFFER_BYTES 4096u

// cross_call's "forward all remaining" sentinel: the engine forwards
// min(gas, remaining), so a value far above any real budget forwards all.
#define FORWARD_ALL_GAS 0x7fffffffffffffffLL // i64::MAX

// instantiate's "forward all remaining" sentinel: UNLIKE cross_call, a
// NEGATIVE gas limit means forward all (HOST_FN_ABI_SPEC §7.12).
#define FORWARD_ALL_CTOR_GAS (-1LL)

// ─── internal helpers (forward decls) ───────────────────────────────────

static pyde_bytes32 salt_of_u64(uint64_t key);
static pyde_bytes32 salt_of_name(pyde_string name);
static pyde_address mint(pyde_address template, pyde_bytes32 salt);
static uint64_t increment(pyde_address child);
static void record(void);
static bool is_valid_utf8(const uint8_t *s, uint32_t n);

// ─── create ─────────────────────────────────────────────────────────────

// create(address) -> address — mint a child with an AUTO salt: the
// factory's `next_key` counter. The caller manages nothing: each call
// takes the next slot. The child is recorded in the u64 registry and
// returned.
pyde_address __create_impl(pyde_address template) {
    uint64_t key = next_key_get();
    pyde_address child = mint(template, salt_of_u64(key));
    children_set(key, child);
    next_key_set(key + 1);
    record();
    return child;
}

// create_with_key(address, uint64) -> address — mint a child at an
// explicit u64 salt key (e.g. a user id). The address is
// (factory, template, Salt::of(key)), so the same key always targets the
// same child — a second call reverts "exists".
pyde_address __create_with_key_impl(pyde_address template, uint64_t key) {
    pyde_address child = mint(template, salt_of_u64(key));
    children_set(key, child);
    record();
    return child;
}

// create_named(address, string) -> address — mint a child at a string
// salt (e.g. a market pair "ETH/USDC"). salt_of_name borsh-encodes the
// string and hashes it, so any string is a valid, deterministic identity.
// Recorded in the name registry.
pyde_address __create_named_impl(pyde_address template, pyde_string name) {
    pyde_address child = mint(template, salt_of_name(name));
    named_set(name, child);
    record();
    return child;
}

// ─── look up ─────────────────────────────────────────────────────────────

// child_of(uint64) -> address — a child by its u64 key (the factory's
// getPair). Zero address if that key was never created.
pyde_address __child_of_impl(uint64_t key) {
    return children_get(key);
}

// child_of_name(string) -> address — a child by its string name. Zero
// address if never created.
pyde_address __child_of_name_impl(pyde_string name) {
    return named_get(name);
}

// next_key() -> uint64 — the key `create` will use next.
uint64_t __next_key_impl(void) {
    return next_key_get();
}

// created() -> uint64 — total children this factory has minted.
uint64_t __created_impl(void) {
    return created_get();
}

// ─── interact ─────────────────────────────────────────────────────────────

// bump(uint64) -> uint64 — drive a child BY u64 KEY: pull it from the
// registry, then call its `increment` via a cross-call. Returns the
// child's new value.
uint64_t __bump_impl(uint64_t key) {
    return increment(children_get(key));
}

// bump_named(string) -> uint64 — drive a child BY STRING NAME.
uint64_t __bump_named_impl(pyde_string name) {
    return increment(named_get(name));
}

// bump_at(address) -> uint64 — drive a child BY ADDRESS, no lookup. This
// is how a contract talks to a contract it (or anyone) created: by
// address, sharing the template's ABI, no typed handle required.
uint64_t __bump_at_impl(pyde_address child) {
    return increment(child);
}

// ─── internals ─────────────────────────────────────────────────────────────

// salt_of_u64 is the C equivalent of pyde::Salt::of(&key): Poseidon2 of the
// borsh encoding of the u64 (8 little-endian bytes).
static pyde_bytes32 salt_of_u64(uint64_t key) {
    pyde_enc __e = pyde_enc_new();
    pyde_enc_u64(&__e, key);
    pyde_bytes __b = pyde_enc_finish(&__e);
    pyde_bytes32 out;
    hash_poseidon2(__b.ptr, (int32_t)__b.len, out.bytes);
    return out;
}

// salt_of_name is the C equivalent of pyde::Salt::of(&name): Poseidon2 of
// the borsh encoding of the string (u32 length prefix + UTF-8).
static pyde_bytes32 salt_of_name(pyde_string name) {
    pyde_enc __e = pyde_enc_new();
    pyde_enc_string(&__e, name);
    pyde_bytes __b = pyde_enc_finish(&__e);
    pyde_bytes32 out;
    hash_poseidon2(__b.ptr, (int32_t)__b.len, out.bytes);
    return out;
}

// mint instantiates `template` at `salt` with NO constructor args — works
// with any contract that has no required constructor. Surfaces the engine's
// outcomes as clean reverts: a repeat identity is "exists" (-44), an unknown
// template is "template-not-found" (-43), and any other failure bubbles the
// child constructor's revert message when present, mirroring Rust's
// e.revert_message().unwrap_or("instantiate-failed").
static pyde_address mint(pyde_address template, pyde_bytes32 salt) {
    pyde_address child = {{0}};
    uint8_t *retbuf = (uint8_t *)__pyde_alloc(RETURN_BUFFER_BYTES);
    uint32_t retlen = RETURN_BUFFER_BYTES;

    // No endowment (16 zero LE bytes), forward all remaining gas.
    uint8_t value16[16] = {0};
    int32_t status = instantiate(
        template.bytes,
        salt.bytes,
        (const uint8_t *)0, 0, // no constructor args
        value16,
        FORWARD_ALL_CTOR_GAS,
        child.bytes,
        retbuf,
        &retlen);

    if (status == 0) {
        return child;
    }
    if (status == -44) {
        pyde_revert_str("exists");
    }
    if (status == -43) {
        pyde_revert_str("template-not-found");
    }
    // Bubble the child constructor's revert message when present (on -40 the
    // engine copies the ctor's revert payload into retbuf; retlen is its
    // length), mirroring Rust's e.revert_message().unwrap_or(...).
    if (retlen > 0 && is_valid_utf8(retbuf, retlen)) {
        revert(retbuf, (int32_t)retlen);
    }
    pyde_revert_str("instantiate-failed");
}

// increment cross-calls a child's `increment` (no args) and returns the new
// value. Forwards the callee's revert message when present, mirroring Rust's
// e.revert_message().unwrap_or("child-call-failed").
static uint64_t increment(pyde_address child) {
    uint8_t *retbuf = (uint8_t *)__pyde_alloc(RETURN_BUFFER_BYTES);
    int32_t retlen = (int32_t)RETURN_BUFFER_BYTES;

    // No value attached (16 zero LE bytes), forward all remaining gas.
    uint8_t value16[16] = {0};
    int32_t status = cross_call(
        child.bytes,
        (const uint8_t *)"increment", 9,
        (const uint8_t *)0, 0, // no args
        value16,
        FORWARD_ALL_GAS,
        retbuf,
        &retlen);

    if (status != 0) {
        if (retlen > 0 && is_valid_utf8(retbuf, (uint32_t)retlen)) {
            revert(retbuf, retlen);
        }
        pyde_revert_str("child-call-failed");
    }
    pyde_dec __d = pyde_dec_new(retbuf, (uint32_t)retlen);
    return pyde_dec_u64(&__d);
}

// record ticks the minted-children counter.
static void record(void) {
    uint64_t n = created_get();
    created_set(n + 1);
}

// is_valid_utf8 mirrors Go's utf8.Valid / Rust's String::from_utf8().ok():
// only a well-formed UTF-8 payload is bubbled verbatim as a revert reason;
// anything else falls back to the canonical default string. Rejects overlong
// encodings, surrogate halves, and code points above U+10FFFF.
static bool is_valid_utf8(const uint8_t *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80u) {
            i += 1;
            continue;
        }
        uint32_t extra;
        uint8_t lo = 0x80u, hi = 0xBFu; // default continuation-byte range
        if ((c & 0xE0u) == 0xC0u) {
            if (c < 0xC2u) {
                return false; // overlong 2-byte
            }
            extra = 1;
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2;
            if (c == 0xE0u) {
                lo = 0xA0u; // exclude overlong
            } else if (c == 0xEDu) {
                hi = 0x9Fu; // exclude surrogates
            }
        } else if ((c & 0xF8u) == 0xF0u) {
            if (c > 0xF4u) {
                return false; // > U+10FFFF
            }
            extra = 3;
            if (c == 0xF0u) {
                lo = 0x90u; // exclude overlong
            } else if (c == 0xF4u) {
                hi = 0x8Fu; // exclude > U+10FFFF
            }
        } else {
            return false; // lone continuation byte or 0xF8..0xFF
        }
        if (i + extra >= n) {
            return false; // truncated multi-byte sequence
        }
        uint8_t b1 = s[i + 1];
        if (b1 < lo || b1 > hi) {
            return false;
        }
        for (uint32_t j = 2; j <= extra; j++) {
            uint8_t bj = s[i + j];
            if (bj < 0x80u || bj > 0xBFu) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}
