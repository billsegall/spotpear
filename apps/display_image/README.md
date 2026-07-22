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

The embedded blob lives in memory-mapped flash, which SPI DMA cannot read from,
so it is copied into a `MALLOC_CAP_DMA` buffer first — the **whole frame at
once**, 115200 bytes, drawn in a single call and then never reused or freed.

That is deliberate, and the frugal-looking alternative is a trap.
`esp_lcd_panel_draw_bitmap()` does not block: it queues the SPI transaction and
returns, so the buffer must stay untouched until the DMA drains. An earlier
version staged 24 rows at a time through a small buffer and overwrote it
microseconds into the previous chunk's ~2.3 ms transfer, which showed on the
panel as the bottom of the image drawn twice. If you make the draw path
incremental again — for animation, say — synchronise on `on_color_trans_done`
or give each in-flight transfer its own buffer.

Note that neither the serial log nor decoding `image.rgb565` back to PNG can
detect that class of bug; both look perfectly healthy while the panel is wrong.
Only looking at the glass catches it.

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
