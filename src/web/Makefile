# alterego-web: the browser. libc, plus mbedTLS for https when the compiler
# finds it (the Buildroot sysroot has it; on a build host: libmbedtls-dev).
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=c17 -Wall -Wextra -Werror -Wpedantic -Wno-format-truncation   # URLs are cut to their buffers on purpose
LDFLAGS += -static

HAVE_MBEDTLS := $(shell echo '\#include <mbedtls/ssl.h>' | $(CC) $(CFLAGS) -E -x c - >/dev/null 2>&1 && echo yes)
ifeq ($(HAVE_MBEDTLS),yes)
CFLAGS += -DHAVE_TLS
LDLIBS += -lmbedtls -lmbedx509 -lmbedcrypto
endif

SRC  = main.c net.c html.c url.c ../agent/utf8.c
TSRC = test.c net.c html.c url.c ../agent/utf8.c

all: alterego-web

alterego-web: $(SRC) web.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

web-test: $(TSRC) web.h
	$(CC) $(CFLAGS) -o $@ $(TSRC) $(LDFLAGS) $(LDLIBS)

test: web-test
	./web-test

check:
	for f in $(SRC) test.c; do $(CC) $(CFLAGS) -fsyntax-only $$f || exit 1; done

clean:
	rm -f alterego-web web-test

.PHONY: all test check clean
