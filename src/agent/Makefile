# AlterEgo agent: persona, its own conversational engine (brain.c) and the
# on-disk store. Static like init: no dependencies beyond libc.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic
LDFLAGS += -static

SRC = main.c persona.c brain.c utf8.c store.c
HDR = persona.h brain.h utf8.h store.h

all: alterego

alterego: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

check: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsyntax-only $(SRC)

clean:
	rm -f alterego

.PHONY: all check clean
