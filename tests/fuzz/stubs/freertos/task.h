#pragma once
#include "freertos/FreeRTOS.h"
// No task runs. fuzz_host.c counts the calls: a restart task from a handler
// that returned an error is a broken invariant.
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, unsigned prio, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
