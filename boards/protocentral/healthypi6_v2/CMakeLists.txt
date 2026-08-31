#
# Copyright 2023 BrainCo Inc.
#
# SPDX-License-Identifier: Apache-2.0
#

# Add custom linker sections for external memory regions
# - SDRAM1/SDRAM2: Used by M7 for LVGL buffers, M4 for TFLite tensor arena
# - SRAM1/SRAM2/SRAM3: Additional RAM regions
zephyr_linker_sources(SECTIONS dc_ram.ld)
