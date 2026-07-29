//! A factory: mint and drive child contracts via `pyde::instantiate`.
//!
//! A factory creates fresh instances of a TEMPLATE — and the template is
//! just ANY contract you have already deployed. Deploy a contract, copy
//! its address, and pass that address to `create`. Each child is a
//! first-class contract — its own address, its own isolated storage —
//! sharing the template's already-cached code, so nothing is copied or
//! recompiled.
//!
//! The template is instantiated with NO constructor arguments, so it
//! works with any contract that has no required constructor (the
//! built-in `counter` template is a perfect fit). To drive a child,
//! this factory cross-calls its `increment() -> u64`, so the template
//! should expose that.
//!
//! A child's address is a pure function of `(factory, template, salt)`,
//! so it is predictable off-chain before the child exists. This example
//! shows the THREE ways to choose that salt and the THREE ways to then
//! reach a child:
//!
//!   CREATE — choose the salt
//!     * `create`          — AUTO. The factory keeps a `next_key`
//!                           counter and salts each child with it, so a
//!                           caller manages no salt at all.
//!     * `create_with_key` — an explicit `u64` salt (a user id, a nonce).
//!     * `create_named`    — a `String` salt (a name, a market pair).
//!
//!   INTERACT — reach a child
//!     * `bump`       — BY u64 KEY: look it up in the registry, call it.
//!     * `bump_named` — BY STRING NAME: look it up, call it.
//!     * `bump_at`    — BY ADDRESS: call any child directly, no lookup.

#![no_std]

extern crate alloc;

use alloc::string::String;
use core::panic::PanicInfo;
use pyde_host as pyde;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

pyde::declare_storage!();

// ─── create ─────────────────────────────────────────────────────────

/// Mint a child with an AUTO salt — the factory's `next_key` counter.
/// The caller manages nothing: each call takes the next slot. The child
/// is recorded in the u64 registry and returned.
#[pyde::entry]
fn create(template: pyde::Address) -> pyde::Address {
    let key = storage::next_key().read();
    let child = mint(&template, &pyde::Salt::of(&key));
    storage::children().write(&key, child);
    storage::next_key().write(key + 1);
    record();
    child
}

/// Mint a child at an explicit `u64` salt key (e.g. a user id). The
/// address is `(factory, template, Salt::of(key))`, so the same key
/// always targets the same child — a second call reverts `exists`.
#[pyde::entry]
fn create_with_key(template: pyde::Address, key: u64) -> pyde::Address {
    let child = mint(&template, &pyde::Salt::of(&key));
    storage::children().write(&key, child);
    record();
    child
}

/// Mint a child at a `String` salt (e.g. a market pair `"ETH/USDC"`).
/// `Salt::of` borsh-encodes the string and hashes it, so any string is
/// a valid, deterministic identity. Recorded in the name registry.
#[pyde::entry]
fn create_named(template: pyde::Address, name: String) -> pyde::Address {
    let child = mint(&template, &pyde::Salt::of(&name));
    storage::named().write(&name, child);
    record();
    child
}

// ─── look up ────────────────────────────────────────────────────────

/// A child by its `u64` key — the factory's getPair. Zero address if
/// that key was never created.
#[pyde::entry]
fn child_of(key: u64) -> pyde::Address {
    storage::children().read(&key)
}

/// A child by its `String` name. Zero address if never created.
#[pyde::entry]
fn child_of_name(name: String) -> pyde::Address {
    storage::named().read(&name)
}

/// The key `create` will use next.
#[pyde::entry]
fn next_key() -> u64 {
    storage::next_key().read()
}

/// Total children this factory has minted.
#[pyde::entry]
fn created() -> u64 {
    storage::created().read()
}

// ─── interact ───────────────────────────────────────────────────────

/// Drive a child BY u64 KEY: pull it from the registry, then call its
/// `increment` via a cross-call. Returns the child's new value.
#[pyde::entry]
fn bump(key: u64) -> u64 {
    increment(storage::children().read(&key))
}

/// Drive a child BY STRING NAME.
#[pyde::entry]
fn bump_named(name: String) -> u64 {
    increment(storage::named().read(&name))
}

/// Drive a child BY ADDRESS — no lookup. This is how a contract talks
/// to a contract it (or anyone) created: by address, sharing the
/// template's ABI, no typed handle required.
#[pyde::entry]
fn bump_at(child: pyde::Address) -> u64 {
    increment(child)
}

// ─── internals ──────────────────────────────────────────────────────

/// Instantiate `template` at `salt` with NO constructor args — works
/// with any contract that has no required constructor. Surfaces the
/// engine's outcomes as clean reverts: a repeat identity is `exists`,
/// an unknown template is `template-not-found`, and a child ctor revert
/// bubbles up its message.
fn mint(template: &pyde::Address, salt: &[u8; 32]) -> pyde::Address {
    match pyde::prepare(template, salt).instantiate() {
        Ok(child) => child,
        Err(pyde::InstantiateError::Exists { .. }) => pyde::revert("exists"),
        Err(pyde::InstantiateError::TemplateNotFound) => pyde::revert("template-not-found"),
        Err(e) => pyde::revert(
            e.revert_message()
                .as_deref()
                .unwrap_or("instantiate-failed"),
        ),
    }
}

/// Cross-call a child's `increment` (no args) and return the new value.
fn increment(child: pyde::Address) -> u64 {
    match pyde::call::execute::<u64>(&child, "increment", &[]) {
        Ok(value) => value,
        Err(e) => pyde::revert(e.revert_message().as_deref().unwrap_or("child-call-failed")),
    }
}

/// Tick the minted-children counter.
fn record() {
    let n = storage::created().read();
    storage::created().write(n + 1);
}
