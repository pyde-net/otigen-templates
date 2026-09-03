# pyde-template — daily development loop.

.DEFAULT_GOAL := help

.PHONY: help check-tools install build asc-build test test-v test-vv test-vvv test-vvvv \
        deploy inspect verify clean

NAME := pyde-template

help:
	@printf "pyde-template — development targets:\n\n"
	@printf "  make install       npm install (assemblyscript + @pyde-net/host)\n"
	@printf "  make build         npm run build + otigen build\n"
	@printf "  make test          otigen test (default verbosity)\n"
	@printf "  make test-v        otigen test -v        (+ gas + duration)\n"
	@printf "  make test-vv       otigen test -vv       (+ events)\n"
	@printf "  make test-vvv      otigen test -vvv      (+ call traces)\n"
	@printf "  make test-vvvv     otigen test -vvvv     (+ storage diffs)\n"
	@printf "  make deploy        otigen deploy --network devnet\n"
	@printf "  make inspect       otigen inspect $$(NAME) --network devnet\n"
	@printf "  make verify        otigen verify  $$(NAME) --network devnet\n"
	@printf "  make clean         rm -rf build/ artifacts/ node_modules/\n"
	@printf "  make check-tools   verify node / npm / asc / otigen are present\n"

check-tools:
	@command -v node   >/dev/null || (echo "missing: node (install via https://nodejs.org)"; exit 1)
	@command -v npm    >/dev/null || (echo "missing: npm  (ships with node)"; exit 1)
	@command -v otigen >/dev/null || (echo "missing: otigen (install via https://github.com/pyde-net/otigen/releases)"; exit 1)
	@[ -f node_modules/.bin/asc ] || (echo "missing: assemblyscript (run \`make install\`)"; exit 1)
	@echo "OK"

install:
	npm install

asc-build:
	npm run build

build: check-tools asc-build
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
	rm -rf build/ artifacts/ node_modules/
