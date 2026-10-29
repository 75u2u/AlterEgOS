# AlterEgo agent: persona, its own conversational engine (brain.c) and the
# on-disk store. Static like init: no dependencies beyond libc.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic
LDFLAGS += -static -lm

SRC = main.c persona.c brain.c utf8.c store.c skills.c apps.c drawings.c shapes.c body.c llm.c
HDR = persona.h brain.h utf8.h store.h skills.h apps.h drawings.h shapes.h body.h llm.h

all: alterego

alterego: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

check: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsyntax-only $(SRC)

# The language model's tests: the gradients against finite differences
# (in double), training, save/load, generation.
test: llm_test llm_test_double
	./llm_test_double
	./llm_test

llm_test: llm_test.c llm.c utf8.c llm.h utf8.h
	$(CC) $(CFLAGS) -o $@ llm_test.c llm.c utf8.c $(LDFLAGS)

llm_test_double: llm_test.c llm.c utf8.c llm.h utf8.h
	$(CC) $(CFLAGS) -DLLM_NUM=double -o $@ llm_test.c llm.c utf8.c $(LDFLAGS)

clean:
	rm -f alterego llm_test llm_test_double

.PHONY: all check test clean
