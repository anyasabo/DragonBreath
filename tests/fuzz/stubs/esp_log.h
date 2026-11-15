#pragma once
// Logs go nowhere. The arguments are still evaluated, as on the device.
static inline void fuzz_log_sink(const char *tag, ...) { (void)tag; }
#define ESP_LOGE(tag, ...) fuzz_log_sink(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fuzz_log_sink(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fuzz_log_sink(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fuzz_log_sink(tag, __VA_ARGS__)
