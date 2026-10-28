# The AlterEgOS screen: framebuffer desktop with the chat and a shell.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic
LDFLAGS += -static

SRC  = screen.c canvas.c dict.c fb.c font.c ime.c input.c kana.c pty.c term.c ../agent/utf8.c
TSRC = test.c canvas.c dict.c font.c ime.c kana.c term.c ../agent/utf8.c

all: alterego-screen

alterego-screen: $(SRC) canvas.h dict.h fb.h font.h ime.h input.h kana.h pty.h term.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

screen-test: $(TSRC) canvas.h dict.h font.h ime.h kana.h term.h
	$(CC) $(CFLAGS) -o $@ $(TSRC) $(LDFLAGS)

test: screen-test
	./screen-test font.hex dict.txt

check:
	for f in $(SRC) test.c; do $(CC) $(CFLAGS) -fsyntax-only $$f || exit 1; done

clean:
	rm -f alterego-screen screen-test

.PHONY: all test check clean
