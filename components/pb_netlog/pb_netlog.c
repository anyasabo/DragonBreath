// SPDX-License-Identifier: MIT
// pb_netlog — syslog and NTP settings in NVS, and their start at boot.
#include "pb_netlog.h"

#include <string.h>

#include "dc_syslog.h"
#include "dc_time.h"
#include "dc_wifi.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "pb_netlog";

#define NVS_NS        "app_nvs"
#define KEY_SL_HOST   "sl_host"
#define KEY_SL_PORT   "sl_port"
#define KEY_NTP_HOST  "ntp_host"

#define APP_NAME      "dragonbreath"

// Written once by pb_netlog_start() in app_main, read by the metrics handler.
static volatile bool s_time_started;
static volatile bool s_syslog_started;

// Read one host string. If the key is missing or the value is not valid, the
// field keeps its default ("").
static void load_host(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    size_t sz = out_size;
    if (nvs_get_str(h, key, out, &sz) != ESP_OK || !pb_netlog_host_valid(out)) {
        if (out[0]) ESP_LOGW(TAG, "%s in NVS is not valid; using \"\"", key);
        out[0] = '\0';
    }
}

esp_err_t pb_netlog_get_config(pb_netlog_config_t *out)
{
    pb_netlog_config_default(out);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;   // nothing saved yet
    if (err != ESP_OK) return err;
    load_host(h, KEY_SL_HOST, out->syslog_host, sizeof out->syslog_host);
    load_host(h, KEY_NTP_HOST, out->ntp_host, sizeof out->ntp_host);
    uint16_t port = 0;
    if (nvs_get_u16(h, KEY_SL_PORT, &port) == ESP_OK && port != 0)
        out->syslog_port = port;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t pb_netlog_set_config(const pb_netlog_config_t *cfg)
{
    if (!pb_netlog_config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_SL_HOST, cfg->syslog_host);
    if (err == ESP_OK) err = nvs_set_u16(h, KEY_SL_PORT, cfg->syslog_port);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_NTP_HOST, cfg->ntp_host);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void pb_netlog_start(void)
{
    pb_netlog_config_t cfg;
    esp_err_t err = pb_netlog_get_config(&cfg);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "could not read the settings: %s (syslog and NTP off)",
                 esp_err_to_name(err));
    const bool syslog_on = cfg.syslog_host[0] != '\0';
    if (!syslog_on && !cfg.ntp_host[0]) {
        ESP_LOGI(TAG, "syslog off, NTP off");
        return;
    }

    // The clock first: the syslog timestamp callback reads it. DHCP option 42
    // is always on. ntp_host, if set, is the second server.
    const dc_time_config_t time_cfg = {
        .server = cfg.ntp_host,
        .use_dhcp = true,
    };
    err = dc_time_start(&time_cfg);
    if (err == ESP_OK)
        s_time_started = true;
    else
        ESP_LOGW(TAG, "dc_time_start: %s (log lines have no timestamp)",
                 esp_err_to_name(err));

    if (!syslog_on) return;
    char hostname[33] = {0};
    if (dc_wifi_get_hostname(hostname, sizeof hostname) != ESP_OK)
        hostname[0] = '\0';   // dc_syslog writes "-"
    const dc_syslog_config_t syslog_cfg = {
        .server = cfg.syslog_host,
        .port = cfg.syslog_port,
        .app_name = APP_NAME,
        .hostname = hostname,
        .timestamp = dc_time_format_now,
    };
    err = dc_syslog_start(&syslog_cfg);
    if (err == ESP_OK)
        s_syslog_started = true;
    else
        ESP_LOGW(TAG, "dc_syslog_start: %s (continuing without syslog)",
                 esp_err_to_name(err));
}

void pb_netlog_get_stats(pb_netlog_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_syslog_started) {
        dc_syslog_stats_t st;
        dc_syslog_stats(&st);
        out->syslog_enabled = true;
        out->syslog_sent = st.sent;
        out->syslog_dropped = st.dropped;
        out->syslog_send_errors = st.send_errors;
        out->syslog_queued = st.queued;
    }
    out->time_synced = s_time_started && dc_time_is_synced();
}
