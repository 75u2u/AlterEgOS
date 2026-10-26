# PID 1 for AlterEgOS. Static so it needs nothing from the rootfs to start.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic -Wno-unused-result
LDFLAGS += -static

all: alteregos-init

alteregos-init: init.c init.h
	$(CC) $(CFLAGS) -o $@ init.c $(LDFLAGS)

# Compile-only check used by CI and by `make check` at the top level.
check: init.c init.h
	$(CC) $(CFLAGS) -fsyntax-only init.c

clean:
	rm -f alteregos-init

.PHONY: all check clean
