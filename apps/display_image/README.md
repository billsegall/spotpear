# display_image

Draws one full-screen image on the board's GC9A01 240×240 round LCD, then holds
it. No LVGL, no framebuffer — a raw RGB565 blob is embedded at build time and
pushed to the panel in row chunks.

## Drop your own image in

```sh
./tools/png_to_rgb565.py your-photo.jpg main/image.rgb565
. ~/esp/esp-idf/export.sh
idf.py -p /dev/ttyACM0 flash
```

`main/image.rgb565` is a headerless blob: 240 × 240 × 2 = **115200 bytes**,
exactly. The app refuses to draw anything else and logs the size it got. The
converter scales to cover 240×240 and centre-crops — the panel is round, so the
corners are outside the visible circle regardless.

## What is committed by default

A diagnostic pattern, not a picture: red / green / blue bands top to bottom, plus
a white square high and left. It exists so a wrong setting is identifiable from
one look at the panel rather than from the serial log, which cannot show colour.

| What you see | What is wrong | Fix in `main/display_image_main.c` |
| --- | --- | --- |
| Bands run blue→green→red top to bottom | Flipped vertically | toggle `LCD_MIRROR_Y` |
| White square on the right | Flipped horizontally | toggle `LCD_MIRROR_X` |
| Top band blue, bottom band red | R and B swapped | `LCD_COLOR_ELEMENT_ORDER` → `LCD_RGB_ELEMENT_ORDER_RGB` |
| Colours look like a negative | Inversion wrong | toggle `LCD_INVERT_COLOR` |
| Muddy noise, no clean bands | Byte order wrong | check `to_rgb565_be()` in `tools/png_to_rgb565.py` |

Regenerate it with `./tools/make_test_pattern.py main/image.rgb565`.

## Notes

Byte order is the easy bug. The GC9A01 clocks the **high byte first**, so pixels
are packed big-endian; writing native little-endian `uint16_t` swaps every pixel.
`make_test_pattern.py` imports the packer from `png_to_rgb565.py` so the two
cannot drift apart.

The embedded blob lives in memory-mapped flash, which SPI DMA cannot read from.
Each 24-row chunk is copied into a `MALLOC_CAP_DMA` bounce buffer first, and the
SPI bus `max_transfer_sz` is sized for one chunk (11520 bytes) rather than a full
frame (115200).

Panel init is copied from the vendor's stock firmware
([`docs/vendor/toy_ai_core_c3_1.28.cc`](../../docs/vendor/toy_ai_core_c3_1.28.cc)),
including `invert_color(true)` and the undocumented `0x62`/`0x63` register
writes. Those are not in the GC9A01 datasheet — don't drop them. Reset is not
wired on this board (`GPIO_NUM_NC`), so the panel resets by command. There is no
backlight pin, so brightness is not controllable.

The GC9A01 driver comes from the component registry
(`espressif/esp_lcd_gc9a01`), declared in `main/idf_component.yml`. It is not
part of ESP-IDF. `managed_components/` and `dependencies.lock` stay gitignored;
the first build downloads them.

Pinout (`docs/vendor/config.h`, also in [`docs/board.md`](../../docs/board.md)):
SCLK 0, MOSI 21, CS 20, DC 1, RESET n/c, `SPI2_HOST` @ 40 MHz.

Unlike [`hello_world`](../hello_world), this app never restarts — it would make
the panel strobe. `sdkconfig.defaults` sets the board's real 16 MB flash size.
