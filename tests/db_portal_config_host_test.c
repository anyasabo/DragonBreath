#include "db_portal_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void current_configs(dc_moonraker_config_t *mr, dc_bambu_config_t *bb,
                            pb_ha_config_t *ha, db_km_config_t *km,
                            dc_prusa_config_t *pr)
{
    snprintf(pr->host, sizeof(pr->host), "prusa.lan");
    pr->port = 80;
    snprintf(pr->api_key, sizeof(pr->api_key), "prusa-secret");
    snprintf(mr->host, sizeof(mr->host), "moonraker.lan");
    mr->port = 7125;
    snprintf(mr->api_key, sizeof(mr->api_key), "moon-secret");
    snprintf(bb->host, sizeof(bb->host), "bambu.lan");
    snprintf(bb->serial, sizeof(bb->serial), "01P00SERIAL");
    snprintf(bb->code, sizeof(bb->code), "bambu-secret");
    snprintf(ha->host, sizeof(ha->host), "ha-broker.lan");
    ha->port = 1883;
    snprintf(ha->user, sizeof(ha->user), "ha-user");
    snprintf(ha->pass, sizeof(ha->pass), "ha-secret");
    snprintf(ha->topic, sizeof(ha->topic), "dragonbreath");
    snprintf(km->host, sizeof(km->host), "km-broker.lan");
    km->port = 1883;
    snprintf(km->user, sizeof(km->user), "km-user");
    snprintf(km->pass, sizeof(km->pass), "km-secret");
    snprintf(km->inst, sizeof(km->inst), "printer");
    snprintf(km->topic, sizeof(km->topic), "dragonbreath");
    km->tls = false;
    km->writeback = true;
}

static esp_err_t plan(const db_portal_product_request_t *request,
                      db_portal_product_plan_t *out, char *message, size_t size)
{
    dc_moonraker_config_t mr = {0};
    dc_bambu_config_t bb = {0};
    pb_ha_config_t ha = {0};
    db_km_config_t km = {0};
    dc_prusa_config_t pr = {0};
    pb_netlog_config_t nl;
    current_configs(&mr, &bb, &ha, &km, &pr);
    pb_netlog_config_default(&nl);
    snprintf(nl.ntp_host, sizeof(nl.ntp_host), "ntp.lan");
    return db_portal_plan_product_save(request, DC_SRC_KLIPPER, &mr, &bb, &ha,
                                       &km, &pr, &nl, out, message, size);
}

int main(void)
{
    db_portal_product_plan_t result;
    char message[128];

    // Selecting None is an unbind only: every saved credential remains available.
    db_portal_product_request_t unbind = {
        .source_present = true,
        .source = DC_SRC_NONE,
    };
    assert(plan(&unbind, &result, message, sizeof(message)) == ESP_OK);
    assert(result.source_changed && result.source == DC_SRC_NONE);
    assert(!result.moonraker_changed && !result.bambu_changed &&
           !result.ha_changed && !result.klipper_mqtt_changed);
    assert(strcmp(result.moonraker.api_key, "moon-secret") == 0);
    assert(strcmp(result.bambu.code, "bambu-secret") == 0);
    assert(strcmp(result.ha.pass, "ha-secret") == 0);
    assert(strcmp(result.klipper_mqtt.pass, "km-secret") == 0);

    // A full SPA echo of unchanged fields must produce no NVS writes.
    db_portal_product_request_t unchanged = {
        .source_present = true, .source = DC_SRC_KLIPPER,
        .mr_host = {true, "moonraker.lan"}, .mr_port = {true, 7125},
        .mr_key = {true, ""},
        .bb_host = {true, "bambu.lan"}, .bb_serial = {true, "01P00SERIAL"},
        .bb_code = {true, ""},
        .ha_host = {true, "ha-broker.lan"}, .ha_port = {true, 1883},
        .ha_user = {true, "ha-user"}, .ha_pass = {true, ""},
        .ha_topic = {true, "dragonbreath"},
        .km_host = {true, "km-broker.lan"}, .km_port = {true, 1883},
        .km_user = {true, "km-user"}, .km_pass = {true, ""},
        .km_inst = {true, "printer"}, .km_topic = {true, "dragonbreath"},
        .km_tls = {true, false}, .km_writeback = {true, true},
        .pr_host = {true, "prusa.lan"}, .pr_key = {true, ""},
        .sl_host = {true, ""}, .sl_port = {true, 514}, .ntp_host = {true, "ntp.lan"},
    };
    assert(plan(&unchanged, &result, message, sizeof(message)) == ESP_OK);
    assert(!result.source_changed && !result.moonraker_changed &&
           !result.bambu_changed && !result.ha_changed &&
           !result.klipper_mqtt_changed && !result.prusa_changed &&
           !result.netlog_changed);

    // Blank secrets retain credentials while ordinary changed fields are staged.
    db_portal_product_request_t changed = {
        .mr_host = {true, "new-moonraker.lan"},
        .mr_key = {true, ""},
        .km_port = {true, 8883},
        .km_pass = {true, ""},
        .km_tls = {true, true},
    };
    assert(plan(&changed, &result, message, sizeof(message)) == ESP_OK);
    assert(result.moonraker_changed && result.klipper_mqtt_changed);
    assert(strcmp(result.moonraker.host, "new-moonraker.lan") == 0);
    assert(strcmp(result.moonraker.api_key, "moon-secret") == 0);
    assert(result.klipper_mqtt.port == 8883 && result.klipper_mqtt.tls);
    assert(strcmp(result.klipper_mqtt.pass, "km-secret") == 0);

    // An edit to one inactive-source field must retain every untouched value
    // supplied by that source's persisted config snapshot.
    db_portal_product_request_t partial_bambu = {
        .bb_host = {true, "new-bambu.lan"},
        .bb_code = {true, ""},
    };
    assert(plan(&partial_bambu, &result, message, sizeof(message)) == ESP_OK);
    assert(result.bambu_changed);
    assert(strcmp(result.bambu.host, "new-bambu.lan") == 0);
    assert(strcmp(result.bambu.serial, "01P00SERIAL") == 0);
    assert(strcmp(result.bambu.code, "bambu-secret") == 0);
    assert(!result.moonraker_changed && !result.ha_changed &&
           !result.klipper_mqtt_changed && !result.source_changed);

    // Prusa: a changed host is staged; a blank API key retains the stored password.
    db_portal_product_request_t prusa_change = {
        .pr_host = {true, "prusa-new.lan"},
        .pr_key = {true, ""},
    };
    assert(plan(&prusa_change, &result, message, sizeof(message)) == ESP_OK);
    assert(result.prusa_changed);
    assert(strcmp(result.prusa.host, "prusa-new.lan") == 0);
    assert(strcmp(result.prusa.api_key, "prusa-secret") == 0);
    assert(!result.moonraker_changed && !result.bambu_changed &&
           !result.ha_changed && !result.klipper_mqtt_changed &&
           !result.source_changed);

    // Prusa: a new API key overwrites; the host is left untouched.
    db_portal_product_request_t prusa_key = {.pr_key = {true, "new-prusa-key"}};
    assert(plan(&prusa_key, &result, message, sizeof(message)) == ESP_OK);
    assert(result.prusa_changed);
    assert(strcmp(result.prusa.api_key, "new-prusa-key") == 0);
    assert(strcmp(result.prusa.host, "prusa.lan") == 0);

    // Prusa: an overlong host is rejected in the planning phase (no silent truncation).
    char pr_too_long[80];
    memset(pr_too_long, 'p', sizeof(pr_too_long));
    pr_too_long[sizeof(pr_too_long) - 1] = '\0';
    db_portal_product_request_t prusa_invalid = {.pr_host = {true, pr_too_long}};
    assert(plan(&prusa_invalid, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "pr_host") != NULL);

    // Validation fails in the pure planning phase, before any setter can run.
    char too_long[80];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    db_portal_product_request_t invalid = {.ha_host = {true, too_long}};
    assert(plan(&invalid, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "ha_host") != NULL);

    db_portal_product_request_t invalid_source = {
        .source_present = true,
        .source = DC_SRC_MAX,
    };
    assert(plan(&invalid_source, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "ctl_src") != NULL);

    db_portal_product_request_t invalid_port = {.ha_port = {true, 0}};
    assert(plan(&invalid_port, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "port") != NULL);

    // Syslog and NTP: a host name or an IPv4 address is staged, the port too.
    db_portal_product_request_t netlog = {
        .sl_host = {true, "192.168.1.10"}, .sl_port = {true, 1514},
    };
    assert(plan(&netlog, &result, message, sizeof(message)) == ESP_OK);
    assert(result.netlog_changed);
    assert(strcmp(result.netlog.syslog_host, "192.168.1.10") == 0);
    assert(result.netlog.syslog_port == 1514);
    assert(strcmp(result.netlog.ntp_host, "ntp.lan") == 0);
    assert(!result.moonraker_changed && !result.source_changed);

    // An empty NTP server is valid: it means the DHCP server only.
    db_portal_product_request_t ntp_off = {.ntp_host = {true, ""}};
    assert(plan(&ntp_off, &result, message, sizeof(message)) == ESP_OK);
    assert(result.netlog_changed && result.netlog.ntp_host[0] == '\0');

    // Not a host name: the plan refuses it and names the field.
    db_portal_product_request_t bad_syslog = {.sl_host = {true, "alloy lan"}};
    assert(plan(&bad_syslog, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "sl_host") != NULL);
    db_portal_product_request_t bad_ntp = {.ntp_host = {true, "-ntp.lan"}};
    assert(plan(&bad_ntp, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "ntp_host") != NULL);
    db_portal_product_request_t long_syslog = {.sl_host = {true, too_long}};
    assert(plan(&long_syslog, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);
    assert(strstr(message, "sl_host") != NULL);
    db_portal_product_request_t zero_port = {.sl_port = {true, 0}};
    assert(plan(&zero_port, &result, message, sizeof(message)) == ESP_ERR_INVALID_ARG);

    // pb_netlog_host_valid: the rules of the host name check.
    char max_host[PB_NETLOG_HOST_MAX + 2];
    memset(max_host, 'a', sizeof(max_host));
    max_host[PB_NETLOG_HOST_MAX] = '\0';
    assert(pb_netlog_host_valid(max_host));
    max_host[PB_NETLOG_HOST_MAX] = 'a';
    max_host[PB_NETLOG_HOST_MAX + 1] = '\0';
    assert(!pb_netlog_host_valid(max_host));
    assert(pb_netlog_host_valid(""));
    assert(pb_netlog_host_valid("homeserver"));
    assert(pb_netlog_host_valid("alloy.home-lab.lan"));
    assert(pb_netlog_host_valid("10.0.0.1"));
    assert(!pb_netlog_host_valid(NULL));
    assert(!pb_netlog_host_valid("."));
    assert(!pb_netlog_host_valid("a..b"));
    assert(!pb_netlog_host_valid("a.b."));
    assert(!pb_netlog_host_valid(".a"));
    assert(!pb_netlog_host_valid("a-.b"));
    assert(!pb_netlog_host_valid("a.-b"));
    assert(!pb_netlog_host_valid("a-"));
    assert(!pb_netlog_host_valid("a_b"));
    assert(!pb_netlog_host_valid("a:514"));
    assert(!pb_netlog_host_valid("a\nb"));
    pb_netlog_config_t cfg;
    pb_netlog_config_default(&cfg);
    assert(pb_netlog_config_valid(&cfg) && cfg.syslog_port == 514 &&
           cfg.syslog_host[0] == '\0' && cfg.ntp_host[0] == '\0');
    cfg.syslog_port = 0;
    assert(!pb_netlog_config_valid(&cfg));
    assert(!pb_netlog_config_valid(NULL));

    puts("db portal config planning tests: PASS");
    return 0;
}
