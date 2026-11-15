// SPDX-License-Identifier: MIT
// Implementation of the host environment in heater_host_env.h.
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "heater_host_env.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "pb_board.h"

host_env_t *env;

void env_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    exit(1);
}

#define ENV_REQUIRE(expr, ...) do { if (!(expr)) env_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)

void env_init(void)
{
    void *p = mmap(NULL, sizeof(host_env_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    env = p;
    env_reset();
}

void env_reset(void)
{
    memset(env, 0, sizeof(*env));
    env->now_us = 1000000;        // 1 s after boot
    env->rref_kohm = 82;
    for (int i = 0; i < 2; ++i) {
        env->ntc_status[i] = 0;   // PB_NTC_OK
        env->ntc_temp[i] = 25.0f;
    }
}

int env_run_code(void (*fn)(void))
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        fn();
        fflush(stdout);
        exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        exit(2);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool env_run(const char *name, void (*fn)(void))
{
    int code = env_run_code(fn);
    if (code != 0) {
        fprintf(stderr, "FAIL: %s (exit code %d)\n", name, code);
        return false;
    }
    printf("ok   %s\n", name);
    return true;
}

void env_check_quiescent(void)
{
    ENV_REQUIRE(env->crit_depth == 0, "critical section left open (depth %d)", env->crit_depth);
    ENV_REQUIRE(env->sem_held == 0, "persist mutex left held (%d)", env->sem_held);
    for (int i = 0; i < HOST_NVS_MAX_HANDLES; ++i)
        ENV_REQUIRE(!env->handles[i].open, "NVS handle %d left open", i);
}

// --- esp_err / esp_timer ------------------------------------------------------

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ESP_ERR_other";
    }
}

int64_t esp_timer_get_time(void) { return env->now_us; }

// --- FreeRTOS -------------------------------------------------------------------

void host_critical_enter(portMUX_TYPE *mux)
{
    // ESP-IDF allows a task to take the same portMUX again (it counts the depth).
    ENV_REQUIRE(env->crit_depth == 0 || env->crit_mux == (void *)mux,
                "nested critical section on a different portMUX");
    env->crit_mux = mux;
    env->crit_depth++;
}

void host_critical_exit(portMUX_TYPE *mux)
{
    ENV_REQUIRE(env->crit_depth >= 1 && env->crit_mux == (void *)mux,
                "critical exit without a matching enter");
    if (--env->crit_depth == 0) env->crit_mux = NULL;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    env->sem_created = true;
    return (SemaphoreHandle_t)&env->sem_held;
}

int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t wait)
{
    ENV_REQUIRE(sem == (SemaphoreHandle_t)&env->sem_held, "unknown mutex");
    ENV_REQUIRE(env->crit_depth == 0, "mutex take inside a critical section");
    if (env->sem_contended) {
        // A blocking take would wait for the other task. The test never
        // simulates that, so a blocking take while contended is a test error.
        ENV_REQUIRE(wait == 0, "blocking mutex take while the test holds it");
        return pdFALSE;
    }
    ENV_REQUIRE(env->sem_held == 0, "recursive take of a non-recursive mutex");
    env->sem_held++;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t sem)
{
    ENV_REQUIRE(sem == (SemaphoreHandle_t)&env->sem_held, "unknown mutex");
    ENV_REQUIRE(env->sem_held == 1, "give of a mutex that is not held");
    env->sem_held--;
    if (env->after_give_hook) {
        // Simulate another task that runs exactly at this point.
        void (*hook)(void) = env->after_give_hook;
        env->after_give_hook = NULL;
        hook();
    }
    return pdTRUE;
}

// --- GPIO -------------------------------------------------------------------------

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    ENV_REQUIRE(env->crit_depth == 0, "gpio_config inside a critical section");
    env->gpio_config_calls++;
    env->gpio_config_mask = cfg->pin_bit_mask;
    env->gpio_config_mode = (int)cfg->mode;
    return env->gpio_config_err;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    ENV_REQUIRE((int)pin >= 0 && pin < GPIO_NUM_MAX, "bad GPIO %d", (int)pin);
    // pb_heater must write only the relay. The fan gate belongs to pb_fan.
    ENV_REQUIRE(pin == PB_GPIO_RELAY, "write to GPIO %d (only the relay GPIO %d is expected)",
                (int)pin, (int)PB_GPIO_RELAY);
    // SAFETY.md, "Rule for contributors": only the control task writes the relay.
    ENV_REQUIRE(env->ctx != CTX_OTHER, "relay GPIO written from a non-control task");
    ENV_REQUIRE(env->crit_depth == 0, "relay GPIO written inside a critical section");
    ENV_REQUIRE(level <= 1, "relay level %u", (unsigned)level);
    env->level[pin] = (int)level;
    env->writes[pin]++;
    if (level) env->high_writes[pin]++;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) { return env->level[pin]; }

// --- NVS ------------------------------------------------------------------------------

static host_nvs_entry_t *store_find(const char *key)
{
    for (int i = 0; i < HOST_NVS_MAX_KEYS; ++i)
        if (env->nvs[i].present && strcmp(env->nvs[i].key, key) == 0) return &env->nvs[i];
    return NULL;
}

bool env_nvs_read(const char *key, uint32_t *out)
{
    host_nvs_entry_t *e = store_find(key);
    if (!e) return false;
    if (out) *out = e->value;
    return true;
}

void env_nvs_write(const char *key, host_nvs_type_t type, uint32_t value)
{
    host_nvs_entry_t *e = store_find(key);
    if (!e) {
        for (int i = 0; i < HOST_NVS_MAX_KEYS && !e; ++i)
            if (!env->nvs[i].present) e = &env->nvs[i];
        ENV_REQUIRE(e != NULL, "NVS store full");
        memset(e, 0, sizeof(*e));
        ENV_REQUIRE(strlen(key) < HOST_NVS_KEY_LEN, "NVS key too long: %s", key);
        strcpy(e->key, key);
        e->present = true;
    }
    e->type = type;
    e->value = value;
    env->nvs_ns_exists = true;
}

void env_nvs_erase(const char *key)
{
    host_nvs_entry_t *e = store_find(key);
    if (e) e->present = false;
}

static host_nvs_handle_t *handle_get(nvs_handle_t h)
{
    ENV_REQUIRE(h >= 1 && h <= HOST_NVS_MAX_HANDLES, "bad NVS handle %u", (unsigned)h);
    host_nvs_handle_t *hh = &env->handles[h - 1];
    ENV_REQUIRE(hh->open, "NVS handle %u not open", (unsigned)h);
    return hh;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    ENV_REQUIRE(env->crit_depth == 0, "nvs_open inside a critical section");
    ENV_REQUIRE(strcmp(ns, "app_nvs") == 0, "unexpected NVS namespace %s", ns);
    if (mode == NVS_READONLY && env->nvs_fail_open_ro) return env->nvs_fail_open_ro;
    if (mode == NVS_READWRITE && env->nvs_fail_open_rw) return env->nvs_fail_open_rw;
    if (mode == NVS_READONLY && !env->nvs_ns_exists) return ESP_ERR_NVS_NOT_FOUND;
    for (int i = 0; i < HOST_NVS_MAX_HANDLES; ++i) {
        if (!env->handles[i].open) {
            memset(&env->handles[i], 0, sizeof(env->handles[i]));
            env->handles[i].open = true;
            env->handles[i].writable = (mode == NVS_READWRITE);
            if (mode == NVS_READWRITE) env->nvs_ns_exists = true;
            *out = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    env_fail(__FILE__, __LINE__, "out of NVS handles (a handle leaks)");
}

static esp_err_t nvs_get(nvs_handle_t h, const char *key, host_nvs_type_t type, uint32_t *out)
{
    ENV_REQUIRE(env->crit_depth == 0, "nvs_get inside a critical section");
    (void)handle_get(h);
    if (env->nvs_fail_get) return env->nvs_fail_get;
    host_nvs_entry_t *e = store_find(key);
    if (!e || e->type != type) return ESP_ERR_NVS_NOT_FOUND;
    *out = e->value;
    return ESP_OK;
}

static esp_err_t nvs_set(nvs_handle_t h, const char *key, host_nvs_type_t type, uint32_t v)
{
    ENV_REQUIRE(env->crit_depth == 0, "nvs_set inside a critical section");
    host_nvs_handle_t *hh = handle_get(h);
    ENV_REQUIRE(hh->writable, "nvs_set on a read-only handle");
    hh->n_sets++;
    env->nvs_sets++;
    if (env->nvs_fail_set) return env->nvs_fail_set;
    if (env->nvs_fail_set_nth > 0 && hh->n_sets == env->nvs_fail_set_nth) return ESP_FAIL;
    env_nvs_write(key, type, v);         // write-through, as in ESP-IDF
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    uint32_t v = 0;
    esp_err_t err = nvs_get(h, key, HOST_NVS_U8, &v);
    if (err == ESP_OK) *out = (uint8_t)v;
    return err;
}

esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    return nvs_get(h, key, HOST_NVS_U32, out);
}

esp_err_t nvs_get_i32(nvs_handle_t h, const char *key, int32_t *out)
{
    uint32_t v = 0;
    esp_err_t err = nvs_get(h, key, HOST_NVS_I32, &v);
    if (err == ESP_OK) *out = (int32_t)v;
    return err;
}

esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v)
{
    return nvs_set(h, key, HOST_NVS_U8, v);
}

esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v)
{
    return nvs_set(h, key, HOST_NVS_U32, v);
}

esp_err_t nvs_set_i32(nvs_handle_t h, const char *key, int32_t v)
{
    return nvs_set(h, key, HOST_NVS_I32, (uint32_t)v);
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    ENV_REQUIRE(env->crit_depth == 0, "nvs_commit inside a critical section");
    (void)handle_get(h);
    if (env->nvs_fail_commit) return env->nvs_fail_commit;
    env->nvs_commits++;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    host_nvs_handle_t *hh = handle_get(h);
    hh->open = false;
}
