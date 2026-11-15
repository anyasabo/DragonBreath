// SPDX-License-Identifier: MIT
// pb_httpd — authoritative DragonBreath HTTP/JSON API v2.
//
//   GET  /api/v2/info
//   GET  /api/v2/state
//   GET  /api/v2/events       Server-Sent Events: state + telemetry snapshots
//   GET  /api/v2/health
//   POST /api/v2/command      revision-aware mode/control command
//   POST /api/v2/heartbeat    refresh exactly one device-issued lease
//
// The alpha /status, /target, /heartbeat, and /reset contract is deliberately
// removed. Read-only requests have no control side effects. A remote POWER_ON
// session stays alive only when its exact device-issued lease is heartbeated.
//
// CSRF / control gate: every mutating endpoint (command, heartbeat, and
// the portal's STA-mode /save) requires the custom header "X-DragonBreath-Auth".
// A cross-origin HTML form cannot set a custom header and we never enable CORS,
// so an ordinary drive-by page cannot command the heater or rewrite its Wi-Fi
// config. Read-only API routes stay open. This is CSRF hardening for a
// trusted LAN, not transport security.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>
#include <stdbool.h>

#define PB_AUTH_HEADER "X-DragonBreath-Auth"

// Register DragonBreath's product API on the HTTP server owned by dc_portal.
// Device/boot identity is initialized here before any request can be served.
esp_err_t pb_httpd_register(httpd_handle_t server);

// True if the request carries a valid PB_AUTH_HEADER. If a control token is
// configured (NVS app_nvs/"ctl_token"), the header must equal it (constant-time
// compare); otherwise any non-empty value passes (presence-only CSRF gate).
// CONFIG_DB_REQUIRE_CONTROL_TOKEN: with no token, it passes only for a request
// that arrived on the setup AP while the Wi-Fi station is not associated. CONFIG_DB_AUTH_LOCKOUT: after
// repeated wrong tokens, it fails for a short time. See pb_auth.h.
bool pb_httpd_auth_ok(httpd_req_t *req);

// Run the same check. On failure, send the API v2 error (403 auth_failed or
// token_required, 429 auth_locked) and return true: the handler must stop.
bool pb_httpd_auth_reject(httpd_req_t *req);

// Store the control token in NVS (NULL or "" erases it). A token must pass
// pb_auth_token_valid() (1-64 printable ASCII characters, no spaces).
esp_err_t pb_httpd_set_ctl_token(const char *token);

// Copy the configured control token into out ("" if none). For device-side use
// only: the served pages never contain the token. The dashboard asks the user
// for it and keeps it in the browser (localStorage).
void pb_httpd_ctl_token(char *out, size_t outsz);
