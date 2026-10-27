################################################################################
#
# alteregos: PID 1, the agent and the screen, from this repository's src/
#
################################################################################

ALTEREGOS_VERSION = local
ALTEREGOS_SITE = $(BR2_EXTERNAL_ALTEREGOS_PATH)/../src
ALTEREGOS_SITE_METHOD = local
ALTEREGOS_LICENSE = TBD

ALTEREGOS_MAKE_ENV = $(TARGET_MAKE_ENV) \
	CC="$(TARGET_CC)" \
	CFLAGS="$(TARGET_CFLAGS) -DALTEREGOS_VERSION=\\\"$(ALTEREGOS_GIT_VERSION)\\\"" \
	LDFLAGS="$(TARGET_LDFLAGS)"

# `git describe` of the repository, or "unknown" outside a checkout.
ALTEREGOS_GIT_VERSION = $(shell git -C $(BR2_EXTERNAL_ALTEREGOS_PATH) describe --always --dirty 2>/dev/null || echo unknown)

define ALTEREGOS_BUILD_CMDS
	$(ALTEREGOS_MAKE_ENV) $(MAKE) -C $(@D)/init
	$(ALTEREGOS_MAKE_ENV) $(MAKE) -C $(@D)/agent
	$(ALTEREGOS_MAKE_ENV) $(MAKE) -C $(@D)/screen
endef

define ALTEREGOS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/init/alteregos-init $(TARGET_DIR)/sbin/init
	$(INSTALL) -D -m 0755 $(@D)/agent/alterego $(TARGET_DIR)/usr/bin/alterego
	$(INSTALL) -D -m 0755 $(@D)/screen/alterego-screen $(TARGET_DIR)/usr/bin/alterego-screen
	$(INSTALL) -D -m 0644 $(@D)/screen/font.hex $(TARGET_DIR)/usr/share/alterego/font.hex
endef

$(eval $(generic-package))
