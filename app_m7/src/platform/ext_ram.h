/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HPI_EXT_RAM -- place a static object in the 32 MB external SDRAM (sdram1,
 * 0xC0000000) instead of the M7's 512 KB internal AXI SRAM.
 *
 * Only for buffers that meet ALL of these:
 *   - touched from thread context only -- never from an ISR (and never a
 *     thread stack: exception entry stacks onto it);
 *   - not a DMA source/target (SDMMC IDMA, UART/SPI DMA, USB): SDRAM is
 *     mapped cacheable, and nothing here does cache maintenance;
 *   - not used before the FMC/SDRAM controller is up (memc, POST_KERNEL
 *     priority CONFIG_MEMC_INIT_PRIORITY).
 *
 * The region is NOLOAD: it is NOT zeroed at boot and has no initialiser
 * image. An object placed here must not rely on C's zero-init of statics --
 * clear it, or track its valid length in an ordinary (internal) variable.
 */

#ifndef HPI_PLATFORM_EXT_RAM_H
#define HPI_PLATFORM_EXT_RAM_H

#include <zephyr/devicetree.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/toolchain.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram1), okay)
#define HPI_EXT_RAM  Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(sdram1)))
#else
#define HPI_EXT_RAM
#endif

#endif /* HPI_PLATFORM_EXT_RAM_H */
