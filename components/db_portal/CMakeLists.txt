# ESP-IDF 6 removed the built-in json component. On ESP-IDF 6 and later,
# idf_component.yml adds the registry cJSON component instead.
set(DB_PORTAL_JSON json)
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DB_PORTAL_JSON "")
endif()

idf_component_register(
    SRCS "db_portal.c" "db_portal_config.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_portal pb_httpd pb_policy dc_source dc_moonraker dc_bambu dc_prusa pb_ha
             db_klipper_mqtt nvs_flash ${DB_PORTAL_JSON} app_update
)

target_add_binary_data(${COMPONENT_LIB} "${COMPONENT_DIR}/www/favicon.png" BINARY)
