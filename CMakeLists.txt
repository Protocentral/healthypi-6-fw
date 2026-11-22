# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# This CMake file is picked by the Zephyr build system because it is defined
# as the module CMake entry point (see zephyr/module.yml).

zephyr_include_directories(include)

add_subdirectory(drivers)

# MCUboot only: a key table with the backup release key in it
# (CONFIG_HPI_BACKUP_SIGNING_KEY; Kconfig there says why). This module is part
# of every image in the sysbuild, the bootloader included, so this is where the
# table can be added without forking MCUboot. The table replaces the one-entry
# table in MCUboot's boot/zephyr/keys.c, which zephyr/patches.yml allows; an
# unpatched MCUboot defines bootutil_keys too, and the link fails.
if(CONFIG_MCUBOOT AND NOT CONFIG_HPI_BACKUP_SIGNING_KEY STREQUAL "")
  if(NOT CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256 OR CONFIG_BOOT_HW_KEY OR CONFIG_BOOT_BUILTIN_KEY)
    message(FATAL_ERROR "CONFIG_HPI_BACKUP_SIGNING_KEY needs ECDSA-P256 keys built into MCUboot")
  endif()
  set(hpi_boot_keys "${CONFIG_BOOT_SIGNATURE_KEY_FILE}" "${CONFIG_HPI_BACKUP_SIGNING_KEY}")
  foreach(k ${hpi_boot_keys})
    if(NOT IS_ABSOLUTE "${k}" OR NOT EXISTS "${k}")
      message(FATAL_ERROR
        "MCUboot key table: '${k}' must be an existing absolute path "
        "(CONFIG_BOOT_SIGNATURE_KEY_FILE, CONFIG_HPI_BACKUP_SIGNING_KEY)")
    endif()
  endforeach()
  # Generated at configure time: a custom command's output in this directory
  # is not visible to MCUboot's `app` target, which lives in another one. The
  # key files are configure dependencies, so changing one reconfigures.
  set(hpi_boot_keys_c ${CMAKE_CURRENT_BINARY_DIR}/hpi_mcuboot_keys.c)
  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/build/gen_m4_pubkey.py
            --format mcuboot
            --key "${CONFIG_BOOT_SIGNATURE_KEY_FILE}"
            --key "${CONFIG_HPI_BACKUP_SIGNING_KEY}"
            --output ${hpi_boot_keys_c}
    RESULT_VARIABLE hpi_boot_keys_rc
  )
  if(NOT hpi_boot_keys_rc EQUAL 0)
    message(FATAL_ERROR "MCUboot key table: gen_m4_pubkey.py failed")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${hpi_boot_keys} ${CMAKE_CURRENT_LIST_DIR}/tools/build/gen_m4_pubkey.py)
  target_sources(app PRIVATE ${hpi_boot_keys_c})
  zephyr_compile_definitions(MCUBOOT_KEYS_EXTERNAL)
  message(STATUS "MCUboot key table: ${CONFIG_BOOT_SIGNATURE_KEY_FILE} + backup ${CONFIG_HPI_BACKUP_SIGNING_KEY}")
endif()
