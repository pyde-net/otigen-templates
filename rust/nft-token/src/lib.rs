//! `nft-token` — the PTS-N reference implementation (`pts-n/1`,
//! PIP-0005 §12), default configuration.
//!
//! Same philosophy as the fungible reference, per-id state:
//!
//!   - **Revert-only mutations** with canonical `token:*` codes
//!     (`mint` returns the fresh id — a creation, not a status).
//!   - **Settle-then-notify `transfer_call`**: ownership fully moved
//!     and `Transfer` emitted before the recipient is notified; the
//!     receiver must return `ACK_NFT` or everything unwinds — the
//!     `safeTransferFrom` reentrancy genre has no half-updated window
//!     to hit, and a fallback cannot silently swallow an NFT.
//!   - **Consent-visible roles** renounced by provable zeroing.
//!   - **Per-id owner slots**: transfers of distinct ids never
//!     conflict under parallel execution. `next_id` is monotonic and
//!     separate from the live count, so burned ids never recycle.
//!   - **On-chain metadata**: `token_uris` values run to 16 KB — real
//!     metadata JSON on-chain, no off-chain rot required.
//!
//! token_id 0 is reserved (mint starts at 1), so a zero-address read
//! on an owner slot is unambiguous "nonexistent". Storage writes zero
//! rather than deleting — the chain has no gas refunds.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::panic::PanicInfo;

use borsh::to_vec;
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
const STANDARD: &str = "pts-n/1";

/// Max bytes of `data` forwarded by `transfer_call`.
const MAX_CALL_DATA: usize = 4_096;

/// Required return of a conformant receiver:
/// `u32::from_le_bytes(Blake3("pts/on_nft_received/1")[0..4])`.
const ACK_NFT: u32 = 1_452_691_067; // LE bytes 7b 4e 96 56

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

/// "No owner" / "cleared approval" sentinel.
const ZERO_ADDRESS: Address = [0u8; 32];

// ─── Constructor ───────────────────────────────────────────────────────
//
// Everything OUTSIDE the sentinels is the generated surface verbatim —
// a drift test in otigen-cli pins the two together. A config-only
// `type = "token"` NFT bakes name/symbol/max_supply into an arg-free
// constructor, replacing exactly the region between these markers.

// @otigen:gen-init:start
/// Metadata + roles to the deployer. `max_supply = 0` = uncapped.
#[pyde::entry]
fn init(name: String, symbol: String, max_supply: u64) {
    let deployer = pyde::ctx::caller();
    storage::token_name().write(name);
    storage::token_symbol().write(symbol);
    storage::max_supply().write(max_supply);
    storage::minter().write(deployer);
    storage::manager().write(deployer);
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

/// Live count (decremented by burn); ids themselves are monotonic.
#[pyde::entry]
fn total_supply() -> u64 {
    storage::total_supply().read()
}

/// 0 = uncapped.
#[pyde::entry]
fn max_supply() -> u64 {
    storage::max_supply().read()
}

#[pyde::entry]
fn owner_of(token_id: u64) -> Address {
    let owner = storage::owners().read(&token_id);
    if owner == ZERO_ADDRESS {
        pyde::revert("token:nonexistent");
    }
    owner
}

/// Count owned by `owner`. Consistent with pts-f: a never-holder
/// reads as 0, no special-casing.
#[pyde::entry]
fn balance_of(owner: Address) -> u64 {
    storage::balances().read(&owner)
}

#[pyde::entry]
fn get_approved(token_id: u64) -> Address {
    require_exists(token_id);
    storage::token_approvals().read(&token_id)
}

#[pyde::entry]
fn is_approved_for_all(owner: Address, operator: Address) -> bool {
    storage::operator_approvals().read(&owner, &operator)
}

#[pyde::entry]
fn token_uri(token_id: u64) -> String {
    require_exists(token_id);
    storage::token_uris().read(&token_id)
}

#[pyde::entry]
fn minter() -> Address {
    storage::minter().read()
}

#[pyde::entry]
fn manager() -> Address {
    storage::manager().read()
}

// ─── Transfers ─────────────────────────────────────────────────────────

/// Move `token_id` from `from` to `to`. Caller must be the owner, the
/// per-id approved address, or an approved operator. NEVER invokes
/// recipient code.
#[pyde::entry]
fn transfer_from(from: Address, to: Address, token_id: u64) {
    let caller = pyde::ctx::caller();
    move_token(&caller, &from, &to, token_id);
    events::Transfer { from, to, token_id }.emit();
}

/// Settle-then-notify (PIP-0005 §12): ownership fully moved and
/// `Transfer` emitted FIRST, then the recipient's
/// `on_nft_received(operator, from, id, data)` must return `ACK_NFT` —
/// a revert, a wrong value, or a fallback-swallowed name-miss unwinds
/// the entire operation (`token:bad_receiver`).
#[pyde::entry]
fn transfer_call(to: Address, token_id: u64, data: Vec<u8>) -> u32 {
    if data.len() > MAX_CALL_DATA {
        pyde::revert("token:data_too_large");
    }
    let operator = pyde::ctx::caller();
    let from = storage::owners().read(&token_id);

    // (1) settle — the caller must be authorized for the id
    move_token(&operator, &from, &to, token_id);
    // (2) emit — discarded with the frame if the notify leg reverts
    events::Transfer { from, to, token_id }.emit();

    // (3) notify. Args filled by the token — never spoofable.
    let calldata = match to_vec(&(operator, from, token_id, data)) {
        Ok(b) => b,
        Err(_) => pyde::revert("token:encode"),
    };
    match pyde::call::execute::<u32>(&to, "on_nft_received", &calldata) {
        Ok(ack) if ack == ACK_NFT => ACK_NFT,
        _ => pyde::revert("token:bad_receiver"),
    }
}

// ─── Approvals ─────────────────────────────────────────────────────────

/// Per-id approval; the zero address clears it. Caller must be the
/// owner or an approved operator.
#[pyde::entry]
fn approve(to: Address, token_id: u64) {
    let owner = storage::owners().read(&token_id);
    if owner == ZERO_ADDRESS {
        pyde::revert("token:nonexistent");
    }
    let caller = pyde::ctx::caller();
    if caller != owner && !storage::operator_approvals().read(&owner, &caller) {
        pyde::revert("token:not_authorized");
    }
    storage::token_approvals().write(&token_id, to);
    events::Approval {
        owner,
        approved: to,
        token_id,
    }
    .emit();
}

/// Collection-wide operator grant for the caller's tokens.
#[pyde::entry]
fn set_approval_for_all(operator: Address, approved: bool) {
    let owner = pyde::ctx::caller();
    if operator == ZERO_ADDRESS || operator == owner {
        pyde::revert("token:invalid_operator");
    }
    storage::operator_approvals().write(&owner, &operator, approved);
    events::ApprovalForAll {
        owner,
        operator,
        approved,
    }
    .emit();
}

// ─── Supply & roles ────────────────────────────────────────────────────

/// Minter only. Assigns the next monotonic id (starting at 1; never
/// recycled), stores the metadata on-chain, emits the zero-sentinel
/// `Transfer`. Returns the fresh id.
#[pyde::entry]
fn mint(to: Address, uri: String) -> u64 {
    let caller = pyde::ctx::caller();
    if caller != storage::minter().read() || caller == ZERO_ADDRESS {
        pyde::revert("token:not_minter");
    }
    guard_recipient(&to);

    let supply = storage::total_supply().read();
    let cap = storage::max_supply().read();
    if cap > 0 && supply >= cap {
        pyde::revert("token:cap_exceeded");
    }

    let token_id = match storage::next_id().read().checked_add(1) {
        Some(id) => id,
        None => pyde::revert("token:overflow"),
    };
    storage::next_id().write(token_id);
    storage::total_supply().write(supply + 1);
    storage::owners().write(&token_id, to);
    let bal = storage::balances().read(&to);
    storage::balances().write(&to, bal + 1);
    storage::token_uris().write(&token_id, uri);

    events::Transfer {
        from: ZERO_ADDRESS,
        to,
        token_id,
    }
    .emit();
    token_id
}

/// Owner or authorized. The id is retired forever: ownership zeroed
/// (never deleted), live count decremented, monotonic `next_id`
/// untouched.
#[pyde::entry]
fn burn(token_id: u64) {
    let caller = pyde::ctx::caller();
    let owner = storage::owners().read(&token_id);
    if owner == ZERO_ADDRESS {
        pyde::revert("token:nonexistent");
    }
    if !is_authorized(&owner, &caller, token_id) {
        pyde::revert("token:not_authorized");
    }
    storage::owners().write(&token_id, ZERO_ADDRESS);
    storage::token_approvals().write(&token_id, ZERO_ADDRESS);
    let bal = storage::balances().read(&owner);
    storage::balances().write(&owner, bal.saturating_sub(1));
    let supply = storage::total_supply().read();
    storage::total_supply().write(supply.saturating_sub(1));
    events::Transfer {
        from: owner,
        to: ZERO_ADDRESS,
        token_id,
    }
    .emit();
}

/// Manager only. Zero address = provable renounce.
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

/// Manager only. Renouncing freezes role governance permanently.
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

fn require_exists(token_id: u64) {
    if storage::owners().read(&token_id) == ZERO_ADDRESS {
        pyde::revert("token:nonexistent");
    }
}

/// Recipient guards shared by every credit path: the zero address and
/// the token's own contract address.
fn guard_recipient(to: &Address) {
    if *to == ZERO_ADDRESS || *to == pyde::ctx::self_address() {
        pyde::revert("token:invalid_recipient");
    }
}

/// Whether `spender` may move `token_id`: owner, per-id approved, or
/// an approved operator.
fn is_authorized(owner: &Address, spender: &Address, token_id: u64) -> bool {
    if spender == owner {
        return true;
    }
    let approved = storage::token_approvals().read(&token_id);
    if &approved == spender && approved != ZERO_ADDRESS {
        return true;
    }
    storage::operator_approvals().read(owner, spender)
}

/// Ownership move with the full guard set. Clears the per-id approval
/// atomically (same slot family — no extra conflict surface).
fn move_token(caller: &Address, from: &Address, to: &Address, token_id: u64) {
    let owner = storage::owners().read(&token_id);
    if owner == ZERO_ADDRESS {
        pyde::revert("token:nonexistent");
    }
    if owner != *from {
        pyde::revert("token:not_owner");
    }
    guard_recipient(to);
    if !is_authorized(&owner, caller, token_id) {
        pyde::revert("token:not_authorized");
    }
    storage::token_approvals().write(&token_id, ZERO_ADDRESS);
    let from_bal = storage::balances().read(from);
    storage::balances().write(from, from_bal.saturating_sub(1));
    let to_bal = storage::balances().read(to);
    match to_bal.checked_add(1) {
        Some(next) => storage::balances().write(to, next),
        None => pyde::revert("token:overflow"),
    }
    storage::owners().write(&token_id, *to);
}
