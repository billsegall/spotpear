// rest_api — the board joins WiFi and exposes a REST API:
//   GET  /health
//   POST /display/image   body = JPEG (Content-Type: image/jpeg) or raw RGB565
//                                (240x240x2 = 115200 bytes)
//   POST /display/text    body = JSON {"text":..., "fg":[r,g,b], "bg":[r,g,b]}
//   POST /play            body = 16-bit PCM WAV
//   GET  /record?seconds=n  -> records the mic, returns a WAV
//
// If API_TOKEN (from .env) is non-empty, every request must carry X-Api-Key.
// All command handlers serialize on a mutex — display and codec are shared.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#include "audio.h"
#include "display.h"
#include "net.h"
#include "wifi_credentials.h"

static const char *TAG = "rest_api";

#define ASSETS_BASE "/assets"
#define UPLOAD_WAV ASSETS_BASE "/upload.wav"
#define RECORD_WAV ASSETS_BASE "/record.wav"
// A 240x240 JPEG is typically 15-30 KB; the whole compressed image is buffered
// in RAM to decode, and free heap after WiFi + framebuffer is only ~50 KB, so cap
// it. Oversize uploads get a graceful error, not a crash.
#define MAX_JPEG_BYTES (48 * 1024)
#define MAX_RECORD_SECONDS 30

static SemaphoreHandle_t s_lock;

// --- helpers ----------------------------------------------------------------

static bool auth_ok(httpd_req_t *req)
{
    if (API_TOKEN[0] == '\0') {
        return true;  // open
    }
    char got[96] = "";
    httpd_req_get_hdr_value_str(req, "X-Api-Key", got, sizeof(got));
    return strcmp(got, API_TOKEN) == 0;
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, "unauthorized\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t reply(httpd_req_t *req, esp_err_t err, const char *ok_msg)
{
    if (err == ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, ok_msg, HTTPD_RESP_USE_STRLEN);
    }
    const char *status = (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_ARG ||
                          err == ESP_ERR_NOT_SUPPORTED) ? "400 Bad Request" : "500 Internal Server Error";
    httpd_resp_set_status(req, status);
    char msg[64];
    snprintf(msg, sizeof(msg), "error: %s\n", esp_err_to_name(err));
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t recv_to_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    char buf[1460];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (r <= 0) { fclose(f); return ESP_FAIL; }
        fwrite(buf, 1, r, f);
        remaining -= r;
    }
    fclose(f);
    return ESP_OK;
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    httpd_resp_set_type(req, ctype);
    char buf[1460];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// --- handlers ---------------------------------------------------------------

static esp_err_t health_get(httpd_req_t *req)
{
    char ip[16] = "?";
    net_get_ip(ip, sizeof(ip));
    char body[128];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"ip\":\"%s\",\"free_heap\":%lu}\n",
             ip, (unsigned long)esp_get_free_heap_size());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t image_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    char ct[40] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    if (strstr(ct, "image/jpeg")) {
        size_t len = req->content_len;
        if (len == 0 || len > MAX_JPEG_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            uint8_t *buf = malloc(len);
            if (!buf) {
                err = ESP_ERR_NO_MEM;
            } else {
                size_t off = 0;
                err = ESP_OK;
                while (off < len) {
                    int r = httpd_req_recv(req, (char *)buf + off, len - off);
                    if (r <= 0) { err = ESP_FAIL; break; }
                    off += r;
                }
                if (err == ESP_OK) err = display_show_jpeg(buf, len);
                free(buf);
            }
        }
    } else if (req->content_len == DISPLAY_FRAME_BYTES) {
        // Stream raw RGB565 straight into the framebuffer halves — no big buffer.
        display_raw_begin();
        char buf[1460];
        int remaining = req->content_len;
        err = ESP_OK;
        while (remaining > 0) {
            int r = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
            if (r <= 0) { err = ESP_FAIL; break; }
            display_raw_write((const uint8_t *)buf, r);
            remaining -= r;
        }
        if (err == ESP_OK) err = display_raw_end();
    } else {
        err = ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreGive(s_lock);
    return reply(req, err, "{\"status\":\"displayed\"}\n");
}

static esp_err_t text_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    int len = req->content_len;
    if (len <= 0 || len > 2048) return reply(req, ESP_ERR_INVALID_SIZE, NULL);

    char *body = malloc(len + 1);
    if (!body) return reply(req, ESP_ERR_NO_MEM, NULL);
    int off = 0;
    while (off < len) {
        int r = httpd_req_recv(req, body + off, len - off);
        if (r <= 0) { free(body); return reply(req, ESP_FAIL, NULL); }
        off += r;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return reply(req, ESP_ERR_INVALID_ARG, NULL);

    const cJSON *text = cJSON_GetObjectItem(root, "text");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(text)) {
        uint16_t fg = display_rgb565(255, 255, 255), bg = display_rgb565(0, 0, 0);
        const cJSON *jfg = cJSON_GetObjectItem(root, "fg");
        const cJSON *jbg = cJSON_GetObjectItem(root, "bg");
        if (cJSON_IsArray(jfg) && cJSON_GetArraySize(jfg) == 3)
            fg = display_rgb565(cJSON_GetArrayItem(jfg, 0)->valueint,
                                cJSON_GetArrayItem(jfg, 1)->valueint,
                                cJSON_GetArrayItem(jfg, 2)->valueint);
        if (cJSON_IsArray(jbg) && cJSON_GetArraySize(jbg) == 3)
            bg = display_rgb565(cJSON_GetArrayItem(jbg, 0)->valueint,
                                cJSON_GetArrayItem(jbg, 1)->valueint,
                                cJSON_GetArrayItem(jbg, 2)->valueint);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = display_show_text(text->valuestring, fg, bg);
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
    return reply(req, err, "{\"status\":\"displayed\"}\n");
}

static esp_err_t play_post(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = recv_to_file(req, UPLOAD_WAV);
    if (err == ESP_OK) err = audio_play_wav_file(UPLOAD_WAV);
    xSemaphoreGive(s_lock);
    return reply(req, err, "{\"status\":\"played\"}\n");
}

static esp_err_t record_get(httpd_req_t *req)
{
    if (!auth_ok(req)) return deny(req);
    int seconds = 3;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(q, "seconds", val, sizeof(val)) == ESP_OK) seconds = atoi(val);
    }
    if (seconds < 1) seconds = 1;
    if (seconds > MAX_RECORD_SECONDS) seconds = MAX_RECORD_SECONDS;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = audio_record_wav_file(RECORD_WAV, seconds);
    if (err == ESP_OK) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"record.wav\"");
        err = send_file(req, RECORD_WAV, "audio/wav");
    }
    xSemaphoreGive(s_lock);
    return err == ESP_OK ? ESP_OK : reply(req, err, NULL);
}

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t uris[] = {
        { .uri = "/health",        .method = HTTP_GET,  .handler = health_get },
        { .uri = "/display/image", .method = HTTP_POST, .handler = image_post },
        { .uri = "/display/text",  .method = HTTP_POST, .handler = text_post },
        { .uri = "/play",          .method = HTTP_POST, .handler = play_post },
        { .uri = "/record",        .method = HTTP_GET,  .handler = record_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

static void mount_assets(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = ASSETS_BASE,
        .partition_label = "assets",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

void app_main(void)
{
    // Display first: its 115 KB framebuffer needs a contiguous DMA-capable block,
    // and the C3's DMA region is only ~128 KB. Allocating before NVS/SPIFFS/WiFi
    // take their cut is what makes it fit.
    ESP_ERROR_CHECK(display_init());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_lock = xSemaphoreCreateMutex();
    mount_assets();
    ESP_ERROR_CHECK(audio_init());

    display_show_text("Connecting to\n" WIFI_SSID " ...",
                      display_rgb565(255, 255, 255), display_rgb565(0, 0, 40));

    if (net_wifi_connect() != ESP_OK) {
        display_show_text("WiFi FAILED\ncheck .env",
                          display_rgb565(255, 80, 80), display_rgb565(0, 0, 0));
        ESP_LOGE(TAG, "WiFi did not connect — check apps/rest_api/.env password. Halting.");
        return;
    }

    char ip[16] = "?";
    net_get_ip(ip, sizeof(ip));
    char banner[64];
    snprintf(banner, sizeof(banner), "Ready\n\nhttp://%s", ip);
    display_show_text(banner, display_rgb565(120, 255, 120), display_rgb565(0, 0, 0));

    start_server();
    ESP_LOGI(TAG, "ready: http://%s/  (free heap %lu)", ip,
             (unsigned long)esp_get_free_heap_size());
}
