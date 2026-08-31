/*
 * Copyright (c) 2023 BrainCo Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* SRAM1: 128KB @ 0x30000000 - Use for LVGL memory pool and large buffers */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(sram1), okay)
GROUP_START(SRAM1)

	SECTION_PROLOGUE(_STM32_SRAM1_SECTION_NAME, (NOLOAD),)
	{
		/* LVGL memory pool - moved from main RAM */
		*(.sram1_lvgl_pool)
		*(.sram1_buffers)
		/* Network buffers could go here too */
		*(.sram1_net)
	} GROUP_LINK_IN(SRAM1)

GROUP_END(SRAM1)
#endif

/* SRAM2: 128KB @ 0x30020000 - Use for heap and secondary buffers */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(sram2), okay)
GROUP_START(SRAM2)

	SECTION_PROLOGUE(_STM32_SRAM2_SECTION_NAME, (NOLOAD),)
	{
		/* System heap - moved from main RAM */
		*(.sram2_heap)
		*(.sram2_buffers)
		/* MQTT/WiFi buffers could go here */
		*(.sram2_mqtt)
	} GROUP_LINK_IN(SRAM2)

GROUP_END(SRAM2)
#endif

/* SRAM3: 32KB @ 0x30040000 - Reserve for future use or small allocations */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(sram3), okay)
GROUP_START(SRAM3)

	SECTION_PROLOGUE(_STM32_SRAM3_SECTION_NAME, (NOLOAD),)
	{
		*(.sram3_buffers)
	} GROUP_LINK_IN(SRAM3)

GROUP_END(SRAM3)
#endif

/* SDRAM1: 32MB W9825G6KH @ 0xC0000000
 *
 * Memory Layout (shared M7/M4):
 * 0xC0000000 - 0xC03FFFFF: LVGL display buffers (4 MB)
 * 0xC0400000 - 0xC04FFFFF: M7 TFLite arena (1 MB) - currently disabled
 * 0xC0500000 - 0xC05FFFFF: M4 TFLite tensor arena (1 MB)
 * 0xC0600000 - 0xC060FFFF: M4 ECG classification buffer (64 KB)
 * 0xC0610000 - 0xC0610BFF: M4 Beat window buffer (3 KB)
 * 0xC0700000 - 0xC077FFFF: M4 Model cache (512 KB)
 * 0xC0800000 - 0xC08FFFFF: Recording module buffers (1 MB)
 * 0xC0900000+: Available for future use
 *
 * NOTE: SDRAM2 (0xD0000000) does NOT exist on HealthyPi 6.
 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram1), okay)
GROUP_START(SDRAM1)

	SECTION_PROLOGUE(_STM32_SDRAM1_SECTION_NAME, (NOLOAD),)
	{
		*(.lvgl_buf)
		/* Recording module and other SDRAM1 allocations */
		KEEP(*(LINKER_DT_NODE_REGION_NAME(DT_N_S_sdram_c0000000)))
	} GROUP_LINK_IN(SDRAM1)

GROUP_END(SDRAM1)
#endif
