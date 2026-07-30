# merkle-claim-airdrop — daily development loop.

.DEFAULT_GOAL := build

.PHONY: help check-tools build clang-build test test-v test-vv test-vvv test-vvvv \
        deploy inspect verify clean

NAME := merkle-claim-airdrop

# clang from PATH. On macOS install LLVM + lld via brew (see README).
# wasm-ld must also be on PATH — the linker driver invokes it by name.
#
# `CC := clang` (not `?=`) because make's built-in default is `cc`,
# which on macOS is Apple's clang — and Apple's clang lacks the wasm32
# backend. `:=` forces us to brew's clang (or any other wasm32-capable
# clang on PATH). Override with `make CC=/path build` if needed.
CC      := clang
CFLAGS  := --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
           -O3 -flto -fvisibility=hidden -fno-builtin-memcpy -fno-builtin-memset

help:
	@printf "merkle-claim-airdrop — development targets:\n\n"
	@printf "  make build         otigen build (generates pyde_gen.h + clang)\n"
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
	@command -v clang   >/dev/null || (echo "missing: clang (brew install llvm; add to PATH)"; exit 1)
	@command -v wasm-ld >/dev/null || (echo "missing: wasm-ld (brew install lld; add to PATH)"; exit 1)
	@command -v otigen  >/dev/null || (echo "missing: otigen (install via https://github.com/pyde-net/otigen/releases)"; exit 1)
	@clang -print-targets 2>&1 | grep -q wasm32 \
		|| (echo "clang lacks wasm32 backend — try brew's LLVM rather than Apple's bundled clang"; exit 1)
	@echo "OK"

# Escape hatch: compile main.c directly with clang. Requires pyde_gen.h to
# already exist (run `otigen build` once first) — main.c #includes it.
clang-build:
	@mkdir -p build
	$(CC) $(CFLAGS) -o build/contract.wasm main.c

# `otigen build` generates pyde_gen.h from otigen.toml and then invokes clang
# itself (with the flags above), so we do NOT run clang ahead of it — that
# ordering would try to compile main.c before the generated header exists.
build:
	otigen build

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
