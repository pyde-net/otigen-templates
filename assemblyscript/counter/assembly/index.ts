// `pyde-template` — minimal Pyde counter contract (increment + get),
// scaffolded by `otigen init --lang as`.
//
// What it shows: host-fn imports from `@pyde-net/host` (run
// `npm install` first), variable-length storage via `sload`/`sstore`
// over a self-derived slot, `export function` entry points registered
// in `otigen.toml`, and a manual little-endian u64 codec.
//
// Bigger surfaces (events, transfers, cross-contract calls) live in
// the canonical examples at `pyde-net/otigen/examples/`.

import {
  sload,
  sstore,
  self_address,
  hash_poseidon2,
  pyde_return,
} from "@pyde-net/host/assembly";

// AssemblyScript abort handler. Replaces AS's default `env.abort`
// import (via asconfig.json's `use: ["abort=..."]`) so no non-`pyde::*`
// import is emitted; any panic traps deterministically. Give it richer
// behaviour (e.g. `revert(reason)`) if you like. Deliberately NOT
// exported — the `use` directive resolves it by module path.
function abort(
  _message: string | null = null,
  _fileName: string | null = null,
  _line: u32 = 0,
  _column: u32 = 0,
): void {
  unreachable();
}

// Storage field name for the counter. Its slot is derived at runtime
// as `Poseidon2(self_address || FIELD_COUNTER)` — field names live
// inside the slot hash and never appear on-chain literally.
const FIELD_COUNTER: StaticArray<u8> = [0x63, 0x6f, 0x75, 0x6e, 0x74, 0x65, 0x72]; // "counter"

// u64 storage codec. Pyde storage is variable-length: a u64 is stored
// as exactly 8 bytes (no 32-byte padding). The slot key is
// `Poseidon2(self_address || field [|| key])` — pass a null key for
// scalar slots. AS has no built-in LE u64 encoder, so we hand-roll it.

function deriveSlot(field: StaticArray<u8>, key: StaticArray<u8> | null): StaticArray<u8> {
  const fieldLen = field.length;
  const keyLen = key != null ? key.length : 0;
  const total = 32 + fieldLen + keyLen;

  const preimage = new StaticArray<u8>(total);
  self_address(changetype<usize>(preimage));
  for (let i = 0; i < fieldLen; i++) {
    preimage[32 + i] = field[i];
  }
  if (key != null) {
    for (let i = 0; i < keyLen; i++) {
      preimage[32 + fieldLen + i] = key[i];
    }
  }

  const out = new StaticArray<u8>(32);
  hash_poseidon2(changetype<usize>(preimage), total, changetype<usize>(out));
  return out;
}

function readCounter(): u64 {
  const slot = deriveSlot(FIELD_COUNTER, null);
  const buf = new StaticArray<u8>(8);
  const actual = sload(changetype<usize>(slot), changetype<usize>(buf), 8);
  if (actual <= 0) {
    return 0;
  }
  // u64 LE decode — canonical Pyde wire encoding per
  // HOST_FN_ABI_SPEC §7.1 (matches borsh + substrate macros).
  let v: u64 = 0;
  for (let i = 7; i >= 0; i--) {
    v = (v << 8) | u64(buf[i]);
  }
  return v;
}

function writeCounter(value: u64): void {
  const slot = deriveSlot(FIELD_COUNTER, null);
  const buf = new StaticArray<u8>(8);
  // u64 LE encode — buf[0] is the lowest 8 bits.
  for (let i = 0; i < 8; i++) {
    buf[i] = u8(value & 0xff);
    value = value >> 8;
  }
  sstore(changetype<usize>(slot), changetype<usize>(buf), 8);
}

// ─────────────────────────────────────────────────────────────────────
// Public entry points (declared in otigen.toml `[functions.*]`)
// ─────────────────────────────────────────────────────────────────────

// increment increments the counter and returns the new value. Pyde
// entry points have the WASM signature `() -> ()`: args arrive via
// `calldata_*` host fns and the return goes out via `pyde_return`.
export function increment(): void {
  const next = readCounter() + 1;
  writeCounter(next);
  emitU64(next);
}

// get reads the current counter without mutating. Marked `view` in
// otigen.toml — must not modify state, transfer value, or emit events.
export function get(): void {
  emitU64(readCounter());
}

// emitU64 writes a u64 as 8 little-endian bytes (the Pyde return-data
// wire format) and exits via `pyde_return`. The trailing
// `unreachable()` marks the never-returns tail AS can't express.
function emitU64(value: u64): void {
  const buf = new StaticArray<u8>(8);
  for (let i = 0; i < 8; i++) {
    buf[i] = u8(value & 0xff);
    value = value >> 8;
  }
  pyde_return(changetype<usize>(buf), 8);
  unreachable();
}
