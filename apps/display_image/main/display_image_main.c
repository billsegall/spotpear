// Displays a single full-screen image on the board's GC9A01 240x240 round LCD.
//
// The image is a raw RGB565 blob embedded from main/image.rgb565 — no decoding
// at runtime. Regenerate it with tools/png_to_rgb565.py; see README.md.
//
// The init sequence below is taken from the vendor's stock firmware
// (docs/vendor/toy_ai_core_c3_1.28.cc). The colour inversion and the 0x62/0x63
// register writes are not guessable from the datasheet — keep them.

#include <string.h>

#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "display_image";

// Panel wiring and geometry — from docs/vendor/config.h, see docs/board.md.
#define LCD_H_RES 240
#define LCD_V_RES 240
#define PIN_SCLK 0
#define PIN_MOSI 21
#define PIN_CS 20
#define PIN_DC 1
#define PIN_RST (-1)  // not wired; the panel is reset by command
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

// --- Panel orientation and colour knobs -------------------------------------
// Baseline values are the vendor's. If the image comes out wrong, these are the
// three things to try, and the diagnostic pattern in image.rgb565 tells you
// which one:
//   red and blue swapped        -> LCD_COLOR_ELEMENT_ORDER
//   colours scrambled entirely  -> byte order in tools/png_to_rgb565.py
//   image mirrored or flipped   -> LCD_MIRROR_X / LCD_MIRROR_Y
#define LCD_COLOR_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_INVERT_COLOR true
#define LCD_MIRROR_X true
#define LCD_MIRROR_Y false

// The embedded image lives in memory-mapped flash, which SPI DMA cannot read
// from. Each chunk is staged through an internal-RAM bounce buffer.
#define CHUNK_ROWS 24
#define CHUNK_BYTES (LCD_H_RES * CHUNK_ROWS * 2)

extern const uint8_t image_start[] asm("_binary_image_rgb565_start");
extern const uint8_t image_end[] asm("_binary_image_rgb565_end");

static esp_err_t init_panel(esp_lcd_panel_io_handle_t *out_io,
                            esp_lcd_panel_handle_t *out_panel)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // Sized for one chunk, not one frame — nothing larger is ever sent.
        .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, NULL, NULL);
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io),
                        TAG, "esp_lcd_new_panel_io_spi failed");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_COLOR_ELEMENT_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(io, &panel_config, &panel),
                        TAG, "esp_lcd_new_panel_gc9a01 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, LCD_INVERT_COLOR), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, LCD_MIRROR_X, LCD_MIRROR_Y), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "display on failed");

    // Vendor-specific gamma/timing registers, copied from the stock firmware.
    // Undocumented in the GC9A01 datasheet; the panel is visibly wrong without them.
    const uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                                  0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x62, data_0x62, sizeof(data_0x62)),
                        TAG, "0x62 write failed");
    const uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                                  0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x63, data_0x63, sizeof(data_0x63)),
                        TAG, "0x63 write failed");

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}

static esp_err_t draw_image(esp_lcd_panel_handle_t panel)
{
    const size_t image_bytes = (size_t)(image_end - image_start);
    const size_t expected = (size_t)LCD_H_RES * LCD_V_RES * 2;
    if (image_bytes != expected) {
        ESP_LOGE(TAG, "image.rgb565 is %u bytes, expected %u (%dx%d RGB565)",
                 (unsigned)image_bytes, (unsigned)expected, LCD_H_RES, LCD_V_RES);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *chunk = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(chunk != NULL, ESP_ERR_NO_MEM, TAG, "no DMA memory for chunk buffer");

    esp_err_t err = ESP_OK;
    for (int y = 0; y < LCD_V_RES; y += CHUNK_ROWS) {
        const int rows = (y + CHUNK_ROWS > LCD_V_RES) ? (LCD_V_RES - y) : CHUNK_ROWS;
        const size_t bytes = (size_t)rows * LCD_H_RES * 2;
        memcpy(chunk, image_start + (size_t)y * LCD_H_RES * 2, bytes);
        err = esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H_RES, y + rows, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "draw_bitmap failed at row %d: %s", y, esp_err_to_name(err));
            break;
        }
    }

    heap_caps_free(chunk);
    return err;
}

void app_main(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    ESP_LOGI(TAG, "initialising GC9A01 %dx%d", LCD_H_RES, LCD_V_RES);
    ESP_ERROR_CHECK(init_panel(&io, &panel));
    ESP_LOGI(TAG, "panel ready");

    ESP_ERROR_CHECK(draw_image(panel));
    ESP_LOGI(TAG, "image drawn (%u bytes)", (unsigned)(image_end - image_start));

    // Hold the image. Unlike hello_world this must not restart, or the panel
    // would visibly strobe every few seconds.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
