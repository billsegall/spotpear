# spotpear

Apps and experiments for the Spotpear Toy AI Core C3 Mini (ESP32-C3).

## Toolchain

ESP-IDF (C).

```sh
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Layout

```
apps/       one directory per application (each an ESP-IDF project)
components/ shared components across apps
docs/       board notes, pinouts, datasheets
```
