#pragma once
// The parts of dragon-core v0.36.0 dc_moonraker.h that the handlers use.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char host[64];
    uint16_t port;
    char api_key[65];
} dc_moonraker_config_t;

esp_err_t dc_moonraker_set_config(const dc_moonraker_config_t *cfg);
esp_err_t dc_moonraker_get_config(dc_moonraker_config_t *out);

typedef struct {
    int state;
    int printer;
    bool printing;
    float bed_temp;
    float bed_target;
    float extruder_temp;
    float chamber_temp;
    float progress;
    char filename[64];
    char material[16];
    bool db_present;
    bool db_connected;
    bool db_fault;
    bool db_inhibited;
    float db_target;
    char db_mode[12];
} dc_moonraker_status_t;
esp_err_t dc_moonraker_get_status(dc_moonraker_status_t *out);
