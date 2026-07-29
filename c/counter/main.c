// `pyde-template` — minimal Pyde contract scaffolded by `otigen init --lang c`.
//
// Demonstrates the four fundamentals every Pyde C contract needs:
//   1. Host-fn imports via `__attribute__((import_module("pyde"),
//      import_name("...")))` (declared in `include/pyde/host.h`).
//   2. Variable-length storage via `sload` / `sstore`:
//      the contract derives `Poseidon2(self_address || field || key)`
//      itself in the `derive_slot` helper below, then reads or writes
//      the exact byte width it needs (8 for uint64, 16 for uint128).
//   3. Public entry points marked
//      `__attribute__((export_name("...")))` and registered in
//      `otigen.toml` under `[functions.<name>]`.
//   4. Manual u64 little-endian codec — pure C, no libc, just byte ops.
//
// `-nostdlib` keeps libc out of the resulting wasm so Pyde's import
// allowlist accepts the binary. For memcpy / memset we use Clang's
// `__builtin_*` intrinsics which lower to inline WASM ops.
//
// Bigger surfaces (events, balance transfers, cross-contract calls)
// live in the canonical examples at `pyde-net/otigen/examples/`.

#include <stdint.h>
#include "pyde/host.h"

// ─────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────

// Storage field name for the counter. The contract derives the slot
// itself via `derive_slot(FIELD_COUNTER, ...)`:
//
//   slot = Poseidon2(self_address || FIELD_COUNTER)
//
// Field names are arbitrary — they live entirely inside the slot
// hash and never appear on-chain literally.
static const uint8_t FIELD_COUNTER[7] = {'c', 'o', 'u', 'n', 't', 'e', 'r'};

// ─────────────────────────────────────────────────────────────────────
// u64 storage codec
// ─────────────────────────────────────────────────────────────────────
//
// Pyde storage is variable-length (HOST_FN_ABI_SPEC §7.1). A uint64
// is stored as exactly 8 bytes — no 32-byte padding. The slot key is
// derived via Pyde's canonical recipe:
//
//   slot = Poseidon2(self_address || field [|| key])
//
// Pass NULL/0 for the key on scalar slots.

static void derive_slot(const uint8_t* field, int32_t field_len,
                        const uint8_t* key,   int32_t key_len,
                        uint8_t out[32]) {
    // Stack-allocated preimage caps at 32 + 96 = 128 bytes — plenty
    // for typical field-name + composite-key pairs.
    uint8_t preimage[128];
    self_address(preimage);                          // 32B
    for (int32_t i = 0; i < field_len; i++) {
        preimage[32 + i] = field[i];
    }
    for (int32_t i = 0; i < key_len; i++) {
        preimage[32 + field_len + i] = key[i];
    }
    hash_poseidon2(preimage, 32 + field_len + key_len, out);
}

static uint64_t read_counter(void) {
    uint8_t slot[32];
    derive_slot(FIELD_COUNTER, sizeof FIELD_COUNTER, (const uint8_t*)0, 0, slot);

    uint8_t buf[8];
    int32_t actual = sload(slot, buf, 8);
    // -1 (missing) and 0 (empty) both mean "default to zero".
    if (actual <= 0) {
        return 0;
    }
    // u64 LE decode — canonical Pyde wire encoding per
    // HOST_FN_ABI_SPEC §7.1 (matches borsh + substrate macros).
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | (uint64_t)buf[i];
    }
    return v;
}

static void write_counter(uint64_t value) {
    uint8_t slot[32];
    derive_slot(FIELD_COUNTER, sizeof FIELD_COUNTER, (const uint8_t*)0, 0, slot);

    uint8_t buf[8];
    // u64 LE encode — buf[0] is the lowest 8 bits.
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(value & 0xff);
        value >>= 8;
    }
    sstore(slot, buf, 8);
}

// ─────────────────────────────────────────────────────────────────────
// Public entry points (declared in otigen.toml `[functions.*]`)
// ─────────────────────────────────────────────────────────────────────

// emit_u64 marshals a uint64 to the chain's return-data wire format:
// 8 bytes little-endian (matching borsh's u64 layout, the canonical
// Pyde wire encoding for integer returns).
//
// `pyde_return` is declared `noreturn` in `include/pyde/host.h` so clang
// knows the host fn never resumes — no post-call epilogue gets
// emitted that would otherwise trap.
static void emit_u64(uint64_t value) {
    uint8_t buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(value & 0xff);
        value >>= 8;
    }
    pyde_return(buf, 8);
}

// increment increments the counter and returns the new value.
//
// Attributes: `entry` (callable from outside the contract).
// Gas: roughly one sload + one sstore + one hash_poseidon2 + arithmetic.
//
// Pyde entry points have the WASM signature `() -> ()` — args come
// in via `calldata_*` host fns (none for this counter) and the
// return value goes out via `pyde_return(buf, len)`. The chain's
// deploy validator rejects any other shape.
//
// `export_name("increment")` tells clang's wasm-ld to expose this
// symbol under the exact name the chain dispatcher looks up.
__attribute__((export_name("increment")))
void increment(void) {
    uint64_t next = read_counter() + 1;
    write_counter(next);
    emit_u64(next);
}

// get reads the current counter without mutating.
//
// Attributes: `entry`, `view` (must not modify state, transfer value,
// or emit events — enforced by the engine).
__attribute__((export_name("get")))
void get(void) {
    emit_u64(read_counter());
}
