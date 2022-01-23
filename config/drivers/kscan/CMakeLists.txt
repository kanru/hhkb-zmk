# Copyright (c) 2022 Kan-Ru Chen
# SPDX-License-Identifier: MIT

zephyr_library_named(zmk__drivers__kscan_topre)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

zephyr_library_sources(kscan_gpio_topre.c)
