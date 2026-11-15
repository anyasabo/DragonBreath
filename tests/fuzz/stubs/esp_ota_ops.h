#pragma once
#include "esp_app_desc.h"
#include "esp_err.h"
typedef struct { char label[17]; } esp_partition_t;
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *out);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
