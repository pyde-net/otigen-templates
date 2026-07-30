# pyde-template — daily development loop.

.DEFAULT_GOAL := help

.PHONY: help check-tools build test test-v test-vv test-vvv test-vvvv \
        deploy inspect verify clean

NAME := pyde-template

help:
	@printf "pyde-template — development targets:\n\n"
	@printf "  make build         otigen build (regenerates pyde_gen.go + tinygo)\n"
	@printf "  make test          otigen test (default verbosity)\n"
	@printf "  make test-v        otigen test -v        (+ gas + duration)\n"
	@printf "  make test-vv       otigen test -vv       (+ events)\n"
	@printf "  make test-vvv      otigen test -vvv      (+ call traces)\n"
	@printf "  make test-vvvv     otigen test -vvvv     (+ storage diffs)\n"
	@printf "  make deploy        otigen deploy --network devnet\n"
	@printf "  make inspect       otigen inspect $$(NAME) --network devnet\n"
	@printf "  make verify        otigen verify  $$(NAME) --network devnet\n"
	@printf "  make clean         rm -rf build/ artifacts/\n"
	@printf "  make check-tools   verify tinygo / otigen are present\n"

check-tools:
	@command -v tinygo >/dev/null || (echo "missing: tinygo (install via https://tinygo.org/getting-started/install/)"; exit 1)
	@command -v otigen >/dev/null || (echo "missing: otigen (install via https://github.com/pyde-net/otigen/releases)"; exit 1)
	@echo "OK"

# `otigen build` regenerates pyde_gen.go from otigen.toml and then invokes
# tinygo itself, so there's no separate compile step to run ahead of it.
build: check-tools
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
