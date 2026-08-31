/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linker fragment backing HL_MODULE_REGISTER() / STRUCT_SECTION_ITERABLE(
 * hl_module_provider, ...) in src/healthylink/. This board uses the GNU ld
 * template linker (CONFIG_CMAKE_LINKER_GENERATOR=n), so zephyr_iterable_section()
 * is a no-op here; the iterable section must be declared via an .ld fragment
 * wired with zephyr_linker_sources(SECTIONS ...). Without it,
 * STRUCT_SECTION_FOREACH has no _hl_module_provider_list_{start,end} symbols.
 *
 * Providers are placed in the RAM data region (loaded from ROM at boot).
 */

ITERABLE_SECTION_RAM(hl_module_provider, Z_LINK_ITERABLE_SUBALIGN)
