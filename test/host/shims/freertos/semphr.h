#ifndef SHIM_SEMPHR_H
#define SHIM_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
#endif
