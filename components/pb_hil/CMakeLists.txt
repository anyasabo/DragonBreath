# ESP-IDF 6 removed the built-in json component. On ESP-IDF 6 and later,
# idf_component.yml adds the registry cJSON component instead.
set(PB_HIL_JSON json)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(PB_HIL_JSON "")
endif()

idf_component_register(
    SRCS "pb_hil.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag ${PB_HIL_JSON} pb_fan pb_leds pb_ntc pb_policy pb_buttons
)
