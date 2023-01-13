/*
 * Copyright (c) 2022 Kan-Ru Chen
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_gpio_topre

#include <device.h>
#include <drivers/kscan.h>
#include <drivers/gpio.h>
#include <logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MATRIX_ROWS 8
#define MATRIX_COLS 8
#define MATRIX_CELLS (MATRIX_ROWS * MATRIX_COLS)
#define SEL_PINS 6

struct kscan_gpio_item_config
{
    char *label;
    gpio_pin_t pin;
    gpio_flags_t flags;
};

#define KSCAN_GPIO_TOPRE_ITEM_CFG(n, idx)                  \
    {                                                      \
        .label = DT_INST_GPIO_LABEL_BY_IDX(n, gpios, idx), \
        .pin = DT_INST_GPIO_PIN_BY_IDX(n, gpios, idx),     \
        .flags = DT_INST_GPIO_FLAGS_BY_IDX(n, gpios, idx), \
    }

struct kscan_gpio_topre_config
{
    struct kscan_gpio_item_config bits[SEL_PINS];
    struct kscan_gpio_item_config power;
    struct kscan_gpio_item_config key;
    struct kscan_gpio_item_config hys;
    struct kscan_gpio_item_config strobe;
    const uint16_t matrix_relax_us;
    const uint16_t adc_read_settle_us;
    const uint16_t active_polling_interval_ms;
    const uint16_t idle_polling_interval_ms;
    const uint16_t sleep_polling_interval_ms;
};

struct kscan_gpio_topre_data
{
    kscan_callback_t callback;
    struct k_timer poll_timer;
    struct k_work poll;
    bool matrix_state[MATRIX_CELLS];
    const struct device *bits[SEL_PINS];
    const struct device *power;
    const struct device *key;
    const struct device *hys;
    const struct device *strobe;
    const struct device *dev;
};

static int kscan_gpio_topre_configure(const struct device *dev, kscan_callback_t callback)
{
    LOG_DBG("KSCAN API configure");
    struct kscan_gpio_topre_data *data = dev->data;
    if (!callback)
    {
        return -EINVAL;
    }
    data->callback = callback;
    LOG_DBG("Configured KSCAN");
    return 0;
}

static int kscan_gpio_topre_enable(const struct device *dev)
{
    LOG_DBG("KSCAN API enable");
    struct kscan_gpio_topre_data *data = dev->data;
    const struct kscan_gpio_topre_config *cfg = dev->config;
    k_timer_start(&data->poll_timer, K_MSEC(cfg->active_polling_interval_ms),
                  K_MSEC(cfg->active_polling_interval_ms));
    return 0;
}

static int kscan_gpio_topre_disable(const struct device *dev)
{
    LOG_DBG("KSCAN API disable");
    struct kscan_gpio_topre_data *data = dev->data;
    k_timer_stop(&data->poll_timer);
    return 0;
}

static void kscan_gpio_topre_timer_handler(struct k_timer *timer)
{
    struct kscan_gpio_topre_data *data =
        CONTAINER_OF(timer, struct kscan_gpio_topre_data, poll_timer);
    k_work_submit(&data->poll);
}

static void kscan_gpio_topre_work_handler(struct k_work *work)
{
    struct kscan_gpio_topre_data *data = CONTAINER_OF(work, struct kscan_gpio_topre_data, poll);
    const struct device *dev = data->dev;
    const struct kscan_gpio_topre_config *cfg = dev->config;
    bool matrix_read[MATRIX_CELLS];

    // Power on everything
    gpio_pin_configure(data->key, cfg->key.pin, GPIO_INPUT | cfg->key.flags);
    gpio_pin_set(data->strobe, cfg->strobe.pin, 1);
    gpio_pin_set(data->power, cfg->power.pin, 1);
    // Topre controller board needs 5 ms to be operational
    k_sleep(K_MSEC(5));
    for (int r = 0; r < MATRIX_ROWS; ++r)
    {
        for (int c = 0; c < MATRIX_COLS; ++c)
        {
            gpio_pin_set(data->bits[0], cfg->bits[0].pin, r & BIT(0));
            gpio_pin_set(data->bits[1], cfg->bits[1].pin, r & BIT(1));
            gpio_pin_set(data->bits[2], cfg->bits[2].pin, r & BIT(2));
            gpio_pin_set(data->bits[3], cfg->bits[3].pin, c & BIT(0));
            gpio_pin_set(data->bits[4], cfg->bits[4].pin, c & BIT(1));
            gpio_pin_set(data->bits[5], cfg->bits[5].pin, c & BIT(2));

            int cell = (r * MATRIX_COLS) + c;
            const bool prev = data->matrix_state[cell];
            gpio_pin_set(data->hys, cfg->hys.pin, prev);

            k_busy_wait(cfg->matrix_relax_us);

            const unsigned int lock = irq_lock();
            // Pull low strobe line to trigger sensing
            gpio_pin_set(data->strobe, cfg->strobe.pin, 0);
            gpio_pin_set(data->strobe, cfg->strobe.pin, 1);
            k_busy_wait(cfg->adc_read_settle_us);
            const bool pressed = gpio_pin_get(data->key, cfg->key.pin);
            irq_unlock(lock);
            gpio_pin_set(data->hys, cfg->hys.pin, 0);

            matrix_read[cell] = pressed;
        }
    }
    // Set all gpio pins to low and power off the controller board to avoid
    // current leakage.
    for (int i = 0; i < SEL_PINS; ++i)
    {
        gpio_pin_set(data->bits[i], cfg->bits[i].pin, 0);
    }
    gpio_pin_configure(data->key, cfg->key.pin, GPIO_DISCONNECTED);
    gpio_pin_set(data->power, cfg->power.pin, 0);
    gpio_pin_set(data->strobe, cfg->strobe.pin, 0);

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

static int kscan_gpio_topre_activity_event_handler(const zmk_event_t *eh)
{
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL)
    {
        return -ENOTSUP;
    }
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct kscan_gpio_topre_data *data = dev->data;
    const struct kscan_gpio_topre_config *cfg = dev->config;
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

ZMK_LISTENER(kscan_gpio_topre, kscan_gpio_topre_activity_event_handler);
ZMK_SUBSCRIPTION(kscan_gpio_topre, zmk_activity_state_changed);

static int kscan_gpio_topre_init(const struct device *dev)
{
    LOG_DBG("KSCAN init");
    struct kscan_gpio_topre_data *data = dev->data;
    const struct kscan_gpio_topre_config *cfg = dev->config;
    for (int i = 0; i < SEL_PINS; ++i)
    {
        const struct kscan_gpio_item_config *pin_cfg = &cfg->bits[i];
        data->bits[i] = device_get_binding(pin_cfg->label);
        gpio_pin_configure(data->bits[i], pin_cfg->pin, GPIO_OUTPUT_INACTIVE | pin_cfg->flags);
    }
    // The power line needs to source more than 0.5 mA current. Set the GPIO
    // drive mode to high drive.
    data->power = device_get_binding(cfg->power.label);
    gpio_pin_configure(data->power, cfg->power.pin,
                       GPIO_OUTPUT_INACTIVE | GPIO_DS_ALT_HIGH | cfg->power.flags);
    data->key = device_get_binding(cfg->key.label);
    gpio_pin_configure(data->key, cfg->key.pin, GPIO_DISCONNECTED);
    data->hys = device_get_binding(cfg->hys.label);
    gpio_pin_configure(data->hys, cfg->hys.pin, GPIO_OUTPUT_INACTIVE | cfg->hys.flags);
    data->strobe = device_get_binding(cfg->strobe.label);
    gpio_pin_configure(data->strobe, cfg->strobe.pin, GPIO_OUTPUT_INACTIVE | cfg->strobe.flags);
    data->dev = dev;

    k_timer_init(&data->poll_timer, kscan_gpio_topre_timer_handler, NULL);
    k_work_init(&data->poll, kscan_gpio_topre_work_handler);

    return 0;
}

static const struct kscan_driver_api kscan_gpio_topre_api = {
    .config = kscan_gpio_topre_configure,
    .enable_callback = kscan_gpio_topre_enable,
    .disable_callback = kscan_gpio_topre_disable,
};

static struct kscan_gpio_topre_data kscan_gpio_topre_data;

static const struct kscan_gpio_topre_config kscan_gpio_topre_config = {
    .bits =
        {
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 3),
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 4),
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 5),
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 6),
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 7),
            KSCAN_GPIO_TOPRE_ITEM_CFG(0, 8),
        },
    .power = KSCAN_GPIO_TOPRE_ITEM_CFG(0, 0),
    .key = KSCAN_GPIO_TOPRE_ITEM_CFG(0, 1),
    .hys = KSCAN_GPIO_TOPRE_ITEM_CFG(0, 2),
    .strobe = KSCAN_GPIO_TOPRE_ITEM_CFG(0, 9),
    .matrix_relax_us = DT_INST_PROP(0, matrix_relax_us),
    .adc_read_settle_us = DT_INST_PROP(0, adc_read_settle_us),
    .active_polling_interval_ms = DT_INST_PROP(0, active_polling_interval_ms),
    .idle_polling_interval_ms = DT_INST_PROP(0, idle_polling_interval_ms),
    .sleep_polling_interval_ms = DT_INST_PROP(0, sleep_polling_interval_ms),
};

DEVICE_DT_INST_DEFINE(0, kscan_gpio_topre_init, NULL, &kscan_gpio_topre_data,
                      &kscan_gpio_topre_config, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY,
                      &kscan_gpio_topre_api);