/*
 * Copyright (c) 2022, 2023 Kan-Ru Chen
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_hhkb_pro2

#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MATRIX_ROWS 8
#define MATRIX_COLS 8
#define MATRIX_CELLS (MATRIX_ROWS * MATRIX_COLS)
#define SEL_PINS 6

struct kscan_hhkb_pro2_config
{
    struct gpio_dt_spec bits[SEL_PINS];
    struct gpio_dt_spec power;
    struct gpio_dt_spec key;
    struct gpio_dt_spec hys;
    struct gpio_dt_spec strobe;
    const uint16_t matrix_relax_us;
    const uint16_t adc_read_settle_us;
    const uint16_t active_polling_interval_ms;
    const uint16_t idle_polling_interval_ms;
    const uint16_t sleep_polling_interval_ms;
};

struct kscan_hhkb_pro2_data
{
    kscan_callback_t callback;
    struct k_timer poll_timer;
    struct k_work poll;
    bool matrix_state[MATRIX_CELLS];
    const struct device *dev;
};

static int kscan_hhkb_pro2_configure(const struct device *dev, kscan_callback_t callback)
{
    LOG_DBG("KSCAN API configure");
    struct kscan_hhkb_pro2_data *data = dev->data;
    if (!callback)
    {
        return -EINVAL;
    }
    data->callback = callback;
    LOG_DBG("Configured KSCAN");
    return 0;
}

static int kscan_hhkb_pro2_enable(const struct device *dev)
{
    LOG_DBG("KSCAN API enable");
    struct kscan_hhkb_pro2_data *data = dev->data;
    const struct kscan_hhkb_pro2_config *cfg = dev->config;
    k_timer_start(&data->poll_timer,
                  K_MSEC(cfg->active_polling_interval_ms),
                  K_MSEC(cfg->active_polling_interval_ms));
    return 0;
}

static int kscan_hhkb_pro2_disable(const struct device *dev)
{
    LOG_DBG("KSCAN API disable");
    struct kscan_hhkb_pro2_data *data = dev->data;
    k_timer_stop(&data->poll_timer);
    return 0;
}

static void kscan_hhkb_pro2_timer_handler(struct k_timer *timer)
{
    struct kscan_hhkb_pro2_data *data =
        CONTAINER_OF(timer, struct kscan_hhkb_pro2_data, poll_timer);
    k_work_submit(&data->poll);
}

static void kscan_hhkb_pro2_work_handler(struct k_work *work)
{
    struct kscan_hhkb_pro2_data *data = CONTAINER_OF(work, struct kscan_hhkb_pro2_data, poll);
    const struct device *dev = data->dev;
    const struct kscan_hhkb_pro2_config *cfg = dev->config;
    bool matrix_read[MATRIX_CELLS];

    // Power on everything
    gpio_pin_configure(cfg->key.port, cfg->key.pin, GPIO_INPUT | cfg->key.dt_flags);
    gpio_pin_set(cfg->strobe.port, cfg->strobe.pin, 1);
    // gpio_pin_set(cfg->power.port, cfg->power.pin, 1);
    // Topre controller board needs 5 ms to be operational
    k_sleep(K_MSEC(5));
    for (int r = 0; r < MATRIX_ROWS; ++r)
    {
        for (int c = 0; c < MATRIX_COLS; ++c)
        {
            gpio_pin_set(cfg->bits[0].port, cfg->bits[0].pin, r & BIT(0));
            gpio_pin_set(cfg->bits[1].port, cfg->bits[1].pin, r & BIT(1));
            gpio_pin_set(cfg->bits[2].port, cfg->bits[2].pin, r & BIT(2));
            gpio_pin_set(cfg->bits[3].port, cfg->bits[3].pin, c & BIT(0));
            gpio_pin_set(cfg->bits[4].port, cfg->bits[4].pin, c & BIT(1));
            gpio_pin_set(cfg->bits[5].port, cfg->bits[5].pin, c & BIT(2));

            int cell = (r * MATRIX_COLS) + c;
            const bool prev = data->matrix_state[cell];
            gpio_pin_set(cfg->hys.port, cfg->hys.pin, prev);

            k_busy_wait(cfg->matrix_relax_us);

            const unsigned int lock = irq_lock();
            // Pull low strobe line to trigger sensing
            gpio_pin_set(cfg->strobe.port, cfg->strobe.pin, 0);
            gpio_pin_set(cfg->strobe.port, cfg->strobe.pin, 1);
            k_busy_wait(cfg->adc_read_settle_us);
            const bool pressed = gpio_pin_get(cfg->key.port, cfg->key.pin);
            irq_unlock(lock);
            gpio_pin_set(cfg->hys.port, cfg->hys.pin, 0);

            matrix_read[cell] = pressed;
        }
    }
    // Set all gpio pins to low and power off the controller board to avoid
    // current leakage.
    for (int i = 0; i < SEL_PINS; ++i)
    {
        gpio_pin_set(cfg->bits[i].port, cfg->bits[i].pin, 0);
    }
    gpio_pin_configure(cfg->key.port, cfg->key.pin, GPIO_DISCONNECTED);
    // gpio_pin_set(cfg->power.port, cfg->power.pin, 0);
    gpio_pin_set(cfg->strobe.port, cfg->strobe.pin, 0);

    for (int r = 0; r < MATRIX_ROWS; ++r)
    {
        for (int c = 0; c < MATRIX_COLS; ++c)
        {
            int cell = (r * MATRIX_COLS) + c;
            if (data->matrix_state[cell] != matrix_read[cell])
            {
                data->matrix_state[cell] = matrix_read[cell];
                data->callback(data->dev, r, c, matrix_read[cell]);
            }
        }
    }
}

static int kscan_hhkb_pro2_activity_event_handler(const struct device *dev, const zmk_event_t *eh)
{
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL)
    {
        return -ENOTSUP;
    }
    struct kscan_hhkb_pro2_data *data = dev->data;
    const struct kscan_hhkb_pro2_config *cfg = dev->config;
    uint16_t poll_interval;
    switch (ev->state)
    {
    case ZMK_ACTIVITY_ACTIVE:
        poll_interval = cfg->active_polling_interval_ms;
        break;
    case ZMK_ACTIVITY_IDLE:
        poll_interval = cfg->idle_polling_interval_ms;
        break;
    case ZMK_ACTIVITY_SLEEP:
        poll_interval = cfg->sleep_polling_interval_ms;
        break;
    default:
        LOG_WRN("Unhandled activity state: %d", ev->state);
        return -EINVAL;
    }
    LOG_DBG("Setting poll interval to %d", poll_interval);
    k_timer_start(&data->poll_timer, K_MSEC(poll_interval), K_MSEC(poll_interval));
    return 0;
}

static int kscan_hhkb_pro2_init(const struct device *dev)
{
    LOG_DBG("KSCAN init");
    struct kscan_hhkb_pro2_data *data = dev->data;
    const struct kscan_hhkb_pro2_config *cfg = dev->config;
    data->dev = dev;
    for (int i = 0; i < SEL_PINS; ++i)
    {
        gpio_pin_configure(cfg->bits[i].port,
                           cfg->bits[i].pin,
                           GPIO_OUTPUT_INACTIVE | cfg->bits[i].dt_flags);
    }
    // The power line needs to source more than 0.5 mA current. Set the GPIO
    // drive mode to high drive.
    gpio_pin_configure(cfg->power.port, cfg->power.pin, GPIO_OUTPUT_ACTIVE | cfg->power.dt_flags);
    // Disconnect input pin to save power.
    gpio_pin_configure(cfg->key.port, cfg->key.pin, GPIO_DISCONNECTED);
    gpio_pin_configure(cfg->hys.port, cfg->hys.pin, GPIO_OUTPUT_INACTIVE | cfg->hys.dt_flags);
    gpio_pin_configure(cfg->strobe.port, cfg->strobe.pin, GPIO_OUTPUT_INACTIVE | cfg->strobe.dt_flags);

    k_timer_init(&data->poll_timer, kscan_hhkb_pro2_timer_handler, NULL);
    k_work_init(&data->poll, kscan_hhkb_pro2_work_handler);

    return 0;
}
static const struct kscan_driver_api kscan_hhkb_pro2_api = {
    .config = kscan_hhkb_pro2_configure,
    .enable_callback = kscan_hhkb_pro2_enable,
    .disable_callback = kscan_hhkb_pro2_disable,
};

#define CREATE_kscan_hhkb_pro2(inst)                                                       \
    static struct kscan_hhkb_pro2_data kscan_hhkb_pro2_data##inst;                         \
    static const struct kscan_hhkb_pro2_config kscan_hhkb_pro2_config##inst = {            \
        .bits =                                                                            \
            {                                                                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 3),                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 4),                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 5),                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 6),                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 7),                              \
                GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 8),                              \
            },                                                                             \
        .power = GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 0),                             \
        .key = GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 1),                               \
        .hys = GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 2),                               \
        .strobe = GPIO_DT_SPEC_INST_GET_BY_IDX(inst, gpios, 9),                            \
        .matrix_relax_us = DT_INST_PROP(inst, matrix_relax_us),                            \
        .adc_read_settle_us = DT_INST_PROP(inst, adc_read_settle_us),                      \
        .active_polling_interval_ms = DT_INST_PROP(inst, active_polling_interval_ms),      \
        .idle_polling_interval_ms = DT_INST_PROP(inst, idle_polling_interval_ms),          \
        .sleep_polling_interval_ms = DT_INST_PROP(inst, sleep_polling_interval_ms),        \
    };                                                                                     \
    static int kscan_hhkb_pro2_activity_event_handler_wrapper##inst(const zmk_event_t *eh) \
    {                                                                                      \
        const struct device *dev = DEVICE_DT_INST_GET(inst);                               \
        return kscan_hhkb_pro2_activity_event_handler(dev, eh);                            \
    }                                                                                      \
    ZMK_LISTENER(kscan_hhkb_pro2##inst,                                                    \
                 kscan_hhkb_pro2_activity_event_handler_wrapper##inst);                    \
    ZMK_SUBSCRIPTION(kscan_hhkb_pro2##inst, zmk_activity_state_changed);                   \
    DEVICE_DT_INST_DEFINE(inst,                                                            \
                          kscan_hhkb_pro2_init,                                            \
                          NULL,                                                            \
                          &kscan_hhkb_pro2_data##inst,                                     \
                          &kscan_hhkb_pro2_config##inst,                                   \
                          APPLICATION,                                                     \
                          CONFIG_APPLICATION_INIT_PRIORITY,                                \
                          &kscan_hhkb_pro2_api);

DT_INST_FOREACH_STATUS_OKAY(CREATE_kscan_hhkb_pro2)
