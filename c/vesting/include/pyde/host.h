// Canonical Pyde host fn declarations — the entire pyde::* ABI a C
// contract can call into. Every fn here is declared in
// HOST_FN_ABI_SPEC §7
// (https://book.pyde.network/companion/HOST_FN_ABI_SPEC).
//
// Why this file is comprehensive: the otigen toolchain doesn't ship a
// per-language SDK. Authors declare imports directly via
// `__attribute__((import_module(...), import_name(...)))`. Declaring
// every fn once here means you never have to copy-paste a signature
// from the spec.
//
// Pointer convention: every `const uint8_t*` / `uint8_t*` parameter
// is a 32-bit offset into linear memory (wasm32 pointers are 32 bits).
// Multi-byte integers cross the boundary in little-endian unless the
// spec explicitly says otherwise.
//
// Unused declarations don't cost anything — wasm-ld emits an import
// only when something references the function. Safe to keep them all.

#ifndef PYDE_HOST_H
#define PYDE_HOST_H

#include <stdint.h>

// C++ compilers apply name mangling to declarations by default; the
// WASM host-import table matches by unmangled symbol name, so C++
// code needs `extern "C"` linkage on every host fn declaration below.
// The guard is a no-op for C compilers.
#ifdef __cplusplus
extern "C" {
#endif

// All pyde::* imports go through this macro for consistency.
#define PYDE_HOST_FN(name) \
    __attribute__((import_module("pyde"), import_name(#name)))

// ─────────────────────────────────────────────────────────────────────
// §7.1 Storage
// ─────────────────────────────────────────────────────────────────────

// Storage values are variable-length (capped at 16 KB per slot).
// Slot keys are always 32 bytes — derive them with hash_poseidon2
// using the canonical recipe `slot = Poseidon2(self_address || field
// [|| key])`.

PYDE_HOST_FN(sload)
extern int32_t sload(const uint8_t* slot_ptr, uint8_t* out_ptr, int32_t out_max_len);

PYDE_HOST_FN(sstore)
extern void sstore(const uint8_t* slot_ptr, const uint8_t* val_ptr, int32_t val_len);

PYDE_HOST_FN(sdelete)
extern void sdelete(const uint8_t* slot_ptr);

// ── Typed storage (schema-derived slots) ─────────────────────────────
//
// The typed-storage family lets the host derive slot keys from the
// declared field name (+ optional keys), so the contract never has to
// call hash_poseidon2 itself. The host also validates the value's
// byte length against the field's declared type. Gas: sstore/sload
// base + a small surcharge for schema lookup + type validation.

PYDE_HOST_FN(sstore_scalar)
extern int32_t sstore_scalar(const uint8_t* field_ptr, int32_t field_len, const uint8_t* value_ptr, int32_t value_len);

PYDE_HOST_FN(sload_scalar)
extern int32_t sload_scalar(const uint8_t* field_ptr, int32_t field_len, uint8_t* out_ptr, int32_t out_max_len);

PYDE_HOST_FN(sdelete_scalar)
extern int32_t sdelete_scalar(const uint8_t* field_ptr, int32_t field_len);

PYDE_HOST_FN(sstore_map1)
extern int32_t sstore_map1(const uint8_t* field_ptr, int32_t field_len, const uint8_t* key_ptr, int32_t key_len, const uint8_t* value_ptr, int32_t value_len);

PYDE_HOST_FN(sload_map1)
extern int32_t sload_map1(const uint8_t* field_ptr, int32_t field_len, const uint8_t* key_ptr, int32_t key_len, uint8_t* out_ptr, int32_t out_max_len);

PYDE_HOST_FN(sdelete_map1)
extern int32_t sdelete_map1(const uint8_t* field_ptr, int32_t field_len, const uint8_t* key_ptr, int32_t key_len);

PYDE_HOST_FN(sstore_map2)
extern int32_t sstore_map2(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len, const uint8_t* value_ptr, int32_t value_len);

PYDE_HOST_FN(sload_map2)
extern int32_t sload_map2(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len, uint8_t* out_ptr, int32_t out_max_len);

PYDE_HOST_FN(sdelete_map2)
extern int32_t sdelete_map2(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len);

PYDE_HOST_FN(sstore_map3)
extern int32_t sstore_map3(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len, const uint8_t* k3_ptr, int32_t k3_len, const uint8_t* value_ptr, int32_t value_len);

PYDE_HOST_FN(sload_map3)
extern int32_t sload_map3(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len, const uint8_t* k3_ptr, int32_t k3_len, uint8_t* out_ptr, int32_t out_max_len);

PYDE_HOST_FN(sdelete_map3)
extern int32_t sdelete_map3(const uint8_t* field_ptr, int32_t field_len, const uint8_t* k1_ptr, int32_t k1_len, const uint8_t* k2_ptr, int32_t k2_len, const uint8_t* k3_ptr, int32_t k3_len);

// ─────────────────────────────────────────────────────────────────────
// §7.2 Account & balance
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(balance)
extern void balance(const uint8_t* addr_ptr, uint8_t* balance_out_ptr);

PYDE_HOST_FN(transfer)
extern int32_t transfer(const uint8_t* to_ptr, const uint8_t* amount_ptr);

// ─────────────────────────────────────────────────────────────────────
// §7.3 Execution context
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(caller)
extern int32_t caller(uint8_t* addr_out_ptr);

PYDE_HOST_FN(origin)
extern int32_t origin(uint8_t* addr_out_ptr);

PYDE_HOST_FN(self_address)
extern int32_t self_address(uint8_t* addr_out_ptr);

PYDE_HOST_FN(wave_id)
extern int64_t wave_id(void);

PYDE_HOST_FN(wave_timestamp)
extern int64_t wave_timestamp(void);

PYDE_HOST_FN(chain_id)
extern int64_t chain_id(void);

// ─────────────────────────────────────────────────────────────────────
// §7.4 Transaction context
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(tx_hash)
extern void tx_hash(uint8_t* hash_out_ptr);

PYDE_HOST_FN(tx_value)
extern void tx_value(uint8_t* value_out_ptr);

PYDE_HOST_FN(tx_gas_remaining)
extern int64_t tx_gas_remaining(void);

PYDE_HOST_FN(calldata_size)
extern int32_t calldata_size(void);

// calldata_copy uses the in/out length convention: *out_len_ptr
// holds the max bytes the contract will accept on call, and the
// actual bytes copied on return. The older 3-arg signature
// (offset/len/out_ptr) is rejected by the engine's import
// validator — the host fn never took an offset.
PYDE_HOST_FN(calldata_copy)
extern int32_t calldata_copy(uint8_t* out_ptr, uint32_t* out_len_ptr);

// ─────────────────────────────────────────────────────────────────────
// §7.5 Events
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(emit_event)
extern int32_t emit_event(
    const uint8_t* topics_ptr,
    int32_t topics_count,
    const uint8_t* data_ptr,
    int32_t data_len);

// ─────────────────────────────────────────────────────────────────────
// §7.6 Hashing primitives
// ─────────────────────────────────────────────────────────────────────
//
// NB: the hash host fns in the current test runner have no return
// value (void). HOST_FN_ABI_SPEC §7.6 says they return `int32_t`
// ("Returns: 0 always."). Template matches the runner so `otigen
// test` instantiates the contract; the spec / runner divergence is
// tracked for follow-up.

PYDE_HOST_FN(hash_blake3)
extern void hash_blake3(const uint8_t* in_ptr, int32_t in_len, uint8_t* out_ptr);

PYDE_HOST_FN(hash_poseidon2)
extern void hash_poseidon2(const uint8_t* in_ptr, int32_t in_len, uint8_t* out_ptr);

PYDE_HOST_FN(hash_keccak256)
extern void hash_keccak256(const uint8_t* in_ptr, int32_t in_len, uint8_t* out_ptr);

// ─────────────────────────────────────────────────────────────────────
// §7.7 Post-quantum cryptography
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(falcon_verify)
extern int32_t falcon_verify(
    const uint8_t* pk_ptr,
    const uint8_t* msg_ptr, int32_t msg_len,
    const uint8_t* sig_ptr, int32_t sig_len);

// ─────────────────────────────────────────────────────────────────────
// §7.8 Cross-contract calls
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(cross_call)
extern int32_t cross_call(
    const uint8_t* target_ptr,
    const uint8_t* fn_name_ptr, int32_t fn_name_len,
    const uint8_t* calldata_ptr, int32_t calldata_len,
    const uint8_t* value_ptr,
    int64_t gas_limit,
    uint8_t* return_data_out_ptr,
    int32_t* return_data_out_len_ptr);

PYDE_HOST_FN(cross_call_static)
extern int32_t cross_call_static(
    const uint8_t* target_ptr,
    const uint8_t* fn_name_ptr, int32_t fn_name_len,
    const uint8_t* calldata_ptr, int32_t calldata_len,
    int64_t gas_limit,
    uint8_t* return_data_out_ptr,
    int32_t* return_data_out_len_ptr);

PYDE_HOST_FN(delegate_call)
extern int32_t delegate_call(
    const uint8_t* target_ptr,
    const uint8_t* fn_name_ptr, int32_t fn_name_len,
    const uint8_t* calldata_ptr, int32_t calldata_len,
    int64_t gas_limit,
    uint8_t* return_data_out_ptr,
    int32_t* return_data_out_len_ptr);

// ─────────────────────────────────────────────────────────────────────
// §7.9 Halt operations
// ─────────────────────────────────────────────────────────────────────

// `return` is a C keyword; the wire name uses `return` but the C
// identifier is `pyde_return` (matches Rust + Go templates).
__attribute__((import_module("pyde"), import_name("return"), noreturn))
extern void pyde_return(const uint8_t* data_ptr, int32_t data_len);

__attribute__((import_module("pyde"), import_name("revert"), noreturn))
extern void revert(const uint8_t* reason_ptr, int32_t reason_len);

// ─────────────────────────────────────────────────────────────────────
// §7.10 Explicit gas metering
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(consume_gas)
extern void consume_gas(int64_t amount);

// ─────────────────────────────────────────────────────────────────────
// §7.11 VRF beacon
// ─────────────────────────────────────────────────────────────────────

PYDE_HOST_FN(beacon_get)
extern int32_t beacon_get(uint8_t* out_ptr);

/* ─────────────────────────────────────────────────────────────────────
 * §7.12 Factory instantiation
 * ─────────────────────────────────────────────────────────────────────
 *
 * Create a child instance of the DEPLOYED template contract at
 * template_addr_ptr (32 bytes), addressed by
 * child_address(self, template, salt) — by reference: the child
 * shares the template's cached code. salt_ptr → 32 opaque
 * caller-derived bytes; init_* → borsh ctor args (≤ 16384 bytes;
 * zero for ctor-less templates); value_ptr → 16-byte LE u128
 * endowment; gas_limit < 0 = forward all remaining.
 *
 * child_addr_out_ptr (32 bytes) is written on every path past the
 * early cap/bounds checks (0, -40, -43, -44, -45, -46, -3 — NOT
 * -48). Return data carries the ctor's return value on 0 and its
 * revert payload VERBATIM on -40. Codes: -40 ctor reverted (ATOMIC
 * refund); -43 template not a contract; -44 occupied by a
 * NON-mergeable account; -45 nonempty init on a ctor-less template;
 * -46 PIP-2 prefix collision; -48 per-tx cap (64); -3 balance <
 * endowment. Traps from view/static frames and at depth >= 1024.
 *
 * gas: 20000 base + 8 per init byte + the ctor's own gas.
 */
PYDE_HOST_FN(instantiate)
extern int32_t instantiate(
    const uint8_t* template_addr_ptr,
    const uint8_t* salt_ptr,
    const uint8_t* init_calldata_ptr, int32_t init_calldata_len,
    const uint8_t* value_ptr,
    int64_t gas_limit,
    uint8_t* child_addr_out_ptr,
    uint8_t* return_data_out_ptr,
    uint32_t* return_data_out_len_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif // PYDE_HOST_H
