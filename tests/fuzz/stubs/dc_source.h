#pragma once
// The parts of dragon-core v0.36.0 dc_source.h that the handlers use.
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    DC_SRC_KLIPPER = 0,
    DC_SRC_BAMBU = 1,
    DC_SRC_HA = 2,
    DC_SRC_NONE = 3,
    DC_SRC_KLIPPER_MQTT = 4,
    DC_SRC_PRUSA = 5,
    DC_SRC_MAX,
} dc_ctl_source_t;

dc_ctl_source_t dc_source_get(void);
esp_err_t dc_source_set(dc_ctl_source_t src);
const char *dc_source_str(dc_ctl_source_t src);
bool dc_source_has_filament(dc_ctl_source_t src);
