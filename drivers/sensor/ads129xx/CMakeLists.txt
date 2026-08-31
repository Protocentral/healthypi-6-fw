# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_library()    
zephyr_library_sources(ads129xx.c ads129xx_async.c)
