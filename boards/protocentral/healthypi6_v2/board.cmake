# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2019 STMicroelectronics

board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")

if(CONFIG_BOARD_HEALTHYPI6_V2_STM32H757XX_M7)
  board_runner_args(jlink "--device=STM32H757BI_M7")
  board_runner_args(openocd "--config=${BOARD_DIR}/support/openocd_healthypi6_stm32_m7.cfg")
  board_runner_args(openocd --target-handle=_CHIPNAME.cpu0)
elseif(CONFIG_BOARD_HEALTHYPI6_V2_STM32H757XX_M4)
  board_runner_args(jlink "--device=STM32H757BI_M4")
  board_runner_args(openocd "--config=${BOARD_DIR}/support/openocd_healthypi6_stm32_m4.cfg")
  board_runner_args(openocd --target-handle=_CHIPNAME.cpu1)
endif()

# keep first
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
