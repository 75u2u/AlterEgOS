include $(sort $(wildcard $(BR2_EXTERNAL_ALTEREGOS_PATH)/package/*/*.mk))

# The cross compiler (gcc 13.3) is C++11-era code: build it as gnu++17
# whatever the host compiler's default dialect is (GCC 16 defaults to C++20,
# which breaks its libcody and may break more). CONF_ENV comes after
# HOST_CONFIGURE_OPTS on the configure line, so this CXXFLAGS wins.
HOST_GCC_INITIAL_CONF_ENV += CXXFLAGS="$(HOST_CXXFLAGS) -std=gnu++17"
HOST_GCC_FINAL_CONF_ENV += CXXFLAGS="$(HOST_CXXFLAGS) -std=gnu++17"
