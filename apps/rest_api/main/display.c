// GC9A01 240x240 round panel: init + framebuffer + image/text draw.
//
// The C3's largest contiguous free block is ~112 KB, just under the 115 KB a full
// 240x240 RGB565 frame needs, and the DMA region must stay free for WiFi and the
// I2S codec. So the framebuffer is split into two 120-row halves in ordinary
// internal RAM, and pushed to the panel one small band at a time through a
// DMA-capable bounce buffer, waiting for each band's transfer before reusing it
// (the async-reuse race that bit display_image). JPEG is decoded block by block
// straight into the same bounce, so it needs no full-frame output buffer.
//
// Panel init (BGR, invert, mirror, 0x62/0x63 register writes) is copied verbatim
// from the vendor stock firmware, as in display_image.

#include "display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "font.h"

static const char *TAG = "display";

#define PIN_SCLK 0
#define PIN_MOSI 21
#define PIN_CS 20
#define PIN_DC 1
#define PIN_RST (-1)
#define LCD_CLK_HZ (40 * 1000 * 1000)

#define TEXT_SCALE 2  // 8x16 glyphs drawn at 16x32

#define HALF_ROWS (DISPLAY_H / 2)                 // 120
#define HALF_PIXELS (DISPLAY_W * HALF_ROWS)       // 28800
#define HALF_BYTES (HALF_PIXELS * 2)              // 57600
#define BAND_ROWS 20                              // divides HALF_ROWS; >= any JPEG MCU block
#define BAND_BYTES (DISPLAY_W * BAND_ROWS * 2)    // 9600

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_half[2];  // logical framebuffer, two 120-row halves, internal RAM
static uint16_t *s_band;     // DMA-capable bounce (also JPEG block output)
static SemaphoreHandle_t s_band_done;
static size_t s_raw_off;     // raw-stream write cursor

uint16_t display_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));  // big-endian, panel clocks high byte first
}

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_band_done, &woken);
    return woken == pdTRUE;
}

// Push one band (already staged in s_band) at rows [y, y+rows) and wait for the
// DMA to finish, so the caller can safely refill s_band.
static void push_band(int y, int rows)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISPLAY_W, y + rows, s_band);
    xSemaphoreTake(s_band_done, portMAX_DELAY);
}

// Push the whole framebuffer (both halves) band by band.
static void display_push(void)
{
    for (int h = 0; h < 2; h++) {
        for (int by = 0; by < HALF_ROWS; by += BAND_ROWS) {
            memcpy(s_band, &s_half[h][by * DISPLAY_W], BAND_BYTES);
            push_band(h * HALF_ROWS + by, BAND_ROWS);
        }
    }
}

static inline void put_px(int x, int y, uint16_t c)
{
    if (x >= 0 && x < DISPLAY_W && y >= 0 && y < DISPLAY_H) {
        s_half[y / HALF_ROWS][(y % HALF_ROWS) * DISPLAY_W + x] = c;
    }
}

esp_err_t display_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, on_trans_done, NULL);
    io_cfg.pclk_hz = LCD_CLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel), TAG, "gc9a01");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    const uint8_t d62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(s_io, 0x62, d62, sizeof(d62));
    const uint8_t d63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    esp_lcd_panel_io_tx_param(s_io, 0x63, d63, sizeof(d63));

    s_band_done = xSemaphoreCreateBinary();
    s_band = heap_caps_aligned_alloc(16, BAND_BYTES, MALLOC_CAP_DMA);
    s_half[0] = heap_caps_malloc(HALF_BYTES, MALLOC_CAP_INTERNAL);
    s_half[1] = heap_caps_malloc(HALF_BYTES, MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_band_done && s_band && s_half[0] && s_half[1], ESP_ERR_NO_MEM,
                        TAG, "framebuffer alloc failed");
    memset(s_half[0], 0, HALF_BYTES);
    memset(s_half[1], 0, HALF_BYTES);
    display_push();
    ESP_LOGI(TAG, "panel ready (2x%d KB fb, %d B band)", HALF_BYTES / 1024, BAND_BYTES);
    return ESP_OK;
}

// --- raw RGB565 streaming ---------------------------------------------------

void display_raw_begin(void) { s_raw_off = 0; }

void display_raw_write(const uint8_t *data, size_t len)
{
    while (len > 0 && s_raw_off < DISPLAY_FRAME_BYTES) {
        int half = s_raw_off / HALF_BYTES;
        size_t within = s_raw_off % HALF_BYTES;
        size_t chunk = HALF_BYTES - within;
        if (chunk > len) chunk = len;
        memcpy((uint8_t *)s_half[half] + within, data, chunk);
        data += chunk;
        len -= chunk;
        s_raw_off += chunk;
    }
}

esp_err_t display_raw_end(void)
{
    if (s_raw_off != DISPLAY_FRAME_BYTES) {
        ESP_LOGE(TAG, "raw rgb565 got %u bytes, expected %u", (unsigned)s_raw_off, DISPLAY_FRAME_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    display_push();
    return ESP_OK;
}

// --- JPEG (block mode, straight to the panel) -------------------------------

esp_err_t display_show_jpeg(const uint8_t *data, size_t len)
{
    jpeg_dec_config_t cfg = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_BE,  // matches the panel byte order
        .block_enable = true,                        // emit one MCU band per process call
    };
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t io = { .inbuf = (uint8_t *)data, .inbuf_len = (int)len };
    jpeg_dec_header_info_t info = { 0 };
    esp_err_t ret = ESP_OK;
    int blk_len = 0, count = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) { ret = ESP_FAIL; goto done; }
    if (info.width != DISPLAY_W || info.height != DISPLAY_H) {
        ESP_LOGE(TAG, "jpeg is %dx%d, need %dx%d", info.width, info.height, DISPLAY_W, DISPLAY_H);
        ret = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    if (jpeg_dec_get_outbuf_len(dec, &blk_len) != JPEG_ERR_OK || blk_len > BAND_BYTES ||
        jpeg_dec_get_process_count(dec, &count) != JPEG_ERR_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    io.outbuf = (uint8_t *)s_band;  // DMA-capable, 16-byte aligned
    int y = 0;
    for (int i = 0; i < count && y < DISPLAY_H; i++) {
        if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) { ret = ESP_FAIL; goto done; }
        int rows = io.out_size / (DISPLAY_W * 2);
        if (y + rows > DISPLAY_H) rows = DISPLAY_H - y;
        push_band(y, rows);  // draws from s_band and waits before the next process refills it
        y += rows;
    }
done:
    jpeg_dec_close(dec);
    return ret;
}

// --- text -------------------------------------------------------------------

static void blit_glyph(char c, int ox, int oy, uint16_t fg)
{
    if (c < FONT_FIRST || c > FONT_LAST) return;
    const uint8_t *g = font8x16[c - FONT_FIRST];
    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            if (!(g[row] & (1 << (FONT_W - 1 - col)))) continue;
            for (int sy = 0; sy < TEXT_SCALE; sy++)
                for (int sx = 0; sx < TEXT_SCALE; sx++)
                    put_px(ox + col * TEXT_SCALE + sx, oy + row * TEXT_SCALE + sy, fg);
        }
    }
}

esp_err_t display_show_text(const char *text, uint16_t fg, uint16_t bg)
{
    for (int h = 0; h < 2; h++)
        for (int i = 0; i < HALF_PIXELS; i++) s_half[h][i] = bg;

    const int cw = FONT_W * TEXT_SCALE;
    const int ch = FONT_H * TEXT_SCALE;
    const int cols = DISPLAY_W / cw;
    int x = 0, y = 4;

    for (const char *p = text; *p; p++) {
        if (*p == '\n') { x = 0; y += ch; continue; }
        if (*p == ' ' && x > 0) {
            int wordlen = 0;
            for (const char *q = p + 1; *q && *q != ' ' && *q != '\n'; q++) wordlen++;
            if ((x / cw) + 1 + wordlen > cols) { x = 0; y += ch; continue; }
        }
        if (x + cw > DISPLAY_W) { x = 0; y += ch; }
        if (y + ch > DISPLAY_H) break;
        blit_glyph(*p, x, y, fg);
        x += cw;
    }
    display_push();
    return ESP_OK;
}
