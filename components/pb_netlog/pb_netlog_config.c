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

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// True if lwIP's ipaddr_aton() (like inet_aton()) reads the label as a number:
// all digits, or "0x" and zero or more hex digits.
static bool label_numeric(const char *p, size_t n)
{
    size_t i = 0;
    if (n >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        for (i = 2; i < n; i++)
            if (!is_hex(p[i])) return false;
        return true;
    }
    for (i = 0; i < n; i++)
        if (!is_digit(p[i])) return false;
    return n > 0;
}

// A dotted-quad octet: decimal 0 to 255 without a leading zero (inet_aton()
// reads "010" as octal 8).
static bool octet_ok(const char *p, size_t n)
{
    if (n == 0 || n > 3 || (n > 1 && p[0] == '0')) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(p[i])) return false;
        v = v * 10 + (unsigned)(p[i] - '0');
    }
    return v <= 255;
}

bool pb_netlog_host_valid(const char *host)
{
    if (!host) return false;
    size_t len = 0;
    while (len <= PB_NETLOG_HOST_MAX && host[len]) len++;
    if (len > PB_NETLOG_HOST_MAX) return false;
    if (len == 0) return true;   // off

    // Pass 1: the host name syntax.
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
    if (label == 0 || host[len - 1] == '-') return false;

    // Pass 2: if each label is a number, lwIP reads the text as an address,
    // not as a name. "1.2.3" becomes 1.2.0.3. Accept only a dotted quad.
    unsigned labels = 0;
    bool all_numeric = true;
    bool quad = true;
    const char *start = host;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && host[i] != '.') continue;
        size_t n = (size_t)(host + i - start);
        labels++;
        if (!label_numeric(start, n)) all_numeric = false;
        if (!octet_ok(start, n)) quad = false;
        start = host + i + 1;
    }
    return !all_numeric || (labels == 4 && quad);
}

bool pb_netlog_config_valid(const pb_netlog_config_t *cfg)
{
    return cfg && cfg->syslog_port != 0 &&
           pb_netlog_host_valid(cfg->syslog_host) &&
           pb_netlog_host_valid(cfg->ntp_host);
}
