#pragma once
// The parts of dragon-core v0.36.0 dc_bambu.h that the handlers use.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char host[64];
    char serial[32];
    char code[32];
} dc_bambu_config_t;

esp_err_t dc_bambu_set_config(const dc_bambu_config_t *cfg);
esp_err_t dc_bambu_get_config(dc_bambu_config_t *out);

#define DC_BAMBU_DISCOVER_MAX 8
typedef struct {
    char host[64];
    char serial[32];
    char model[24];
    char name[32];
    uint32_t age_s;
} dc_bambu_found_t;
esp_err_t dc_bambu_scan_start(void);
bool dc_bambu_scanning(void);
int dc_bambu_discover_get(dc_bambu_found_t *out, int max);

#define DC_BAMBU_ZONE_COUNT 6
#define DC_BAMBU_CUSTOM_MAX 8
#define DC_BAMBU_ZONE_MAX   (DC_BAMBU_ZONE_COUNT + DC_BAMBU_CUSTOM_MAX)
typedef struct {
    char name[12];
    uint8_t target_c;
    uint8_t default_c;
    bool custom;
} dc_bambu_zone_t;
int dc_bambu_zone_get_all(dc_bambu_zone_t *out, int max);
esp_err_t dc_bambu_zone_set(const char *name, uint8_t target_c);
esp_err_t dc_bambu_zone_remove(const char *name);

typedef struct {
    int state;
    bool connected;
    float bed_temp;
    float bed_target;
    float chamber_temp;
    uint32_t chamber_temp_age_ms;
    char filament[16];
    bool printing;
    bool error;
    float progress;
    int print_state;
} dc_bambu_status_t;
esp_err_t dc_bambu_get_status(dc_bambu_status_t *out);
