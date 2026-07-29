# factory-rust — daily development loop.
#
# Common: `make build && make test`. See `make help` for everything.
# The full create → bump flow needs a deployed template contract; see
# the README for that walk-through.

.DEFAULT_GOAL := help

.PHONY: help check-tools build cargo-build test test-v test-vv test-vvv test-vvvv \
        deploy inspect verify clean fmt clippy

NAME := pyde-template

help:
	@printf "factory-rust — development targets:\n\n"
	@printf "  make build         cargo --release + otigen build\n"
	@printf "  make test          otigen test (default verbosity)\n"
	@printf "  make test-vvvv     otigen test -vvvv (+ storage diffs)\n"
	@printf "  make deploy        otigen deploy --network devnet\n"
	@printf "  make inspect       otigen inspect $$(NAME) --network devnet\n"
	@printf "  make verify        otigen verify  $$(NAME) --network devnet\n"
	@printf "  make fmt           cargo fmt --all\n"
	@printf "  make clippy        cargo clippy -- -D warnings\n"
	@printf "  make clean         cargo clean + rm -rf artifacts/\n"
	@printf "  make check-tools   verify cargo / wasm32 / otigen are present\n"

check-tools:
	@command -v cargo >/dev/null  || (echo "missing: cargo (install via https://rustup.rs)"; exit 1)
	@command -v otigen >/dev/null || (echo "missing: otigen (install via https://github.com/pyde-net/otigen/releases)"; exit 1)
	@rustup target list --installed | grep -q wasm32-unknown-unknown \
		|| (echo "missing: rustup target add wasm32-unknown-unknown"; exit 1)
	@echo "OK"

cargo-build:
	cargo build --target wasm32-unknown-unknown --release

build: cargo-build
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

fmt:
	cargo fmt --all

clippy:
	cargo clippy --all-targets -- -D warnings

clean:
	cargo clean
	rm -rf artifacts/
