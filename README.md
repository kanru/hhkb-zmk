# HHKB Pro 2 with nice!nano board and ZMK firmware

## Build locally

Follow the steps in the [official ZMK document](https://zmk.dev/docs/user-setup) to setup build environment.

Add the extra kscan module:

```sh
west build -b nice_nano_v2 -- \
    -DSHIELD=hhkb \
    -DZEPHYR_EXTRA_MODULES=$ZMK_PATH/app/drivers/;$WORKSPACE_PATH/config/hhkb_drivers/ \
    -DZMK_CONFIG=$WORKSPACE_PATH/config
```

## Soldering (WIP)

![parts](./images/parts.jpg)

![connector](./images/connector.jpg)

![board](./images/board.jpg)
