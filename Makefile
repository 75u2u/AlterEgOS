# AlterEgOS top-level targets. The real build is Buildroot (scripts/build.sh);
# the targets here are the fast paths used day to day and in CI.

ARCH ?= x86_64

.PHONY: all check host clean x86_64 aarch64 run-x86_64 run-aarch64

all: host

# Build init and the agent with the host compiler. Not bootable; this is the
# compile/warning check and the way to run the agent locally.
host:
	$(MAKE) -C src/init
	$(MAKE) -C src/agent

# Syntax-only check, also for a cross compiler: make check CC=aarch64-linux-gnu-gcc
check:
	$(MAKE) -C src/init check
	$(MAKE) -C src/agent check

# Full OS images via Buildroot.
x86_64 aarch64:
	scripts/build.sh $@

run-x86_64 run-aarch64:
	scripts/run-qemu.sh $(@:run-%=%)

clean:
	$(MAKE) -C src/init clean
	$(MAKE) -C src/agent clean
