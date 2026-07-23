// ES8311 duplex audio: play a WAV to the speaker, record the mic to a WAV.
//
// Codec/I2S/I2C wiring is the vendor's (docs/vendor/config.h), same as play_audio.
// The C3 is I2S master, the ES8311 a slave; the amp's PA pin (GPIO 11) is driven
// by the es8311 driver via pa_pin. The esp_codec_dev I2S path requires an even
// channel count, so playback and capture both run 2-channel and we duplicate /
// downmix mono at the edges.

#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_log.h"

static const char *TAG = "audio";

#define I2C_SDA 3
#define I2C_SCL 4
#define I2S_MCLK 10
#define I2S_BCLK 8
#define I2S_WS 6
#define I2S_DOUT 5
#define I2S_DIN 7
#define PA_PIN 11

#define OUT_VOLUME 75
#define IN_GAIN_DB 30.0f
#define FRAMES 512  // stereo frames per chunk

static esp_codec_dev_handle_t s_play;
static esp_codec_dev_handle_t s_rec;

esp_err_t audio_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus), TAG, "i2c bus");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_chan_handle_t tx = NULL, rx = NULL;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RECORD_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK, .bclk = I2S_BCLK, .ws = I2S_WS, .dout = I2S_DOUT, .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx std");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx std");

    audio_codec_i2s_cfg_t i2s_if_cfg = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "data_if");

    audio_codec_i2c_cfg_t i2c_if_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "ctrl_if");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,  // duplex: DAC + ADC
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = PA_PIN,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311");

    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec_if, .data_if = data_if };
    s_play = esp_codec_dev_new(&play_cfg);
    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec_if, .data_if = data_if };
    s_rec = esp_codec_dev_new(&rec_cfg);
    ESP_RETURN_ON_FALSE(s_play && s_rec, ESP_FAIL, TAG, "codec dev");

    esp_codec_dev_set_out_vol(s_play, OUT_VOLUME);
    esp_codec_dev_set_in_gain(s_rec, IN_GAIN_DB);
    ESP_LOGI(TAG, "codec ready (duplex)");
    return ESP_OK;
}

// --- minimal WAV header handling -------------------------------------------

typedef struct {
    uint16_t audio_format, channels, bits;
    uint32_t sample_rate, data_bytes;
    long data_offset;
} wav_info_t;

static uint32_t rd_u32(FILE *f) { uint8_t b[4]; fread(b, 1, 4, f); return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t rd_u16(FILE *f) { uint8_t b[2]; fread(b, 1, 2, f); return b[0] | b[1] << 8; }

static esp_err_t wav_parse(FILE *f, wav_info_t *w)
{
    char id[4];
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) return ESP_ERR_INVALID_ARG;
    fseek(f, 4, SEEK_CUR);  // riff size
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) return ESP_ERR_INVALID_ARG;

    while (fread(id, 1, 4, f) == 4) {
        uint32_t sz = rd_u32(f);
        if (!memcmp(id, "fmt ", 4)) {
            w->audio_format = rd_u16(f);
            w->channels = rd_u16(f);
            w->sample_rate = rd_u32(f);
            fseek(f, 6, SEEK_CUR);  // byte rate + block align
            w->bits = rd_u16(f);
            fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(id, "data", 4)) {
            w->data_bytes = sz;
            w->data_offset = ftell(f);
            return (w->audio_format == 1 && w->bits == 16 && w->channels >= 1 && w->channels <= 2)
                       ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        } else {
            fseek(f, (sz + 1) & ~1u, SEEK_CUR);  // skip, word-aligned
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static void wav_write_header(FILE *f, uint32_t rate, uint16_t channels, uint32_t data_bytes)
{
    uint32_t byte_rate = rate * channels * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;
    h[4] = riff; h[5] = riff >> 8; h[6] = riff >> 16; h[7] = riff >> 24;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;      // fmt size
    h[20] = 1; h[21] = 0;                        // PCM
    h[22] = channels; h[23] = 0;
    h[24] = rate; h[25] = rate >> 8; h[26] = rate >> 16; h[27] = rate >> 24;
    h[28] = byte_rate; h[29] = byte_rate >> 8; h[30] = byte_rate >> 16; h[31] = byte_rate >> 24;
    h[32] = channels * 2; h[33] = 0;             // block align
    h[34] = 16; h[35] = 0;                       // bits
    memcpy(h + 36, "data", 4);
    h[40] = data_bytes; h[41] = data_bytes >> 8; h[42] = data_bytes >> 16; h[43] = data_bytes >> 24;
    fwrite(h, 1, 44, f);
}

// --- playback ---------------------------------------------------------------

esp_err_t audio_play_wav_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", path);

    wav_info_t w = { 0 };
    esp_err_t ret = wav_parse(f, &w);
    if (ret != ESP_OK) { fclose(f); ESP_LOGE(TAG, "unsupported WAV"); return ret; }
    ESP_LOGI(TAG, "play %lu Hz, %u ch, %lu bytes", (unsigned long)w.sample_rate, w.channels,
             (unsigned long)w.data_bytes);

    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = w.sample_rate };
    ret = esp_codec_dev_open(s_play, &fs);
    if (ret != ESP_OK) { fclose(f); return ret; }

    static int16_t in[FRAMES * 2];    // up to stereo source frames
    static int16_t out[FRAMES * 2];   // always stereo out
    fseek(f, w.data_offset, SEEK_SET);
    uint32_t remaining = w.data_bytes;
    while (remaining > 0) {
        size_t want = w.channels == 2 ? sizeof(in) : sizeof(in) / 2;
        if (want > remaining) want = remaining;
        size_t got = fread(in, 1, want, f);
        if (got == 0) break;
        remaining -= got;
        int frames = w.channels == 2 ? got / 4 : got / 2;
        for (int i = 0; i < frames; i++) {
            int16_t l = w.channels == 2 ? in[2 * i] : in[i];
            int16_t r = w.channels == 2 ? in[2 * i + 1] : in[i];
            out[2 * i] = l;
            out[2 * i + 1] = r;
        }
        if (esp_codec_dev_write(s_play, out, frames * 4) != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    esp_codec_dev_close(s_play);
    fclose(f);
    return ret;
}

// --- record -----------------------------------------------------------------

esp_err_t audio_record_wav_file(const char *path, int seconds)
{
    FILE *f = fopen(path, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", path);
    wav_write_header(f, AUDIO_RECORD_RATE, 1, 0);  // patched at the end

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 2, .sample_rate = AUDIO_RECORD_RATE };
    esp_err_t ret = esp_codec_dev_open(s_rec, &fs);
    if (ret != ESP_OK) { fclose(f); return ret; }

    static int16_t in[FRAMES * 2];   // stereo from codec
    static int16_t mono[FRAMES];
    uint32_t total_frames = (uint32_t)seconds * AUDIO_RECORD_RATE;
    uint32_t data_bytes = 0;
    while (total_frames > 0) {
        int frames = total_frames < FRAMES ? (int)total_frames : FRAMES;
        if (esp_codec_dev_read(s_rec, in, frames * 4) != ESP_CODEC_DEV_OK) { ret = ESP_FAIL; break; }
        for (int i = 0; i < frames; i++) {
            mono[i] = in[2 * i];  // left channel = the mic
        }
        fwrite(mono, sizeof(int16_t), frames, f);
        data_bytes += frames * sizeof(int16_t);
        total_frames -= frames;
    }
    esp_codec_dev_close(s_rec);

    fseek(f, 0, SEEK_SET);
    wav_write_header(f, AUDIO_RECORD_RATE, 1, data_bytes);
    fclose(f);
    ESP_LOGI(TAG, "recorded %lu bytes -> %s", (unsigned long)data_bytes, path);
    return ret;
}
