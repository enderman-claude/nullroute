# NULLROUTE build targets. Everything except the launcher is plain sh.

VERSION := $(shell sed -n 's/^NR_VERSION="\(.*\)"/\1/p' game/lib/core.sh)
CC      ?= cc
WINCC   ?= x86_64-w64-mingw32-gcc
SHELL_UNDER_TEST ?= sh

CFLAGS  := -std=c99 -Wall -Wextra -Werror -pedantic

.PHONY: all test lint launcher launcher-win package clean

all: launcher

test:
	NR_TEST_SHELL="$(SHELL_UNDER_TEST)" sh tests/run.sh

lint:
	shellcheck -s sh game/bin/nullroute game/lib/*.sh tests/run.sh tools/package.sh

build/nullroute: launcher/nullroute.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

build/nullroute.exe: launcher/nullroute.c
	@mkdir -p build
	$(WINCC) -std=c99 -Wall -Wextra -Werror -o $@ $<

launcher: build/nullroute

launcher-win: build/nullroute.exe

package: launcher launcher-win
	sh tools/package.sh linux-x86_64 build/nullroute nullroute tar.gz
	sh tools/package.sh windows-x86_64 build/nullroute.exe nullroute.exe zip

clean:
	rm -rf build dist
