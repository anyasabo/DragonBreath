// SPDX-License-Identifier: MIT
// GET /metrics — Prometheus text format 0.0.4. Unauthenticated and read-only,
// like GET /api/v2/state. Private to pb_httpd.
#pragma once
#include "esp_http_server.h"

esp_err_t pb_httpd_metrics_get(httpd_req_t *req);
