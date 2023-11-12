# Copyright (c) 2022 Kan-Ru Chen
# SPDX-License-Identifier: MIT

zephyr_library_named(zmk__hhkb_drivers__kscan_topre)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

zephyr_library_sources(kscan_hhkb_pro2.c)
