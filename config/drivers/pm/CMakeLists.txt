# Copyright (c) 2023 Kan-Ru Chen
# SPDX-License-Identifier: MIT

zephyr_library_named(zmk__drivers__pm_wake_sensor)
zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)

zephyr_library_sources(pm_wake_sensor.c)
