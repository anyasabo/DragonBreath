#pragma once
#include <stdint.h>

// The test counts the critical-section depth, so that it can make sure that
// no NVS or GPIO call runs inside the spinlock and that each enter has an exit.
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portMAX_DELAY 0xffffffffU
#define pdTRUE  1
#define pdFALSE 0

void host_critical_enter(portMUX_TYPE *mux);
void host_critical_exit(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) host_critical_enter(mux)
#define taskEXIT_CRITICAL(mux)  host_critical_exit(mux)
