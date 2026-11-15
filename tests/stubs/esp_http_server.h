#pragma once
// Minimal host stand-in for the ESP-IDF esp_http_server.h. Each host test
// defines the functions that it uses.
#include <stddef.h>
#include "esp_err.h"

#define HTTPD_SOCK_ERR_FAIL        -1
#define HTTPD_SOCK_ERR_INVALID     -2
#define HTTPD_SOCK_ERR_TIMEOUT     -3
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xb003

typedef struct httpd_req {
    size_t content_len;
    void *user_ctx;
} httpd_req_t;

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
