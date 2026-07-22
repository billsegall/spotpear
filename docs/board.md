# Spotpear Toy AI Core C3 Mini — board notes

Status: **partly documented, no hardware inspected yet.** Everything below is
either from the vendor wiki or explicitly marked unknown. Nothing here has been
confirmed against the physical board or a schematic — confirm before writing
driver code against it.

Vendor page:
<https://spotpear.com/wiki/ESP32-mini-C3-AI-Toy-AI-Core-DeepSeek-XiaoZhi-DouBao-1.54-1.28-0.71-inch-LCD.html>

## From the vendor wiki

| Item | Value |
| ---- | ----- |
| MCU | ESP32-C3 (single-core RISC-V, 160 MHz) |
| Flash | 16 MB |
| PSRAM | not stated |
| Display | 1.28" / 1.54" / 0.71" LCD variants, attached via 18P 0.5 mm FPC connector |
| LCD controller | not stated |
| Microphones | two, on-board |
| Speaker | driven via a speaker cable header; amplifier part not stated |
| RGB LED | WS2812B |
| Buttons | BOOT, PWR/switch |
| USB | Type-C |
| Antenna | external, u.FL-style seat |
| Extra | wake-word module header |
| Stock firmware | Xiaozhi, provisioned over Wi-Fi, paired at xiaozhi.me |

## Unknown — needs resolving before hardware work

- **GPIO pin map.** The wiki publishes no pin table. Required for: I2S mic in,
  I2S/PDM speaker out, LCD (SPI/QSPI + DC/CS/RST/BL), WS2812B data, button GPIOs.
- **LCD controller part** (likely GC9A01 on the 1.28" round variant — *unverified
  guess, do not code against it*).
- **Audio codec / amplifier parts** — mic could be analog, PDM, or I2S; this
  changes the driver completely.
- **Battery + charge circuit** — presence and charge IC unknown.
- **Actual flash size on the unit in hand** — read it back with
  `esptool.py flash_id` once connected, rather than trusting the listing.

## How to resolve

1. Connect the board and run `esptool.py --port /dev/ttyACM0 flash_id` — gives
   real flash size and chip revision.
2. The stock firmware is [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32),
   which keeps one directory per supported board under `main/boards/`, each with
   a `config.h` holding the exact pin assignments. Find the directory matching
   this board and the pin map comes from there. Not yet located for this
   specific C3 variant.
3. Failing that: ask Spotpear for the schematic, or buzz out the FPC connector.

## Recovery

Stock Xiaozhi firmware is overwritten by the first `idf.py flash`. Back it up
first if you want the option to return:

```sh
esptool.py --port /dev/ttyACM0 read_flash 0 ALL stock-firmware-backup.bin
```

Hold BOOT while plugging in if the board does not enter download mode on its own.
