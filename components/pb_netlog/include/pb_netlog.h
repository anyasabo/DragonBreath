// SPDX-License-Identifier: MIT
// pb_netlog — syslog and an NTP wall clock for diagnostics.
//
// The settings are in NVS (app_nvs: sl_host, sl_port, ntp_host). The portal
// writes them. pb_netlog_start() reads them once at boot, so a change applies
// after a restart.
//
// - Syslog (dc_syslog): if sl_host is set, the device sends its ESP-IDF log as
//   RFC 5424 over UDP. APP-NAME is "dragonbreath", HOSTNAME is the dc_wifi host
//   name. If sl_host is empty, syslog is off.
// - Clock (dc_time): SNTP with the NTP server from DHCP, and ntp_host as a
//   second server if it is set. dc_time starts only if syslog is on or ntp_host
//   is set. Its only use is the log timestamp.
//
// SAFETY: nothing related to safety or control uses the wall clock. The heater,
// the fan and the watchdogs use esp_timer and the FreeRTOS tick only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pb_netlog_config.h"

// Read the settings from NVS. If a key is missing or not valid, its default is
// used. Always fills out. Returns the NVS error if NVS could not be read.
esp_err_t pb_netlog_get_config(pb_netlog_config_t *out);

// Validate and save the settings. They apply at the next boot.
// ESP_ERR_INVALID_ARG if a field is not valid.
esp_err_t pb_netlog_set_config(const pb_netlog_config_t *cfg);

// Call once, after dc_wifi_start() returned ESP_OK. Errors are logged. The
// device continues without syslog or a clock.
//
// LIMIT: dc_wifi_start() returns after the station has its first DHCP lease.
// lwIP gives the DHCP NTP server (option 42) to SNTP only from a DHCP reply
// that comes after dc_time_start(): the next renewal (usually after half the
// lease time) or the next reconnect. Until then, only ntp_host can sync the
// clock. Set ntp_host if the log timestamps are important.
void pb_netlog_start(void);

typedef struct {
    bool syslog_enabled;        // dc_syslog runs
    uint32_t syslog_sent;       // datagrams that sendto() accepted
    uint32_t syslog_dropped;    // lines dropped because the queue was full
    uint32_t syslog_send_errors;// failed sends and address lookups
    uint32_t syslog_queued;     // lines in the queue now
    bool time_synced;           // SNTP synced at least once
} pb_netlog_stats_t;

// Copy the syslog counters and the clock state. Does not block. If syslog is
// off, the syslog fields are 0. Any task can call it.
void pb_netlog_get_stats(pb_netlog_stats_t *out);
