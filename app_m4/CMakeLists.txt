#-------------------------------------------------------------------------------
# Zephyr M4 Application (Algorithm Processor)
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# Copyright (c) 2025 ProtoCentral Electronics
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# M4 is a calculations-only core (QRS/HR/HRV, SpO2) - C only.
# Arrhythmia ML classification lives on the HealthyLink Compute (NPU) module.
project(app LANGUAGES C)

# Application source files (C)
FILE(GLOB app_sources_c src/*.c)

# Add ring buffer implementation from parent directory
target_sources(app PRIVATE
    ${app_sources_c}
    ../src/ring_buffer.c
)

# Include directories
zephyr_include_directories(src)