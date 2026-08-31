# HealthyLink Expansion Port Driver
# Copyright (c) 2025 ProtoCentral
# SPDX-License-Identifier: MIT

zephyr_library()

# Always include core driver and pinmux when HEALTHYLINK is enabled
zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK
    healthylink_core.c
    healthylink_pinmux.c
)

# Module-specific drivers
zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK_EEG_8CH
    healthylink_eeg.c
)

zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK_CAN_INTERFACE
    healthylink_can.c
)

zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK_TRIGGER_IO
    healthylink_trigger.c
)

zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK_COMPUTE
    healthylink_compute.c
)

# LLEXT loader for dynamic module loading
zephyr_library_sources_ifdef(CONFIG_HEALTHYLINK_LLEXT_FALLBACK
    healthylink_llext.c
)

# Include paths. The repo-wide include/ is already added globally by the
# top-level CMakeLists (zephyr_include_directories(include)), so we only add
# this driver's own dir. (Previously referenced the other repo's module dir
# via ${ZEPHYR_HEALTHYPI_6_STM32_MODULE_DIR}, which is empty now that
# fw-staging is standalone -- that produced a bogus -I/include.)
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
