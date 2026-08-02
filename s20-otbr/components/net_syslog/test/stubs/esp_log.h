/* Host-test stub: just enough esp_log.h to compile net_syslog.c off-target. */
#pragma once

#include <stdarg.h>
#include <stdio.h>

typedef int (*vprintf_like_t)(const char *, va_list);

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);

/* The component's own ESP_LOGx calls are noise in a test run. Dropping them
 * also keeps them out of the log hook, which would otherwise recurse. */
#define ESP_LOGW(tag, ...) do { } while (0)
#define ESP_LOGI(tag, ...) do { } while (0)
