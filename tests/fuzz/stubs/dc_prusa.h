#pragma once
// The parts of dragon-core v0.36.0 dc_prusa.h that the handlers use.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char host[64];
    uint16_t port;
    char api_key[65];
} dc_prusa_config_t;

#define DC_PRUSA_DEFAULT_PORT 80

esp_err_t dc_prusa_set_config(const dc_prusa_config_t *cfg);
esp_err_t dc_prusa_get_config(dc_prusa_config_t *out);

typedef struct {
    int state;
    bool online;
    float bed_temp;
    float bed_target;
    char printer_state[12];
    char material[24];
    uint32_t status_age_ms;
} dc_prusa_status_t;
esp_err_t dc_prusa_get_status(dc_prusa_status_t *out);
