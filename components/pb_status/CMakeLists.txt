idf_component_register(
    SRCS "pb_status.c" "pb_status_line.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log
)
