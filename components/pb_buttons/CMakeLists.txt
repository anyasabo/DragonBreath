idf_component_register(
    SRCS "pb_buttons.c" "pb_buttons_sm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio pb_board
)
