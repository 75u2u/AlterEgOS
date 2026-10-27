# The AlterEgOS screen: framebuffer desktop with the chat and a shell.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic
LDFLAGS += -static

SRC  = screen.c canvas.c fb.c font.c input.c kana.c pty.c term.c ../agent/utf8.c
TSRC = test.c canvas.c font.c kana.c term.c ../agent/utf8.c

all: alterego-screen

alterego-screen: $(SRC) canvas.h fb.h font.h input.h kana.h pty.h term.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

screen-test: $(TSRC) canvas.h font.h kana.h term.h
	$(CC) $(CFLAGS) -o $@ $(TSRC) $(LDFLAGS)

test: screen-test
	./screen-test font.hex

check:
	for f in $(SRC) test.c; do $(CC) $(CFLAGS) -fsyntax-only $$f || exit 1; done

clean:
	rm -f alterego-screen screen-test

.PHONY: all test check clean
