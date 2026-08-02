/* Host-test stub: FreeRTOS mutex API, backed by pthreads in rtos_stub.c. */
#pragma once

#include "FreeRTOS.h"

typedef struct rtos_stub_mutex *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
