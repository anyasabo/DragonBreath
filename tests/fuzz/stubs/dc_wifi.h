#pragma once
// The parts of dragon-core v0.36.0 dc_wifi.h that the handlers use.
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t dc_wifi_get_hostname(char *out, size_t out_size);
esp_err_t dc_wifi_set_hostname(const char *hostname);
// A single DNS label (RFC 1123): 1-32 chars of [A-Za-z0-9-], no leading or
// trailing hyphen.
bool dc_wifi_hostname_valid(const char *hostname);
