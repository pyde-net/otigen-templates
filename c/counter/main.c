// `pyde-template` — minimal Pyde contract scaffolded by `otigen init --lang c`.
//
// You write only the typed inner functions (`__<fn>_impl`) plus otigen.toml.
// `otigen build` reads [functions.*] / [state] / [events] and generates
// `pyde_gen.h`: the () -> () export shims (calldata → borsh-decode → call the
// impl → borsh-encode → return), the typed storage accessors, and the event
// emitters. Nothing here derives a storage slot, packs calldata, or touches
// borsh — the generated header owns all of it. Each `__<fn>_impl` below is
// forward-declared in that header, so if a signature stops matching the
// manifest the C compiler rejects the build rather than shipping a contract
// that decodes calldata one way and reads it another.
//
// `-nostdlib` still applies: the generated bump allocator + byte-loop mem ops
// keep libc out. Bigger surfaces (maps, events, transfers, cross-contract
// calls) live in the source templates — see `otigen new --list`.

#include "pyde_gen.h"

// increment() -> uint64 — advance the counter, return the new value.
// `counter_get`/`counter_set` are generated from the [state] `counter` field.
uint64_t __increment_impl(void) {
    uint64_t next = counter_get() + 1;
    counter_set(next);
    return next;
}

// get() -> uint64 — read the counter without mutating. Declared `view` in
// otigen.toml, so the engine rejects any state write from this frame.
uint64_t __get_impl(void) {
    return counter_get();
}
