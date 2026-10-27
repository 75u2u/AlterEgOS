# AlterEgOS top-level targets. The real build is Buildroot (scripts/build.sh);
# the targets here are the fast paths used day to day and in CI.

ARCH ?= x86_64

.PHONY: all check host clean x86_64 aarch64 run-x86_64 run-aarch64 run-efi boot-test-x86_64 boot-test-aarch64 boot-test-efi

all: host

# Build init and the agent with the host compiler. Not bootable; this is the
# compile/warning check and the way to run the agent locally.
host:
	$(MAKE) -C src/init
	$(MAKE) -C src/agent
	$(MAKE) -C src/screen

# Syntax-only check, also for a cross compiler: make check CC=aarch64-linux-gnu-gcc
check:
	$(MAKE) -C src/init check
	$(MAKE) -C src/agent check
	$(MAKE) -C src/screen check

# Full OS images via Buildroot.
x86_64 aarch64:
	scripts/build.sh $@

run-x86_64 run-aarch64:
	scripts/run-qemu.sh $(@:run-%=%)

# Unattended boot checks (what CI runs): init up and AlterEgo greeting on serial;
# boot-test-efi boots the USB image through UEFI firmware (needs ovmf).
boot-test-x86_64 boot-test-aarch64:
	scripts/boot-test.sh $(@:boot-test-%=%)

boot-test-efi:
	scripts/boot-test-efi.sh

run-efi:
	scripts/run-qemu.sh x86_64 --efi

clean:
	$(MAKE) -C src/init clean
	$(MAKE) -C src/agent clean
	$(MAKE) -C src/screen clean
