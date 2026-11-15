idf_component_register(
    SRCS "pb_heater.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board pb_ntc dc_pid esp_driver_gpio esp_timer nvs_flash
)
