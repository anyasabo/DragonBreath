#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t w) { (void)s; (void)w; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
