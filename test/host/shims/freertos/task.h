#ifndef SHIM_TASK_H
#define SHIM_TASK_H
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
/* The host tests exercise the synchronous path only, so task creation is a
 * stub that reports failure and the current handle is never a live task. */
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                                 uint32_t stack, void *arg,
                                                 UBaseType_t prio, TaskHandle_t *out,
                                                 BaseType_t core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)out; (void)core;
    return pdFAIL;
}
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)0; }
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
#endif
