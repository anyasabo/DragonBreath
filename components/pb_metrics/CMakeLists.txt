idf_component_register(
    SRCS "pb_metrics.c" "pb_metrics_fmt.c" "pb_metrics_airflow.c" "pb_panic.c" "pb_panic_record.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash
    PRIV_REQUIRES esp_event esp_wifi freertos heap riscv
)

# Panic breadcrumb (pb_panic.c): wrap esp_panic_handler. The -u keeps the
# wrapper object in the link.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_esp_panic_handler")
