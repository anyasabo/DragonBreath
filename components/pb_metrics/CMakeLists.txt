idf_component_register(
    SRCS "pb_metrics.c" "pb_metrics_fmt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash
)
