# pyde-template — daily development loop.

.DEFAULT_GOAL := help

.PHONY: help check-tools build clang-build test test-v test-vv test-vvv test-vvvv \
        deploy inspect verify clean

NAME := pyde-template

# clang from PATH. On macOS install LLVM + lld via brew (see README).
# wasm-ld must also be on PATH — the linker driver invokes it by name.
#
# `CC := clang` (not `?=`) because make's built-in default is `cc`,
# which on macOS is Apple's clang — and Apple's clang lacks the wasm32
# backend. `:=` forces us to brew's clang (or any other wasm32-capable
# clang on PATH). Override with `make CC=/path build` if needed.
CC      := clang
CFLAGS  := --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
           -Iinclude \
           -O3 -flto -fvisibility=hidden -fno-builtin-memcpy -fno-builtin-memset

help:
	@printf "pyde-template — development targets:\n\n"
	@printf "  make build         clang --target=wasm32 -> build/contract.wasm\n"
	@printf "  make test          otigen test (default verbosity)\n"
	@printf "  make test-v        otigen test -v        (+ gas + duration)\n"
	@printf "  make test-vv       otigen test -vv       (+ events)\n"
	@printf "  make test-vvv      otigen test -vvv      (+ call traces)\n"
	@printf "  make test-vvvv     otigen test -vvvv     (+ storage diffs)\n"
	@printf "  make deploy        otigen deploy --network devnet\n"
	@printf "  make inspect       otigen inspect $(NAME) --network devnet\n"
	@printf "  make verify        otigen verify  $(NAME) --network devnet\n"
	@printf "  make clean         rm -rf build/ artifacts/\n"
	@printf "  make check-tools   verify clang / wasm-ld / otigen are present\n"

check-tools:
	@command -v $(CC) >/dev/null 2>&1 \
		|| (echo "missing: $(CC) (brew install llvm, or set CC=/path/to/clang)"; exit 1)
	@$(CC) -print-targets 2>&1 | grep -q wasm32 \
		|| (echo "$(CC) lacks the wasm32 backend — use brew's LLVM (brew install llvm) or set CC to a wasm-capable clang"; exit 1)
	@ccdir=$$(dirname "$$(command -v $(CC))"); \
		command -v wasm-ld >/dev/null 2>&1 || [ -x "$$ccdir/wasm-ld" ] \
		|| (echo "missing: wasm-ld (brew install lld, add to PATH, or ensure it sits beside $(CC))"; exit 1)
	@command -v otigen >/dev/null 2>&1 \
		|| (echo "missing: otigen (install via https://github.com/pyde-net/otigen/releases)"; exit 1)
	@echo "OK"

clang-build:
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/contract.wasm main.c

# `build` chains clang directly; `otigen build` is NOT invoked from
# here. Otigen builds always own the bundling step themselves, and
# `otigen build` for C now runs clang directly without going through
# make — so delegating back here would recurse. Authors who want to
# package without `otigen build` can call `make clang-build` for the
# raw wasm.
#
# `check-tools` runs first so a missing compiler / wasm-ld / otigen, or
# an Apple clang without the wasm32 backend, surfaces a one-liner the
# user can act on rather than the cryptic `unrecognized target` from
# clang. It probes `$(CC)` (not a bare `clang`) so a `make CC=... build`
# override is validated against the compiler that will actually run,
# and looks for `wasm-ld` beside `$(CC)` as well as on PATH — a full
# LLVM install co-locates them, and clang finds its sibling linker
# without any PATH entry.
build: check-tools clang-build

test: build
	otigen test

test-v: build
	otigen test -v

test-vv: build
	otigen test -vv

test-vvv: build
	otigen test -vvv

test-vvvv: build
	otigen test -vvvv

deploy: build
	otigen deploy --network devnet

inspect:
	otigen inspect $(NAME) --network devnet

verify: build
	otigen verify $(NAME) --network devnet

clean:
	rm -rf build/ artifacts/
