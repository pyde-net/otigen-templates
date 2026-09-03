// The PTS-N reference implementation (`pts-n/1`, PIP-0005 §12), default
// configuration.
//
// Same philosophy as the fungible reference, with per-id state:
//
//   - **Revert-only mutations** with canonical `token:*` codes. `mint`
//     returns the fresh id — a creation, not a status.
//   - **Settle-then-notify `transfer_call`**: ownership fully moved and
//     `Transfer` emitted before the recipient is notified; the receiver
//     must return `ACK_NFT` or everything unwinds. The
//     `safeTransferFrom` reentrancy genre has no half-updated window to
//     hit, and a fallback cannot silently swallow an NFT.
//   - **Consent-visible roles** renounced by provable zeroing.
//   - **Per-id owner slots**: transfers of distinct ids never conflict
//     under parallel execution. `next_id` is monotonic and separate from
//     the live count, so burned ids never recycle.
//   - **On-chain metadata**: `token_uris` values run to 16 KB — real
//     metadata JSON on-chain, no off-chain rot required.
//
// token_id 0 is reserved (minting starts at 1), so a zero-address read on
// an owner slot is unambiguously "nonexistent". Storage writes zero
// rather than deleting, because the chain has no gas refunds.

import {
  Address,
  BorshEncoder,
  BorshDecoder,
  Call,
  caller,
  selfAddress,
  equals32,
  newAddress,
  revertStr,
} from "@pyde-net/host/assembly";
import { storage } from "./pyde.storage.generated";
import { events } from "./pyde.events.generated";

// ─── Protocol constants (PIP-0005 §3) ─────────────────────────────────

/// The surface this contract conforms to.
const STANDARD: string = "pts-n/1";

/// Max bytes of `data` forwarded by `transfer_call`.
const MAX_CALL_DATA: i32 = 4_096;

/// Required return of a conformant receiver:
/// `u32::from_le_bytes(Blake3("pts/on_nft_received/1")[0..4])`.
const ACK_NFT: u32 = 1_452_691_067; // LE bytes 7b 4e 96 56

/// 32-byte role identifiers carried by `RoleTransfer` (PIP-0005 §3):
/// `Blake3("minter")` / `Blake3("manager")`.
const ROLE_MINTER: StaticArray<u8> = [
  0x73, 0xab, 0x3f, 0x67, 0x1e, 0x61, 0x21, 0x7b, 0x4d, 0xa0, 0x19, 0x61, 0x4d, 0xf2, 0x58,
  0xeb, 0x97, 0xda, 0x1a, 0xcf, 0xd8, 0x73, 0x1a, 0x89, 0x30, 0x73, 0x92, 0x73, 0x46, 0x96,
  0xe5, 0xdd,
];
const ROLE_MANAGER: StaticArray<u8> = [
  0x12, 0x4a, 0xfb, 0x10, 0x8c, 0x8d, 0x69, 0xca, 0xc3, 0x5b, 0x28, 0xa8, 0x51, 0xd6, 0xf6,
  0x8f, 0x5b, 0x01, 0x0a, 0x28, 0x33, 0xc7, 0x32, 0x5b, 0x60, 0x75, 0x79, 0xc8, 0x8c, 0x9e,
  0x35, 0xc1,
];

// ─── Constructor ───────────────────────────────────────────────────────

/// Metadata and roles to the deployer. `max_supply = 0` = uncapped.
@entry
export function init(name: string, symbol: string, max_supply: u64): void {
  const deployer = caller();
  const zero = newAddress();
  storage.token_name.write(name);
  storage.token_symbol.write(symbol);
  storage.max_supply.write(max_supply);
  storage.minter.write(deployer);
  storage.manager.write(deployer);
  events.RoleTransfer(ROLE_MINTER, deployer, zero);
  events.RoleTransfer(ROLE_MANAGER, deployer, zero);
}

// ─── Views ─────────────────────────────────────────────────────────────

@entry
@view
export function standard(): string {
  return STANDARD;
}

@entry
@view
export function name(): string {
  return storage.token_name.read();
}

@entry
@view
export function symbol(): string {
  return storage.token_symbol.read();
}

/// Live count, decremented by burn; the ids themselves are monotonic.
@entry
@view
export function total_supply(): u64 {
  return storage.total_supply.read();
}

/// 0 = uncapped.
@entry
@view
export function max_supply(): u64 {
  return storage.max_supply.read();
}

@entry
@view
export function owner_of(token_id: u64): Address {
  const owner = storage.owners.read(token_id);
  if (equals32(owner, newAddress())) {
    revertStr("token:nonexistent");
  }
  return owner;
}

/// Count owned by `owner`. Consistent with pts-f: a never-holder reads as
/// 0, with no special-casing.
@entry
@view
export function balance_of(owner: Address): u64 {
  return storage.balances.read(owner);
}

@entry
@view
export function get_approved(token_id: u64): Address {
  requireExists(token_id);
  return storage.token_approvals.read(token_id);
}

@entry
@view
export function is_approved_for_all(owner: Address, operator: Address): bool {
  return storage.operator_approvals.read(owner, operator);
}

@entry
@view
export function token_uri(token_id: u64): string {
  requireExists(token_id);
  return storage.token_uris.read(token_id);
}

@entry
@view
export function minter(): Address {
  return storage.minter.read();
}

@entry
@view
export function manager(): Address {
  return storage.manager.read();
}

// ─── Transfers ─────────────────────────────────────────────────────────

/// Move `token_id` from `from` to `to`. The caller must be the owner, the
/// per-id approved address, or an approved operator. NEVER invokes
/// recipient code.
@entry
export function transfer_from(from: Address, to: Address, token_id: u64): void {
  moveToken(caller(), from, to, token_id);
  events.Transfer(from, to, token_id);
}

/// Settle-then-notify (PIP-0005 §12): ownership fully moved and
/// `Transfer` emitted FIRST, then the recipient's
/// `on_nft_received(operator, from, id, data)` must return `ACK_NFT`. A
/// revert, a wrong value, or a fallback-swallowed name-miss unwinds the
/// entire operation (`token:bad_receiver`).
@entry
export function transfer_call(to: Address, token_id: u64, data: Array<u8>): u32 {
  if (data.length > MAX_CALL_DATA) {
    revertStr("token:data_too_large");
  }
  const operator = caller();
  const from = storage.owners.read(token_id);

  // (1) settle — the caller must be authorized for the id
  moveToken(operator, from, to, token_id);
  // (2) emit — discarded with the frame if the notify leg reverts
  events.Transfer(from, to, token_id);

  // (3) notify. The arguments are filled by the token and are never
  // spoofable. A borsh tuple is its fields back to back with no framing.
  const bytes = new StaticArray<u8>(data.length);
  for (let i = 0, n = data.length; i < n; i++) {
    unchecked((bytes[i] = data[i]));
  }
  const calldata = new BorshEncoder()
    .address(operator)
    .address(from)
    .u64(token_id)
    .bytes(bytes)
    .toBytes();

  const r = Call(to, "on_nft_received").args(calldata).exec();
  if (!r.ok || r.data.length != 4) {
    revertStr("token:bad_receiver");
  }
  if (new BorshDecoder(r.data).u32() != ACK_NFT) {
    revertStr("token:bad_receiver");
  }
  return ACK_NFT;
}

// ─── Approvals ─────────────────────────────────────────────────────────

/// Per-id approval; the zero address clears it. The caller must be the
/// owner or an approved operator.
@entry
export function approve(to: Address, token_id: u64): void {
  const owner = storage.owners.read(token_id);
  if (equals32(owner, newAddress())) {
    revertStr("token:nonexistent");
  }
  const who = caller();
  if (!equals32(who, owner) && !storage.operator_approvals.read(owner, who)) {
    revertStr("token:not_authorized");
  }
  storage.token_approvals.write(token_id, to);
  events.Approval(owner, to, token_id);
}

/// Collection-wide operator grant for the caller's tokens.
@entry
export function set_approval_for_all(operator: Address, approved: bool): void {
  const owner = caller();
  if (equals32(operator, newAddress()) || equals32(operator, owner)) {
    revertStr("token:invalid_operator");
  }
  storage.operator_approvals.write(owner, operator, approved);
  events.ApprovalForAll(owner, operator, approved);
}

// ─── Supply and roles ──────────────────────────────────────────────────

/// Minter only. Assigns the next monotonic id (starting at 1, never
/// recycled), stores the metadata on-chain, and emits the zero-sentinel
/// `Transfer`. Returns the fresh id.
@entry
export function mint(to: Address, uri: string): u64 {
  const who = caller();
  if (!equals32(who, storage.minter.read()) || equals32(who, newAddress())) {
    revertStr("token:not_minter");
  }
  guardRecipient(to);

  const supply = storage.total_supply.read();
  const cap = storage.max_supply.read();
  if (cap > 0 && supply >= cap) {
    revertStr("token:cap_exceeded");
  }

  const previousId = storage.next_id.read();
  const token_id = previousId + 1;
  if (token_id < previousId) {
    revertStr("token:overflow");
  }
  storage.next_id.write(token_id);
  storage.total_supply.write(supply + 1);
  storage.owners.write(token_id, to);
  storage.balances.write(to, storage.balances.read(to) + 1);
  storage.token_uris.write(token_id, uri);

  events.Transfer(newAddress(), to, token_id);
  return token_id;
}

/// Owner or authorized. The id is retired forever: ownership zeroed
/// (never deleted), the live count decremented, and the monotonic
/// `next_id` left untouched.
@entry
export function burn(token_id: u64): void {
  const zero = newAddress();
  const owner = storage.owners.read(token_id);
  if (equals32(owner, zero)) {
    revertStr("token:nonexistent");
  }
  if (!isAuthorized(owner, caller(), token_id)) {
    revertStr("token:not_authorized");
  }
  storage.owners.write(token_id, zero);
  storage.token_approvals.write(token_id, zero);
  storage.balances.write(owner, satSub1(storage.balances.read(owner)));
  storage.total_supply.write(satSub1(storage.total_supply.read()));
  events.Transfer(owner, zero, token_id);
}

/// Manager only. Zero address = provable renounce.
@entry
export function set_minter(holder: Address): void {
  requireManager();
  const previous = storage.minter.read();
  storage.minter.write(holder);
  events.RoleTransfer(ROLE_MINTER, holder, previous);
}

/// Manager only. Renouncing freezes role governance permanently.
@entry
export function set_manager(holder: Address): void {
  requireManager();
  const previous = storage.manager.read();
  storage.manager.write(holder);
  events.RoleTransfer(ROLE_MANAGER, holder, previous);
}

// ─── Internal helpers ──────────────────────────────────────────────────

/// `n - 1` floored at zero. AssemblyScript's u64 wraps rather than
/// saturating, so the guard is explicit.
function satSub1(n: u64): u64 {
  return n == 0 ? 0 : n - 1;
}

function requireExists(token_id: u64): void {
  if (equals32(storage.owners.read(token_id), newAddress())) {
    revertStr("token:nonexistent");
  }
}

/// Revert unless the caller is the seated manager. A zero-address caller
/// can never match, so a renounced manager freezes role governance.
function requireManager(): void {
  const who = caller();
  if (!equals32(who, storage.manager.read()) || equals32(who, newAddress())) {
    revertStr("token:not_manager");
  }
}

/// Recipient guards shared by every credit path: the zero address and the
/// token's own contract address.
function guardRecipient(to: Address): void {
  if (equals32(to, newAddress()) || equals32(to, selfAddress())) {
    revertStr("token:invalid_recipient");
  }
}

/// Whether `spender` may move `token_id`: owner, per-id approved, or an
/// approved operator.
function isAuthorized(owner: Address, spender: Address, token_id: u64): bool {
  if (equals32(spender, owner)) {
    return true;
  }
  const approved = storage.token_approvals.read(token_id);
  if (equals32(approved, spender) && !equals32(approved, newAddress())) {
    return true;
  }
  return storage.operator_approvals.read(owner, spender);
}

/// Ownership move with the full guard set. Clears the per-id approval
/// atomically — same slot family, so no extra conflict surface.
function moveToken(who: Address, from: Address, to: Address, token_id: u64): void {
  const owner = storage.owners.read(token_id);
  if (equals32(owner, newAddress())) {
    revertStr("token:nonexistent");
  }
  if (!equals32(owner, from)) {
    revertStr("token:not_owner");
  }
  guardRecipient(to);
  if (!isAuthorized(owner, who, token_id)) {
    revertStr("token:not_authorized");
  }
  storage.token_approvals.write(token_id, newAddress());
  storage.balances.write(from, satSub1(storage.balances.read(from)));
  const toBal = storage.balances.read(to);
  const toNext = toBal + 1;
  if (toNext < toBal) {
    revertStr("token:overflow");
  }
  storage.balances.write(to, toNext);
  storage.owners.write(token_id, to);
}
