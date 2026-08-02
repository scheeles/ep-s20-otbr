/* Host-test stub: FreeRTOS task API, backed by pthreads in rtos_stub.c. */
#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(TaskFunction_t func, const char *name, uint32_t stack_depth, void *param, uint32_t priority,
                       TaskHandle_t *created);
int xPortInIsrContext(void);
