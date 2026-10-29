include $(sort $(wildcard $(BR2_EXTERNAL_ALTEREGOS_PATH)/package/*/*.mk))


# CMake 4 (Kali, Debian sid, Fedora) refuses projects whose
# cmake_minimum_required is below 3.5; mbedtls 2.28's says 2.8.12. Tell CMake
# to configure it anyway (newer Buildroot does this for every cmake package).
MBEDTLS_CONF_OPTS += -DCMAKE_POLICY_VERSION_MINIMUM=3.5
