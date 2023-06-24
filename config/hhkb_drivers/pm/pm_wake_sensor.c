/*
 * Copyright (c) 2023 Kan-Ru Chen
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_pm_wake_sensor

#include <device.h>
#include <drivers/gpio.h>

struct pm_wake_sensor_config
{
    struct gpio_dt_spec power;
    struct gpio_dt_spec sensor;
};

static int
pm_wake_sensor_init(const struct device *dev)
{
    const struct pm_wake_sensor_config *cfg = dev->config;
    gpio_pin_configure(cfg->power.port, cfg->power.pin, GPIO_OUTPUT_ACTIVE | cfg->power.dt_flags);
    gpio_pin_configure(cfg->sensor.port, cfg->sensor.pin, GPIO_INPUT | cfg->sensor.dt_flags);
    gpio_pin_interrupt_configure(cfg->sensor.port, cfg->sensor.pin, GPIO_INT_LEVEL_ACTIVE | cfg->sensor.dt_flags);

    return 0;
}

#define CREATE_PM_WAKE_SENSOR(inst)                          \
    static struct pm_wake_sensor_config config##inst = {     \
        .power = GPIO_DT_SPEC_INST_GET(inst, power_gpios),   \
        .sensor = GPIO_DT_SPEC_INST_GET(inst, sensor_gpios), \
    };                                                       \
    DEVICE_DT_INST_DEFINE(inst,                              \
                          pm_wake_sensor_init,               \
                          NULL,                              \
                          NULL,                              \
                          &config##inst,                     \
                          APPLICATION, 99,                   \
                          NULL);

DT_INST_FOREACH_STATUS_OKAY(CREATE_PM_WAKE_SENSOR)