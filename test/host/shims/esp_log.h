#ifndef SHIM_ESP_LOG_H
#define SHIM_ESP_LOG_H
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOG_BUFFER_HEX(tag, buf, len) do { (void)(buf); (void)(len); } while (0)
#endif
