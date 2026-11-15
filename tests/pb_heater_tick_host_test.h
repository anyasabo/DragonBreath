// SPDX-License-Identifier: MIT
// Shared helpers of pb_heater_tick_host_test.c and pb_heater_tick_random.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "heater_host_env.h"
#include "pb_board.h"
#include "pb_ntc.h"

int relay(void);                 // current level of the relay GPIO
void tick(void);                 // pb_heater_tick() in the control-task context
void boot(void);                 // init + load_config + load_fault, with checks
void advance_ms(int64_t ms);
void sensors(float chamber_c, float ptc_c);   // both channels OK
esp_err_t set_target(float c);
void link_alive(void);
esp_err_t clear_fault(void);
int nvs_u(const char *key);      // committed NVS value, or -1 if absent

// Randomized invariant test. Returns true if all seeds pass.
bool heater_random_test(bool strict_gaps);
