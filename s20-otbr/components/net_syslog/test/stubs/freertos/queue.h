/* Host-test stub: FreeRTOS queue API, backed by a pthread ring in rtos_stub.c.
 * Real copy-by-value semantics and a real bounded depth, so drop-on-full is
 * exercised rather than assumed. */
#pragma once

#include "FreeRTOS.h"

typedef struct rtos_stub_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait);

/* Test-visible counter of lines the queue refused because it was full. */
extern unsigned rtos_stub_dropped;
