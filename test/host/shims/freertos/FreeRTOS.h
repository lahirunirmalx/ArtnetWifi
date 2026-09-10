#ifndef SHIM_FREERTOS_H
#define SHIM_FREERTOS_H
#include <stdint.h>
typedef int           BaseType_t;
typedef unsigned int  UBaseType_t;
typedef uint32_t      TickType_t;
#define pdPASS         1
#define pdFAIL         0
#define pdTRUE         1
#define pdFALSE        0
#define tskNO_AFFINITY 0x7FFFFFFF
#define portMAX_DELAY  0xFFFFFFFFU
#define pdMS_TO_TICKS(ms) (ms)
#endif
