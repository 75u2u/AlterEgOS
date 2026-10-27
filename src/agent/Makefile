# AlterEgo agent: persona, the conversation over the Claude API (libcurl),
# the self-projection learning and the on-disk store. On the target it is
# linked static like init (Buildroot passes STATIC=1 and a pkg-config that
# resolves libcurl's static dependencies); on a host it links dynamically.
CC         ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2
CFLAGS     += -std=c17 -Wall -Wextra -Werror -Wpedantic
CFLAGS     += $(shell $(PKG_CONFIG) --cflags libcurl)
LDLIBS     += $(shell $(PKG_CONFIG) --libs libcurl)
ifeq ($(STATIC),1)
LDFLAGS    += -static
endif

SRC = main.c persona.c learn.c json.c api.c store.c
HDR = persona.h learn.h json.h api.h store.h

all: alterego

alterego: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

check: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsyntax-only $(SRC)

clean:
	rm -f alterego

.PHONY: all check clean
