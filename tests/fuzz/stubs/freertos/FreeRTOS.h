#pragma once
// One thread runs the harness, so the locks and critical sections do nothing.
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m)  ((void)(m))
