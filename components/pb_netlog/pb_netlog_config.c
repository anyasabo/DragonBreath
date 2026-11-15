// SPDX-License-Identifier: MIT
#include "pb_netlog_config.h"

#include <string.h>

void pb_netlog_config_default(pb_netlog_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->syslog_port = PB_NETLOG_DEFAULT_SYSLOG_PORT;
}

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
}

bool pb_netlog_host_valid(const char *host)
{
    if (!host) return false;
    size_t len = 0;
    while (len <= PB_NETLOG_HOST_MAX && host[len]) len++;
    if (len > PB_NETLOG_HOST_MAX) return false;
    size_t label = 0;
    for (size_t i = 0; i < len; i++) {
        char c = host[i];
        if (c == '.') {
            if (label == 0 || host[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        if (!host_char(c)) return false;
        if (c == '-' && label == 0) return false;
        label++;
    }
    // "" is valid (off). Otherwise the last label must not be empty or end in '-'.
    return len == 0 || (label > 0 && host[len - 1] != '-');
}

bool pb_netlog_config_valid(const pb_netlog_config_t *cfg)
{
    return cfg && cfg->syslog_port != 0 &&
           pb_netlog_host_valid(cfg->syslog_host) &&
           pb_netlog_host_valid(cfg->ntp_host);
}
