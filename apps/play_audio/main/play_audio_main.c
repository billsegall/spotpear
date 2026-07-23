// Plays a raw PCM audio file through the board's ES8311 codec, looping forever.
//
// The file main/audio_fs/audio.pcm is mono, 24 kHz, signed 16-bit little-endian
// (produced by tools/fetch_audio.sh) and is flashed into the "assets" SPIFFS
// partition. It is copyrighted and gitignored — not in the repo.
//
// Pin map and codec config come from the vendor stock firmware
// (docs/vendor/config.h, toy_ai_core_c3_1.28.cc). The ES8311 is an I2S slave; the
// C3 is I2S master. The class-D amp is gated by a PA pin that must be driven high
// — the es8311 driver does that for us via es8311_cfg.pa_pin on open.

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "play_audio";

// Codec / I2S / I2C wiring — docs/vendor/config.h, also docs/board.md.
#define I2C_SDA_PIN 3
#define I2C_SCL_PIN 4
#define I2S_MCLK_PIN 10
#define I2S_BCLK_PIN 8
#define I2S_WS_PIN 6
#define I2S_DOUT_PIN 5
#define PA_PIN 11  // class-D amp enable; muted unless high

#define SAMPLE_RATE 24000
#define OUT_VOLUME 70  // 0..100

// The audio file is mono, but the esp_codec_dev I2S data path only accepts an
// even channel count (audio_codec_data_i2s.c rejects odd channels), so we open
// the codec as 2-channel and duplicate each mono sample into L and R before
// writing. CHUNK_SAMPLES mono samples become 2*CHUNK_SAMPLES stereo samples.
#define CHUNK_SAMPLES 1024

#define ASSETS_BASE "/assets"
#define ASSETS_LABEL "assets"
#define AUDIO_PATH ASSETS_BASE "/audio.pcm"

static esp_err_t mount_assets(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = ASSETS_BASE,
        .partition_label = ASSETS_LABEL,
        .max_files = 2,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "SPIFFS mount failed");

    size_t total = 0, used = 0;
    esp_spiffs_info(ASSETS_LABEL, &total, &used);
    ESP_LOGI(TAG, "assets mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

static esp_err_t init_codec(esp_codec_dev_handle_t *out_dev)
{
    // I2C master for codec control (ES8311 @ 0x18).
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c bus failed");

    // I2S TX, master, standard Philips format. The clock is set here to a
    // placeholder; esp_codec_dev_open() reconfigures it to the real sample rate.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t tx_handle = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, NULL), TAG, "i2s channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "i2s std init failed");

    // Wire the esp_codec_dev interfaces to those handles.
    audio_codec_i2s_cfg_t i2s_if_cfg = { .port = I2S_NUM_0, .tx_handle = tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data_if failed");

    audio_codec_i2c_cfg_t i2c_if_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl_if failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "gpio_if failed");

    // DAC (playback) only. pa_pin lets the driver enable the amp on open.
    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = PA_PIN,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311 codec failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(dev, ESP_FAIL, TAG, "codec dev failed");

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(dev, OUT_VOLUME), TAG, "set vol failed");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,  // even channel count required by the i2s data path
        .sample_rate = SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(dev, &fs), TAG, "codec open failed");

    *out_dev = dev;
    return ESP_OK;
}

// Read mono 16-bit samples, duplicate each to stereo, and write to the codec.
// Returns ESP_OK on a full pass through the file.
static esp_err_t play_once(esp_codec_dev_handle_t dev, FILE *f)
{
    static int16_t mono[CHUNK_SAMPLES];
    static int16_t stereo[CHUNK_SAMPLES * 2];

    rewind(f);
    size_t n;
    while ((n = fread(mono, sizeof(int16_t), CHUNK_SAMPLES, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            stereo[2 * i] = mono[i];
            stereo[2 * i + 1] = mono[i];
        }
        int ret = esp_codec_dev_write(dev, stereo, (int)(n * 2 * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec write failed: %d", ret);
            return ESP_FAIL;
        }
    }
    return ferror(f) ? ESP_FAIL : ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(mount_assets());

    esp_codec_dev_handle_t dev = NULL;
    ESP_ERROR_CHECK(init_codec(&dev));
    ESP_LOGI(TAG, "codec ready, playing %s in a loop", AUDIO_PATH);

    FILE *f = fopen(AUDIO_PATH, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open %s", AUDIO_PATH);
        abort();
    }

    // Loop forever. Unlike hello_world this never restarts — a reboot would gap
    // the audio.
    uint32_t pass = 0;
    while (1) {
        esp_err_t err = play_once(dev, f);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "playback error on pass %u", (unsigned)pass);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGI(TAG, "finished pass %u, repeating", (unsigned)pass++);
        }
    }
}
