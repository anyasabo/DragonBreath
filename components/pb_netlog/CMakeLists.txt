idf_component_register(
    SRCS "pb_netlog.c" "pb_netlog_config.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES dc_syslog dc_time dc_wifi log nvs_flash
)
