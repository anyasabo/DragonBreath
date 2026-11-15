#pragma once
// Host stand-in for the ESP-IDF esp_http_server.h (v5.3). Only the parts that
// the DragonBreath handlers use. fuzz_host.c implements the functions: it feeds
// the fuzz input to httpd_req_recv() and records the response.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

#define ESP_ERR_HTTPD_BASE         0xb000
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 3)
#define ESP_ERR_HTTPD_INVALID_REQ  (ESP_ERR_HTTPD_BASE + 6)

#define HTTPD_SOCK_ERR_FAIL    -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3

#define HTTPD_RESP_USE_STRLEN -1

typedef void *httpd_handle_t;

typedef enum {
    HTTP_DELETE = 0,
    HTTP_GET = 1,
    HTTP_HEAD = 2,
    HTTP_POST = 3,
    HTTP_PUT = 4,
} httpd_method_t;

typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 0,
    HTTPD_501_METHOD_NOT_IMPLEMENTED,
    HTTPD_505_VERSION_NOT_SUPPORTED,
    HTTPD_400_BAD_REQUEST,
    HTTPD_401_UNAUTHORIZED,
    HTTPD_403_FORBIDDEN,
    HTTPD_404_NOT_FOUND,
    HTTPD_405_METHOD_NOT_ALLOWED,
    HTTPD_408_REQ_TIMEOUT,
    HTTPD_411_LENGTH_REQUIRED,
    HTTPD_414_URI_TOO_LONG,
    HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE,
    HTTPD_ERR_CODE_MAX
} httpd_err_code_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    const char uri[513];
    size_t content_len;
    void *aux;
    void *user_ctx;
    void *sess_ctx;
} httpd_req_t;

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    unsigned task_priority;
    size_t stack_size;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    bool lru_purge_enable;
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { \
    .task_priority = 5, .stack_size = 4096, .max_open_sockets = 7, \
    .max_uri_handlers = 8, .recv_wait_timeout = 5, .send_wait_timeout = 5, \
    .lru_purge_enable = false, .keep_alive_enable = false, \
    .keep_alive_idle = 0, .keep_alive_interval = 0, .keep_alive_count = 0, \
}

esp_err_t httpd_register_uri_handler(httpd_handle_t hd, const httpd_uri_t *uri);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
int httpd_req_to_sockfd(httpd_req_t *r);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *r);

static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    return httpd_resp_send(r, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}

static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *str)
{
    return httpd_resp_send_chunk(r, str, (str == NULL) ? 0 : HTTPD_RESP_USE_STRLEN);
}
