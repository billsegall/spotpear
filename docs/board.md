# Spotpear Toy AI Core C3 Mini — board notes

Status: **silicon confirmed on hardware, full pin map recovered.** The "Verified"
section was read off the board itself; the pin map comes from the vendor's own
firmware source for this exact board variant. Anything still open is under
[Still unknown](#still-unknown).

Vendor page:
<https://spotpear.com/wiki/ESP32-mini-C3-AI-Toy-AI-Core-DeepSeek-XiaoZhi-DouBao-1.54-1.28-0.71-inch-LCD.html>

## Verified on hardware

Read with `esptool.py --port /dev/ttyACM0 flash_id`, 2026-07-22.

| Item | Value |
| ---- | ----- |
| Chip | ESP32-C3, QFN32 package, silicon revision **v0.4** |
| Features | WiFi, BLE |
| Crystal | 40 MHz |
| USB | native USB-Serial/JTAG (no CH340/CP210x bridge) |
| Flash | **16 MB**, manufacturer `0x20`, device `0x4018` — matches the vendor claim |

Vendor 16 MB figure confirmed. Note the ESP-IDF default `sdkconfig` still builds
against 2 MB — set flash size in menuconfig, see [toolchain.md](toolchain.md).

### Stock partition table

Parsed out of the backup at offset `0x8000`:

```
# Name,   Type, SubType, Offset,    Size
nvs,      data, nvs,     0x9000,    16K
otadata,  data, ota,     0xd000,    8K
phy_init, data, phy,     0xf000,    4K
ota_0,    app,  ota_0,   0x20000,   4032K
ota_1,    app,  ota_1,   0x410000,  4032K
assets,   data, spiffs,  0x800000,  4000K
```

Dual-OTA layout with an 8 MB tail: apps get ~4 MB each, a 4 MB SPIFFS `assets`
partition at `0x800000` holds the Xiaozhi resources (voice prompts, images), and
roughly 8 MB above that is unallocated.

Reproduce with:

```sh
dd if=~/esp/spotpear-c3-stock-firmware.bin of=ptable.bin bs=1 skip=32768 count=3072
python3 $IDF_PATH/components/partition_table/gen_esp32part.py ptable.bin
```

### Connecting

Enumerates as USB ID `303a:1001` "Espressif USB JTAG/serial debug unit" →
`/dev/ttyACM0`.

**The board does not enumerate on plug-in alone — the PWR button must be
pressed.** If `lsusb` shows nothing, that is the first thing to check, before
suspecting the cable.

`/dev/ttyACM0` is `root:dialout`, so serial access needs `dialout` membership
(`sudo usermod -aG dialout $USER`, effective next login). For access without
logging out: `sudo setfacl -m u:$USER:rw /dev/ttyACM0` — resets on replug.

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

## Pin map

**This unit is the `toy-ai-core-c3-1.28` variant** — the stock firmware names its
own source file `./main/boards/toy-ai-core-c3-1.28/toy_ai_core_c3_1.28.cc`.

Source: vendor's own `xiaozhi-esp32-2.0.3` release,
`main/boards/toy-ai-core-c3-1.28/config.h`. This board is **not** in upstream
[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) or any public fork — the
directory exists only in Spotpear's zip (see [Vendor downloads](#vendor-downloads)).

### Audio — ES8311 codec

Confirmed live in the stock boot log: `Es8311AudioCodec`, ES8311 in **slave
mode**, duplex, 24 kHz in and out, 16-bit stereo I2S STD mode.

| Function | GPIO |
| -------- | ---- |
| I2S MCLK | 10 |
| I2S BCLK | 8 |
| I2S WS / LRCK | 6 |
| I2S DIN (mic → MCU) | 7 |
| I2S DOUT (MCU → speaker) | 5 |
| PA enable (amp shutdown) | 11 |
| I2C SDA (codec control) | 3 |
| I2C SCL (codec control) | 4 |

Codec I2C address: `ES8311_CODEC_DEFAULT_ADDR` (0x18). PA pin must be driven high
to get audio out — the amplifier is muted otherwise.

### Display — GC9A01, 240×240

| Function | GPIO |
| -------- | ---- |
| SPI SCLK | 0 |
| SPI MOSI | 21 |
| SPI CS | 20 |
| SPI DC | 1 |
| SPI RESET | not connected (`GPIO_NUM_NC`) |

Bus `SPI2_HOST` at 40 MHz, 16 bpp, **BGR** byte order. Orientation flags used by
stock: `MIRROR_X = true`, `MIRROR_Y = false`, `SWAP_XY = false`, offsets 0/0. No
backlight pin is defined. Reset is unwired, so the panel must be reset by command,
not by GPIO.

The 1.54" sibling variant uses a **GC9D01N** instead — the two are not
interchangeable.

### Other

| Function | GPIO |
| -------- | ---- |
| WS2812B RGB LED | 2 |
| BOOT button | 9 |

The LED is driven by xiaozhi's `SingleLed` class (addressable, one pixel),
consistent with the WS2812B in the vendor spec.

### Build config used by stock

From the board's `config.json`: target `esp32c3`, with `CONFIG_PM_ENABLE`,
`CONFIG_FREERTOS_USE_TICKLESS_IDLE`, `CONFIG_USE_ESP_WAKE_WORD`, and
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` appended.

## Still unknown

- **Battery + charge circuit** — presence and charge IC unconfirmed. The schematic
  PDF below would settle it; not yet read.
- **Wake-word module header** pinout — vendor ships a separate "示例代码-PA4"
  example for modifying the wake word.
- **PSRAM** — none reported; assume the C3 has none.

## Vendor downloads

All from the wiki page. Not mirrored into this repo (large binaries).

| What | URL |
| ---- | --- |
| Full source (contains this board's `config.h`) | `https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-C3-MINI/xiaozhi-esp32-2.0.3.zip` (373 MB) |
| **Schematic PDF** | `https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-C3-MINI/Toy-AI-Core-C3-MINI.pdf` |
| Stock firmware 1.28" EN | `https://cdn.static.spotpear.com/uploads/picture/learn/ESP32/ESP32-C3-MINI/Toy-AI-Core-C3-1.28-EN-1.bin` |
| Dimensions | `…/Toy-AI-Core-C3-MINI.step`, `…/Toy-AI-Core-C3-MINI.DWG` |

The source zip also carries prebuilt `releases/v2.0.3_toy-ai-core-c3-1.28/`
images in both CN and EN — a second route back to stock alongside the local dump.

## Recovery

Stock Xiaozhi firmware is overwritten by the first `idf.py flash`.

A full 16 MB dump was taken before any flashing, kept **outside this repo** (too
large for git):

```
~/esp/spotpear-c3-stock-firmware.bin
16777216 bytes (exactly 16 MB)
sha256 5dc4fde2e86e4d7312752608951dd2d5642be26bdb24e335cf8b04fab05b0701
```

Starts with `e9` — a valid ESP image header, so the dump is sound.

Retaken with:

```sh
esptool.py --port /dev/ttyACM0 read_flash 0 ALL stock-firmware-backup.bin
```

Restore with:

```sh
esptool.py --port /dev/ttyACM0 write_flash 0 ~/esp/spotpear-c3-stock-firmware.bin
```

The restore is untested — it has never been exercised on this board.

Hold BOOT while plugging in if the board does not enter download mode on its own.
