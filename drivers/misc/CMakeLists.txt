# Copyright (c) 2025 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

# HealthyBridge ESP32 host link (UART4 + hardware RTS/CTS) - enabled via devicetree.
add_subdirectory_ifdef(CONFIG_DT_HAS_PROTOCENTRAL_HEALTHYBRIDGE_ESP32_UART_ENABLED
                       healthybridge_esp32)

# HealthyLink Expansion Port driver
add_subdirectory_ifdef(CONFIG_HEALTHYLINK healthylink)

# MP2696B battery charger (I2C)
add_subdirectory_ifdef(CONFIG_MP2696 mp2696)
