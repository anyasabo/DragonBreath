#pragma once
#include <stdint.h>

// Single-threaded mutex model. The test can make a take with a zero wait fail,
// to simulate another task that holds the lock.
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t wait);
int xSemaphoreGive(SemaphoreHandle_t sem);
