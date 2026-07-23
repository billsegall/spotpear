#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Bring up WiFi in station mode and connect to the SSID from wifi_credentials.h.
// Blocks until the first association + DHCP lease (or returns an error on repeated
// failure). Retries automatically on later disconnects.
esp_err_t net_wifi_connect(void);

// Copy the current DHCP IPv4 address ("a.b.c.d") into buf. Returns false if not
// connected yet.
bool net_get_ip(char *buf, size_t len);
