# Texas Instruments ADS1299 8-Channel EEG AFE Driver
# Copyright (c) 2025 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_library_sources(ads1299.c)

# Expose ads1299.h to the whole build (the app's healthylink mod_eeg.c includes
# it), not just this driver's own sources. zephyr_library_include_directories()
# would only reach the driver -- matches the ads129xx/afe4400 drivers.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
