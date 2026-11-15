// SPDX-License-Identifier: MIT
// Host environment for the tests that compile the REAL pb_heater.c (and
// pb_ntc.c): a clock that the test controls, a GPIO recorder, an in-memory NVS
// with fault injection, a critical-section counter and a mutex model. The
// headers that pb_heater.c and pb_ntc.c include are in tests/stubs_heater/.
//
// The whole environment lives in one MAP_SHARED block. Each scenario runs in a
// child process that is forked from a parent that never calls pb_heater, so
// each child starts with the static state of a fresh boot. A "reboot" is a new
// child: the NVS contents in the shared block survive it, the RAM state of
// pb_heater does not.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "nvs.h"

#define HOST_NVS_MAX_KEYS    32
#define HOST_NVS_KEY_LEN     16
#define HOST_NVS_MAX_HANDLES 4
#define HOST_NVS_MAX_STAGED  8
#define HOST_ADC_CHANNELS    5

typedef enum { HOST_NVS_U8 = 1, HOST_NVS_U32, HOST_NVS_I32 } host_nvs_type_t;

typedef struct {
    char key[HOST_NVS_KEY_LEN];
    host_nvs_type_t type;
    uint32_t value;
    bool present;
} host_nvs_entry_t;

typedef struct {
    bool open;
    bool writable;
    int n_staged;
    host_nvs_entry_t staged[HOST_NVS_MAX_STAGED];
} host_nvs_handle_t;

// Which task context the test simulates. Only CTX_CONTROL and CTX_BOOT
// (app_main before the control task starts) can write the relay GPIO.
typedef enum { CTX_BOOT = 0, CTX_CONTROL, CTX_OTHER } host_ctx_t;

typedef struct {
    int64_t now_us;
    host_ctx_t ctx;

    // GPIO
    int level[GPIO_NUM_MAX];
    unsigned writes[GPIO_NUM_MAX];
    unsigned gpio_config_calls;
    uint64_t gpio_config_mask;
    int gpio_config_mode;
    esp_err_t gpio_config_err;

    // FreeRTOS
    int crit_depth;
    bool sem_created;
    int sem_held;
    bool sem_contended;          // a zero-wait take fails (another task holds it)
    void (*after_give_hook)(void);   // one-shot: runs after the next mutex give

    // NVS ("app_nvs" is the only namespace the code under test uses)
    bool nvs_ns_exists;
    host_nvs_entry_t nvs[HOST_NVS_MAX_KEYS];
    host_nvs_handle_t handles[HOST_NVS_MAX_HANDLES];
    esp_err_t nvs_fail_open_ro;  // 0 = no injected failure
    esp_err_t nvs_fail_open_rw;
    esp_err_t nvs_fail_get;
    esp_err_t nvs_fail_set;
    esp_err_t nvs_fail_commit;
    unsigned nvs_commits;

    // Sensor inputs for the pb_ntc API stub (heater tick test only).
    int ntc_status[2];           // pb_ntc_status_t, indexed by pb_ntc_channel_t
    float ntc_temp[2];
    int rref_kohm;
    unsigned ntc_reads;

    // ADC inputs for the real pb_ntc.c (chain test only), indexed by ADC channel.
    int adc_raw[HOST_ADC_CHANNELS];
    int adc_mv[HOST_ADC_CHANNELS];
    esp_err_t adc_read_err[HOST_ADC_CHANNELS];
    esp_err_t adc_cali_err;
    esp_err_t adc_new_unit_err;
    esp_err_t adc_config_err;
    esp_err_t adc_cali_create_err;
} host_env_t;

extern host_env_t *env;

// Map the shared block and set defaults. Call once from main().
void env_init(void);
// Zero the block (GPIO, NVS, injections, clock) for a new independent scenario.
void env_reset(void);
// Run fn in a forked child. Returns true if the child exited with status 0.
bool env_run(const char *name, void (*fn)(void));
// Like env_run, but return the raw exit code of the child (-1 on a signal).
int env_run_code(void (*fn)(void));

// Print a message and terminate the child with a failure.
void env_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4), noreturn));
#define CHECK(expr) do { if (!(expr)) env_fail(__FILE__, __LINE__, "CHECK failed: %s", #expr); } while (0)
#define CHECKF(expr, ...) do { if (!(expr)) env_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)

// No critical section open and no mutex held. Call after each API call.
void env_check_quiescent(void);

// Direct access to the committed NVS store (bypasses the injected failures).
bool env_nvs_read(const char *key, uint32_t *out);
void env_nvs_write(const char *key, host_nvs_type_t type, uint32_t value);
void env_nvs_erase(const char *key);
