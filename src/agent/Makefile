# AlterEgo agent. Day 1: persona + echo. Linked static like init so a broken
# shared-library setup on the target cannot take the agent down with it.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic
LDFLAGS += -static

SRC = main.c persona.c

all: alterego

alterego: $(SRC) persona.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

check: $(SRC) persona.h
	$(CC) $(CFLAGS) -fsyntax-only $(SRC)

clean:
	rm -f alterego

.PHONY: all check clean
