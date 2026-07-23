#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_W 240
#define DISPLAY_H 240
#define DISPLAY_FRAME_BYTES (DISPLAY_W * DISPLAY_H * 2)

// Init the GC9A01 panel and allocate the framebuffer. Call once.
esp_err_t display_init(void);

// Decode a 240x240 JPEG (block by block) and show it.
esp_err_t display_show_jpeg(const uint8_t *data, size_t len);

// Render text (word-wrapped, top-left) on a solid background and show it.
// Colors are RGB565 big-endian (use display_rgb565()).
esp_err_t display_show_text(const char *text, uint16_t fg, uint16_t bg);

// Stream a raw RGB565 (big-endian) full frame in arbitrary-sized chunks:
// begin, write DISPLAY_FRAME_BYTES total across any number of calls, then end
// (which validates the total length and pushes to the panel).
void display_raw_begin(void);
void display_raw_write(const uint8_t *data, size_t len);
esp_err_t display_raw_end(void);

// Pack r,g,b (0..255) into big-endian RGB565 as the panel expects.
uint16_t display_rgb565(uint8_t r, uint8_t g, uint8_t b);
