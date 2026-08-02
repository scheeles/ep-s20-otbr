/* pthread-backed stand-ins for the FreeRTOS queue and task APIs.
 *
 * These are deliberately real implementations rather than no-ops: the worker
 * task actually runs on its own thread and the queue actually has a bounded
 * depth, so the tests exercise the hand-off between the log hook and the worker
 * — which is the part that went wrong on the device — instead of a simulation
 * of it.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

unsigned rtos_stub_dropped;

struct rtos_stub_queue {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    uint32_t length;
    uint32_t item_size;
    uint32_t count;
    uint32_t head;
    uint32_t tail;
    unsigned char *storage;
};

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size)
{
    struct rtos_stub_queue *queue = calloc(1, sizeof(*queue));

    if (queue == NULL) {
        return NULL;
    }
    queue->storage = calloc(length, item_size);
    if (queue->storage == NULL) {
        free(queue);
        return NULL;
    }
    queue->length = length;
    queue->item_size = item_size;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    return queue;
}

void vQueueDelete(QueueHandle_t queue)
{
    if (queue == NULL) {
        return;
    }
    free(queue->storage);
    free(queue);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    BaseType_t ret = pdTRUE;

    /* net_syslog only ever enqueues with a zero timeout; a blocking send would
     * defeat the purpose, so this stub refuses to implement one. */
    (void)ticks_to_wait;

    pthread_mutex_lock(&queue->lock);
    if (queue->count == queue->length) {
        rtos_stub_dropped++;
        ret = pdFALSE;
    } else {
        memcpy(queue->storage + (size_t)queue->tail * queue->item_size, item, queue->item_size);
        queue->tail = (queue->tail + 1) % queue->length;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
    }
    pthread_mutex_unlock(&queue->lock);
    return ret;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *buffer, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait; /* net_syslog's worker always waits forever. */

    pthread_mutex_lock(&queue->lock);
    while (queue->count == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }
    memcpy(buffer, queue->storage + (size_t)queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    queue->count--;
    pthread_mutex_unlock(&queue->lock);
    return pdTRUE;
}

struct task_start {
    TaskFunction_t func;
    void *param;
};

static void *task_trampoline(void *arg)
{
    struct task_start *start = arg;
    TaskFunction_t func = start->func;
    void *param = start->param;

    free(start);
    func(param);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t func, const char *name, uint32_t stack_depth, void *param, uint32_t priority,
                       TaskHandle_t *created)
{
    struct task_start *start;
    pthread_t thread;

    (void)name;
    (void)stack_depth;
    (void)priority;

    start = malloc(sizeof(*start));
    if (start == NULL) {
        return pdFALSE;
    }
    start->func = func;
    start->param = param;

    if (pthread_create(&thread, NULL, task_trampoline, start) != 0) {
        free(start);
        return pdFALSE;
    }
    pthread_detach(thread);
    if (created != NULL) {
        *created = (TaskHandle_t)thread;
    }
    return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return (TaskHandle_t)pthread_self();
}

int xPortInIsrContext(void)
{
    return 0;
}

struct rtos_stub_mutex {
    pthread_mutex_t lock;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct rtos_stub_mutex *mutex = calloc(1, sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }
    pthread_mutex_init(&mutex->lock, NULL);
    return mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait; /* net_syslog always waits forever. */
    pthread_mutex_lock(&mutex->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    pthread_mutex_unlock(&mutex->lock);
    return pdTRUE;
}
