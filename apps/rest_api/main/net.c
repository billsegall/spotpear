// WiFi station bring-up. Connects to the network named in wifi_credentials.h
// (generated from .env) and tracks the DHCP-assigned IP.

#include "net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "wifi_credentials.h"

static const char *TAG = "net";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_RETRY 10

static EventGroupHandle_t s_events;
static int s_retries;
static char s_ip[16] = "";

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        if (s_retries < MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retries, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        ESP_LOGI(TAG, "got IP: %s", s_ip);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t net_wifi_connect(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    ESP_LOGI(TAG, "free heap before wifi start: %lu", (unsigned long)esp_get_free_heap_size());
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "free heap after wifi start:  %lu", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "connecting to SSID \"%s\"...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected; free heap: %lu", (unsigned long)esp_get_free_heap_size());
        return ESP_OK;
    }
    ESP_LOGE(TAG, "failed to connect to \"%s\" after %d retries", WIFI_SSID, MAX_RETRY);
    return ESP_FAIL;
}

bool net_get_ip(char *buf, size_t len)
{
    if (s_ip[0] == '\0') {
        return false;
    }
    strlcpy(buf, s_ip, len);
    return true;
}
