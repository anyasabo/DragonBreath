idf_component_register(
    SRCS "pb_leds.c"
    INCLUDE_DIRS "include"
    REQUIRES pb_board esp_driver_gpio nvs_flash
)
