# SPDX-License-Identifier: Apache-2.0


zephyr_library_amend()
zephyr_include_directories(.)
zephyr_library_sources_ifdef(CONFIG_ILI9881C_DSI	display_ili9881c_dsi.c)
zephyr_library_sources_ifdef(CONFIG_GC9503V_DSI		display_gc9503v_dsi.c)


