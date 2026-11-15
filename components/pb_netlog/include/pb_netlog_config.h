// SPDX-License-Identifier: MIT
// pb_netlog_config — the pure parts of the syslog and NTP settings: the
// settings struct, the defaults and the host name check. No ESP-IDF headers,
// so the host test compiles this file.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Longest host name or IPv4 address. dc_syslog and dc_time accept 63 characters.
#define PB_NETLOG_HOST_MAX            63
#define PB_NETLOG_DEFAULT_SYSLOG_PORT 514

typedef struct {
    char syslog_host[PB_NETLOG_HOST_MAX + 1];  // "" = syslog is off
    uint16_t syslog_port;                      // UDP port, 1 to 65535
    char ntp_host[PB_NETLOG_HOST_MAX + 1];     // "" = only the NTP server from DHCP
} pb_netlog_config_t;

// Syslog off, port 514, no NTP server (DHCP only).
void pb_netlog_config_default(pb_netlog_config_t *out);

// True if host is "" (off) or a host name or an IPv4 address: a maximum of
// PB_NETLOG_HOST_MAX characters, labels of 1 to 63 letters, digits and
// hyphens, separated by dots. A label does not start or end with a hyphen.
bool pb_netlog_host_valid(const char *host);

// True if all fields of cfg are valid (host names and a port that is not 0).
bool pb_netlog_config_valid(const pb_netlog_config_t *cfg);
