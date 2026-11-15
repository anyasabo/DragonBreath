# ESP-IDF 6 removed the built-in json component. On ESP-IDF 6 and later,
# idf_component.yml adds the registry cJSON component instead.
set(PB_HTTPD_JSON json)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(PB_HTTPD_JSON "")
endif()

idf_component_register(
    SRCS "pb_httpd.c" "pb_httpd_metrics.c" "pb_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server pb_heater pb_ntc pb_leds pb_policy dc_source dc_bambu dc_moonraker dc_prusa dc_evlog ${PB_HTTPD_JSON} nvs_flash app_update
             esp_wifi esp_netif esp_timer esp_system pb_metrics
)
