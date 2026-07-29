//! `upgradeable-proxy` — thin proxy + admin-swappable logic
//! delegate-call pattern.
//!
//! `forward(function, calldata)` runs the logic contract's code in
//! THIS contract's frame via `pyde::call::execute_delegate_raw`:
//!
//!   - `self_address` = proxy
//!   - `caller`       = caller (preserved across delegate)
//!   - storage slots  = derived from proxy's self_address, so the
//!                      logic's `storage::value()` macro accessor
//!                      writes to the proxy's slot
//!
//! Result: when the admin swaps `logic` for a newer implementation,
//! the proxy's state (in particular `value`) is preserved across the
//! upgrade. That's the whole point of the pattern.
//!
//! ## Privileged-slot namespacing
//!
//! Pyde's storage slots are derived as
//! `Poseidon2(self_address || field_name)`. Under delegate-call the
//! logic contract sees the proxy's `self_address`, so a logic
//! contract that happens to declare a field named `admin` would
//! collide with the proxy's admin slot and clobber it. The fix:
//! prefix the proxy's privileged fields with `proxy_` so the logic
//! contract would have to specifically choose the same prefixed
//! name to collide — a much louder mistake. `value` stays unprefixed
//! since it's the *intended* shared-state demonstration field.

#![no_std]

extern crate alloc;

use alloc::vec::Vec;
use alloc::string::String;
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

/// 32-byte all-zero address — sentinel for an unset / invalid address.
/// Used to reject inputs that would permanently brick the proxy
/// (zero admin = no one can ever upgrade; zero logic = every
/// `forward` delegate-calls into a non-existent contract).
const ZERO_ADDRESS: Address = [0u8; 32];

#[pyde::entry]
fn init(initial_logic: Address) {
    // Defence-in-depth re-init guard. The manifest already tags this
    // function as `["constructor"]` so the chain rejects post-deploy
    // calls — but the in-source flag catches accidental re-runs in
    // tests + makes the invariant explicit at the source level.
    if storage::proxy_admin().read() != ZERO_ADDRESS {
        pyde::revert("proxy: already initialized");
    }
    let admin = pyde::ctx::caller();
    if admin == ZERO_ADDRESS || initial_logic == ZERO_ADDRESS {
        pyde::revert("proxy: init with zero address");
    }
    storage::proxy_admin().write(admin);
    storage::proxy_logic().write(initial_logic);
    events::Initialized { admin, initial_logic }.emit();
}

/// Admin-only logic-pointer swap. Doesn't touch `value` — the
/// preserved state is the whole point.
#[pyde::entry]
fn upgrade_to(new_logic: Address) {
    let admin = storage::proxy_admin().read();
    let caller = pyde::ctx::caller();
    if caller != admin {
        pyde::revert("proxy: caller is not admin");
    }
    if new_logic == ZERO_ADDRESS {
        pyde::revert("proxy: upgrade to zero address");
    }
    let old_logic = storage::proxy_logic().read();
    storage::proxy_logic().write(new_logic);
    events::Upgraded { old_logic, new_logic }.emit();
}

/// Rotate the admin role. Admin-only. Reverts on a zero-address
/// new admin — use `renounce_admin()` for that path so the
/// irrevocable lock is loud rather than disguised as a transfer.
#[pyde::entry]
fn transfer_admin(new_admin: Address) {
    let admin = storage::proxy_admin().read();
    let caller = pyde::ctx::caller();
    if caller != admin {
        pyde::revert("proxy: caller is not admin");
    }
    if new_admin == ZERO_ADDRESS {
        pyde::revert("proxy: transfer to zero address; use renounce_admin");
    }
    let old_admin = admin;
    storage::proxy_admin().write(new_admin);
    events::AdminTransferred { old_admin, new_admin }.emit();
}

/// Renounce the admin role — sets the admin slot to the zero
/// address. After this call NO ONE can `upgrade_to` or call any
/// admin-gated entry; the logic pointer is frozen at its current
/// value forever. Irreversible.
#[pyde::entry]
fn renounce_admin() {
    let admin = storage::proxy_admin().read();
    let caller = pyde::ctx::caller();
    if caller != admin {
        pyde::revert("proxy: caller is not admin");
    }
    storage::proxy_admin().write(ZERO_ADDRESS);
    events::AdminTransferred { old_admin: admin, new_admin: ZERO_ADDRESS }.emit();
}

/// Dispatcher. Delegate-calls `logic.function(calldata)` in this
/// contract's frame. Returns the raw bytes the logic produced as
/// its `pyde::return_(...)` payload — caller decodes them per
/// the function's documented return type.
///
/// Uses [`pyde::call::execute_delegate_raw`] (not the typed
/// `execute_delegate::<T>`) because the proxy is a type-erased
/// forwarder — it doesn't know the logic's return shape and
/// can't borsh-decode it. The raw wrapper hands the logic's
/// bytes back verbatim.
#[pyde::entry]
fn forward(function: String, calldata: Vec<u8>) -> Vec<u8> {
    let logic = storage::proxy_logic().read();
    match pyde::call::execute_delegate_raw(&logic, &function, &calldata) {
        Ok(bytes) => bytes,
        Err(CallError::Reverted(payload)) => {
            // Pass the logic's revert string straight through to
            // the proxy's caller so they see exactly what the
            // logic said, not a generic "proxy failed" message.
            let msg = core::str::from_utf8(&payload).unwrap_or("proxy: delegate-call failed");
            pyde::revert(msg);
        }
        Err(CallError::InvalidFunction) => {
            pyde::revert("proxy: logic has no such function");
        }
        Err(_) => {
            pyde::revert("proxy: delegate-call failed");
        }
    }
}

#[pyde::entry]
fn get_admin() -> Address {
    storage::proxy_admin().read()
}

#[pyde::entry]
fn get_logic() -> Address {
    storage::proxy_logic().read()
}

#[pyde::entry]
fn get_value() -> u64 {
    storage::value().read()
}
