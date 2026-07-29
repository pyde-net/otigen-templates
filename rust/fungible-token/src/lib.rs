//! `fungible-token` — the PTS-F reference implementation (`pts-f/1`,
//! PIP-0005), default extension configuration.
//!
//! What this surface fixes, each deviation pointing at a documented
//! loss class on other chains:
//!
//!   - **Revert-only mutations.** No boolean returns to mis-handle;
//!     failures are canonical machine-readable `token:*` codes that
//!     propagate verbatim through `cross_call` unwinds.
//!   - **Expiring, delta-only allowances.** Every grant carries a
//!     mandatory expiry wave (TTL-capped at ~1 year); increase/decrease
//!     deltas replace raw overwrites, so the approve race is dead by
//!     construction and "unlimited forever" is unrepresentable.
//!   - **Settle-then-notify deposits.** `transfer_call` writes balances
//!     and emits `Transfer` FIRST, then notifies the recipient, which
//!     must return the `ACK_TOKEN` acknowledgement — a name-miss
//!     falling through to a fallback cannot silently swallow tokens.
//!     Plain `transfer` never invokes recipient code.
//!   - **Consent-visible control.** Minter/manager roles are declared
//!     state, rotated by the manager, renounced by provable zeroing.
//!   - **Parallel-execution-ready layout.** A transfer writes exactly
//!     the two parties' balance slots; `total_supply` is written only
//!     by mint/burn. Disjoint transfers commute under Block-STM.
//!
//! Storage economics: balances and allowances are zeroed, never
//! deleted — the chain has no gas refunds, so `sdelete` is strictly
//! costlier than writing zero.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::panic::PanicInfo;

use borsh::{to_vec, BorshDeserialize, BorshSerialize};
use pyde_host as pyde;
use pyde::Address;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

pyde::declare_storage!();
pyde::declare_events!();

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

/// The surface this contract conforms to.
const STANDARD: &str = "pts-f/1";

/// Hard cap on allowance lifetime: ~1 year at 500 ms/wave. Even a
/// maximal grant self-destructs.
const MAX_ALLOWANCE_TTL_WAVES: u64 = 63_072_000;

/// Max `owners` accepted by `balance_of_batch`.
const MAX_BATCH: usize = 256;

/// Max bytes of `data` forwarded by `transfer_call` — bounds
/// payload-driven gas griefing under no-refund economics.
const MAX_CALL_DATA: usize = 4_096;

/// Required return of a conformant receiver:
/// `u32::from_le_bytes(Blake3("pts/on_token_received/1")[0..4])`.
const ACK_TOKEN: u32 = 2_266_754_145; // LE bytes 61 ec 1b 87

/// 32-byte role identifiers carried by `RoleTransfer` (PIP-0005 §3):
/// `Blake3("minter")` / `Blake3("manager")`.
const ROLE_MINTER: [u8; 32] = [
    0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
    0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
    0xe5, 0xdd,
];
const ROLE_MANAGER: [u8; 32] = [
    0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
    0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
    0x35, 0xc1,
];

/// Extension flags reported by `token_info()` (PIP-0005 §5): this is
/// the default configuration — bit 4 (`transfer_call`) + bit 5
/// (`burnable`).
const EXTENSION_FLAGS: u32 = (1 << 4) | (1 << 5);

const ZERO_ADDRESS: Address = [0u8; 32];

// ─── Custom types (declared in otigen.toml [types.*]) ─────────────────

/// One allowance grant, as read/written through its two sibling
/// storage slots (`allowance_amounts` + `allowance_expiries`, both
/// keyed (owner, spender)). `{0, 0}` means "no allowance".
#[derive(Default, Clone, Copy)]
struct Allowance {
    amount: u128,
    expiry_wave: u64,
}

fn read_allowance(owner: &Address, spender: &Address) -> Allowance {
    Allowance {
        amount: storage::allowance_amounts().read(owner, spender),
        expiry_wave: storage::allowance_expiries().read(owner, spender),
    }
}

/// Aggregate metadata surface — one free view call instead of five.
#[derive(BorshSerialize, BorshDeserialize)]
pub struct TokenInfo {
    pub name: String,
    pub symbol: String,
    pub decimals: u8,
    pub total_supply: u128,
    pub max_supply: u128,
    pub minter: Address,
    pub extension_flags: u32,
}

// ─── Constructor ───────────────────────────────────────────────────────
//
// otigen's `type = "token"` generator replaces exactly the region
// between the two `@otigen:gen-init` sentinels below with a baked,
// argument-free `init()` that reads the manifest's `[token]` values as
// compile-time constants (config-only tokens deploy with no `--args`).
// Everything OUTSIDE the sentinels is the generated surface verbatim —
// a drift test in otigen-cli pins the two together.

// @otigen:gen-init:start
/// Set metadata, mint `initial_supply` to the deployer, and seat the
/// deployer as both minter and manager. `max_supply = 0` = uncapped.
///
/// `decimals` is a display-only hint. The PTS default (and the value
/// every example uses) is 9 — parity with native quanta, 1 PYDE = 10⁹.
#[pyde::entry]
fn init(name: String, symbol: String, decimals: u8, initial_supply: u128, max_supply: u128) {
    if decimals > 18 {
        pyde::revert("token:invalid_decimals");
    }
    if max_supply > 0 && initial_supply > max_supply {
        pyde::revert("token:cap_exceeded");
    }
    let deployer = pyde::ctx::caller();

    storage::token_name().write(name);
    storage::token_symbol().write(symbol);
    storage::token_decimals().write(decimals);
    storage::total_supply().write(initial_supply);
    storage::max_supply().write(max_supply);
    storage::minter().write(deployer);
    storage::manager().write(deployer);
    storage::balances().write(&deployer, initial_supply);

    // Mint sentinel: from = zero address. One event family carries
    // all supply accounting.
    events::Transfer {
        from: ZERO_ADDRESS,
        to: deployer,
        amount: initial_supply,
    }
    .emit();
    events::RoleTransfer {
        role: ROLE_MINTER,
        holder: deployer,
        previous: ZERO_ADDRESS,
    }
    .emit();
    events::RoleTransfer {
        role: ROLE_MANAGER,
        holder: deployer,
        previous: ZERO_ADDRESS,
    }
    .emit();
}
// @otigen:gen-init:end

// ─── Views ─────────────────────────────────────────────────────────────

/// Runtime discovery handshake: which surface, which version.
#[pyde::entry]
fn standard() -> String {
    String::from(STANDARD)
}

#[pyde::entry]
fn name() -> String {
    storage::token_name().read()
}

#[pyde::entry]
fn symbol() -> String {
    storage::token_symbol().read()
}

#[pyde::entry]
fn decimals() -> u8 {
    storage::token_decimals().read()
}

#[pyde::entry]
fn total_supply() -> u128 {
    storage::total_supply().read()
}

/// 0 = uncapped.
#[pyde::entry]
fn max_supply() -> u128 {
    storage::max_supply().read()
}

/// A never-written slot reads as 0 — holding zero and never having
/// held are distinguishable at the state layer, not here.
#[pyde::entry]
fn balance_of(owner: Address) -> u128 {
    storage::balances().read(&owner)
}

/// Batched balance reads for routers/wallets — one call instead of an
/// N-round-trip loop. Views are free off-chain, so the only bound is
/// the anti-griefing cap.
#[pyde::entry]
fn balance_of_batch(owners: Vec<Address>) -> Vec<u128> {
    if owners.len() > MAX_BATCH {
        pyde::revert("token:batch_too_large");
    }
    let mut out = Vec::with_capacity(owners.len());
    for owner in &owners {
        out.push(storage::balances().read(owner));
    }
    out
}

/// Remaining spendable NOW: 0 once expired. Expiry is evaluated
/// lazily against the wave clock — no keeper, no revocation gas for
/// the forgetful.
#[pyde::entry]
fn allowance(owner: Address, spender: Address) -> u128 {
    effective_allowance(&owner, &spender).amount
}

/// The raw expiry wave of the stored grant (0 = no grant).
#[pyde::entry]
fn allowance_expiry(owner: Address, spender: Address) -> u64 {
    storage::allowance_expiries().read(&owner, &spender)
}

/// The whole metadata surface in one free call. `extension_flags` is
/// the wallet capability renderer: bit 0 freeze, 1 pause,
/// 2 registration-required, 3 metadata_uri, 4 transfer_call,
/// 5 burnable.
#[pyde::entry]
fn token_info() -> TokenInfo {
    TokenInfo {
        name: storage::token_name().read(),
        symbol: storage::token_symbol().read(),
        decimals: storage::token_decimals().read(),
        total_supply: storage::total_supply().read(),
        max_supply: storage::max_supply().read(),
        minter: storage::minter().read(),
        extension_flags: EXTENSION_FLAGS,
    }
}

/// Zero address = renounced.
#[pyde::entry]
fn minter() -> Address {
    storage::minter().read()
}

#[pyde::entry]
fn manager() -> Address {
    storage::manager().read()
}

// ─── Transfers ─────────────────────────────────────────────────────────

/// Move `amount` from the caller to `to`. Writes exactly two balance
/// slots; NEVER invokes code on `to` (mandatory hooks are the
/// costliest exploit mechanism in token history — notification is the
/// opt-in `transfer_call` path).
#[pyde::entry]
fn transfer(to: Address, amount: u128) {
    let from = pyde::ctx::caller();
    move_tokens(&from, &to, amount);
    events::Transfer { from, to, amount }.emit();
}

/// Settle-then-notify (PIP-0005 §6): (1) full balance settlement,
/// (2) `Transfer` emission, (3) notify the recipient, which must
/// return `ACK_TOKEN`. A recipient revert — or a missing/wrong
/// acknowledgement, including a name-miss swallowed by a fallback —
/// reverts the ENTIRE operation atomically (`token:bad_receiver`).
///
/// This one atomic message replaces approve-then-pull for deposits:
/// no standing authority is ever created.
#[pyde::entry]
fn transfer_call(to: Address, amount: u128, data: Vec<u8>) -> u32 {
    if data.len() > MAX_CALL_DATA {
        pyde::revert("token:data_too_large");
    }
    let operator = pyde::ctx::caller();

    // (1) settle
    move_tokens(&operator, &to, amount);
    // (2) emit — discarded with the frame if the notify leg reverts
    events::Transfer {
        from: operator,
        to,
        amount,
    }
    .emit();

    // (3) notify. Args are filled by the token — never spoofable by
    // the sender. The receiver frame sees caller() = this contract.
    let calldata = match to_vec(&(operator, operator, amount, data)) {
        Ok(b) => b,
        Err(_) => pyde::revert("token:encode"),
    };
    match pyde::call::execute::<u32>(&to, "on_token_received", &calldata) {
        Ok(ack) if ack == ACK_TOKEN => ACK_TOKEN,
        // Wrong magic, decode mismatch, revert, no code at `to`,
        // name-miss → fallback (which cannot produce the ack): all
        // collapse to the one canonical rejection.
        _ => pyde::revert("token:bad_receiver"),
    }
}

/// Spend a live allowance of `(from, caller)`: expiry checked against
/// the wave clock, amount decremented, balance moved. Emits `Transfer`
/// only — the hot pull path stays at one event; operator attribution
/// is derivable from the enclosing transaction.
#[pyde::entry]
fn transfer_from(from: Address, to: Address, amount: u128) {
    let spender = pyde::ctx::caller();
    spend_allowance(&from, &spender, amount);
    move_tokens(&from, &to, amount);
    events::Transfer { from, to, amount }.emit();
}

// ─── Delegated spending ────────────────────────────────────────────────

/// Compatibility form: set the allowance to exactly `amount` with the
/// maximum TTL auto-applied. Integrators pattern-match the name;
/// "unlimited forever" stays unrepresentable — even this grant
/// self-destructs after ~1 year of waves.
#[pyde::entry]
fn approve(spender: Address, amount: u128) {
    let owner = pyde::ctx::caller();
    let expiry_wave = pyde::ctx::wave_id().saturating_add(MAX_ALLOWANCE_TTL_WAVES);
    write_allowance(&owner, &spender, Allowance { amount, expiry_wave });
}

/// Delta increase with an explicit expiry. Deltas kill the approve
/// front-running race by construction (there is no overwrite to
/// race). An expired grant contributes 0 to the new amount.
#[pyde::entry]
fn increase_allowance(spender: Address, amount: u128, expiry_wave: u64) {
    let owner = pyde::ctx::caller();
    let now = pyde::ctx::wave_id();
    if expiry_wave <= now || expiry_wave > now.saturating_add(MAX_ALLOWANCE_TTL_WAVES) {
        pyde::revert("token:invalid_expiry");
    }
    let current = effective_allowance(&owner, &spender).amount;
    let Some(next) = current.checked_add(amount) else {
        pyde::revert("token:overflow");
    };
    write_allowance(
        &owner,
        &spender,
        Allowance {
            amount: next,
            expiry_wave,
        },
    );
}

/// Delta decrease, floored at zero. Expiry is unchanged for a live
/// grant; an expired grant collapses to `{0, 0}`.
#[pyde::entry]
fn decrease_allowance(spender: Address, amount: u128) {
    let owner = pyde::ctx::caller();
    let current = effective_allowance(&owner, &spender);
    let next = Allowance {
        amount: current.amount.saturating_sub(amount),
        expiry_wave: if current.amount == 0 { 0 } else { current.expiry_wave },
    };
    write_allowance(&owner, &spender, next);
}

/// Hard-zero the grant. Writes `{0, 0}` — never `sdelete` (no gas
/// refunds exist; deletion is strictly costlier than zeroing).
#[pyde::entry]
fn revoke_allowance(spender: Address) {
    let owner = pyde::ctx::caller();
    write_allowance(&owner, &spender, Allowance::default());
}

/// Compare-and-set: change the grant to `new_remaining` ONLY if the
/// current effective remaining equals `expected_remaining` — closes
/// the delta-accumulation footgun (re-granting without reading hands
/// the spender old + new). Under commit-reveal the check can fail at
/// reveal time if state moved; `token:allowance_changed` is a
/// retriable condition, not corruption.
#[pyde::entry]
fn set_allowance_exact(
    spender: Address,
    expected_remaining: u128,
    new_remaining: u128,
    expiry_wave: u64,
) {
    let owner = pyde::ctx::caller();
    let now = pyde::ctx::wave_id();
    if effective_allowance(&owner, &spender).amount != expected_remaining {
        pyde::revert("token:allowance_changed");
    }
    if new_remaining == 0 {
        write_allowance(&owner, &spender, Allowance::default());
        return;
    }
    if expiry_wave <= now || expiry_wave > now.saturating_add(MAX_ALLOWANCE_TTL_WAVES) {
        pyde::revert("token:invalid_expiry");
    }
    write_allowance(
        &owner,
        &spender,
        Allowance {
            amount: new_remaining,
            expiry_wave,
        },
    );
}

// ─── Supply & roles ────────────────────────────────────────────────────

/// Minter only. Reverts above `max_supply` (when capped). Emits the
/// zero-sentinel `Transfer`.
#[pyde::entry]
fn mint(to: Address, amount: u128) {
    let caller = pyde::ctx::caller();
    if caller != storage::minter().read() || caller == ZERO_ADDRESS {
        pyde::revert("token:not_minter");
    }
    guard_recipient(&to);
    let supply = storage::total_supply().read();
    let Some(next_supply) = supply.checked_add(amount) else {
        pyde::revert("token:overflow");
    };
    let cap = storage::max_supply().read();
    if cap > 0 && next_supply > cap {
        pyde::revert("token:cap_exceeded");
    }
    storage::total_supply().write(next_supply);
    let bal = storage::balances().read(&to);
    // Balance can't overflow if supply didn't: balance ≤ supply.
    storage::balances().write(&to, bal + amount);
    events::Transfer {
        from: ZERO_ADDRESS,
        to,
        amount,
    }
    .emit();
}

/// Burn the caller's own tokens (default config is burnable). Emits
/// the zero-sentinel `Transfer`.
#[pyde::entry]
fn burn(amount: u128) {
    let from = pyde::ctx::caller();
    burn_tokens(&from, amount);
}

/// Burn from `from`, spending a live allowance of `(from, caller)`.
#[pyde::entry]
fn burn_from(from: Address, amount: u128) {
    let spender = pyde::ctx::caller();
    spend_allowance(&from, &spender, amount);
    burn_tokens(&from, amount);
}

/// Manager only. Zero address = provable renounce; a renounced minter
/// can never be re-seated once the manager is also renounced.
#[pyde::entry]
fn set_minter(holder: Address) {
    let caller = pyde::ctx::caller();
    if caller != storage::manager().read() || caller == ZERO_ADDRESS {
        pyde::revert("token:not_manager");
    }
    let previous = storage::minter().read();
    storage::minter().write(holder);
    events::RoleTransfer {
        role: ROLE_MINTER,
        holder,
        previous,
    }
    .emit();
}

/// Manager only. Renouncing the manager (zero address) freezes role
/// governance permanently.
#[pyde::entry]
fn set_manager(holder: Address) {
    let caller = pyde::ctx::caller();
    if caller != storage::manager().read() || caller == ZERO_ADDRESS {
        pyde::revert("token:not_manager");
    }
    let previous = storage::manager().read();
    storage::manager().write(holder);
    events::RoleTransfer {
        role: ROLE_MANAGER,
        holder,
        previous,
    }
    .emit();
}

// ─── Internal helpers ──────────────────────────────────────────────────

/// Recipient guards shared by every credit path: the zero address
/// (burn must be explicit) and the token's own address — the single
/// largest measured stuck-token bucket on chains without this check.
fn guard_recipient(to: &Address) {
    if *to == ZERO_ADDRESS || *to == pyde::ctx::self_address() {
        pyde::revert("token:invalid_recipient");
    }
}

/// Debit `from`, credit `to`, with the full guard set. `from == to`
/// validates and no-ops (the two slots are the same slot).
fn move_tokens(from: &Address, to: &Address, amount: u128) {
    guard_recipient(to);
    let from_bal = storage::balances().read(from);
    if from_bal < amount {
        pyde::revert("token:insufficient_balance");
    }
    if from == to {
        return;
    }
    let to_bal = storage::balances().read(to);
    let Some(to_next) = to_bal.checked_add(amount) else {
        pyde::revert("token:overflow");
    };
    storage::balances().write(from, from_bal - amount);
    storage::balances().write(to, to_next);
}

fn burn_tokens(from: &Address, amount: u128) {
    let bal = storage::balances().read(from);
    if bal < amount {
        pyde::revert("token:insufficient_balance");
    }
    storage::balances().write(from, bal - amount);
    let supply = storage::total_supply().read();
    storage::total_supply().write(supply.saturating_sub(amount));
    events::Transfer {
        from: *from,
        to: ZERO_ADDRESS,
        amount,
    }
    .emit();
}

/// The stored grant if live, `{0, 0}` if expired or absent. Live
/// means `wave_id() <= expiry_wave`.
fn effective_allowance(owner: &Address, spender: &Address) -> Allowance {
    let stored = read_allowance(owner, spender);
    if stored.amount > 0 && pyde::ctx::wave_id() <= stored.expiry_wave {
        stored
    } else {
        Allowance::default()
    }
}

/// Decrement a live allowance by `amount`, distinguishing "expired"
/// from "too small" so integrators get the retriable signal.
fn spend_allowance(owner: &Address, spender: &Address, amount: u128) {
    let stored = read_allowance(owner, spender);
    if stored.amount > 0 && pyde::ctx::wave_id() > stored.expiry_wave {
        pyde::revert("token:allowance_expired");
    }
    let live = effective_allowance(owner, spender);
    if live.amount < amount {
        pyde::revert("token:insufficient_allowance");
    }
    storage::allowance_amounts().write(owner, spender, live.amount - amount);
}

/// Write + emit the absolute post-state — indexers reconstruct
/// allowance state from the latest `Approval` per pair, no delta
/// replay needed.
fn write_allowance(owner: &Address, spender: &Address, next: Allowance) {
    storage::allowance_amounts().write(owner, spender, next.amount);
    storage::allowance_expiries().write(owner, spender, next.expiry_wave);
    events::Approval {
        owner: *owner,
        spender: *spender,
        remaining: next.amount,
        expiry_wave: next.expiry_wave,
    }
    .emit();
}
