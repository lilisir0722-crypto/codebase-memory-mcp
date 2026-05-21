/*
 * log.c — Structured key-value logging to stderr.
 */
#include "log.h"
#include "foundation/constants.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static CBMLogLevel g_log_level = CBM_LOG_INFO;
static cbm_log_sink_fn g_log_sink = NULL;

void cbm_log_set_sink(cbm_log_sink_fn fn) {
    g_log_sink = fn;
}

void cbm_log_set_level(CBMLogLevel level) {
    g_log_level = level;
}

CBMLogLevel cbm_log_get_level(void) {
    return g_log_level;
}

static const char *level_str(CBMLogLevel level) {
    switch (level) {
    case CBM_LOG_DEBUG:
        return "debug";
    case CBM_LOG_INFO:
        return "info";
    case CBM_LOG_WARN:
        return "warn";
    case CBM_LOG_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

void cbm_log(CBMLogLevel level, const char *msg, ...) {
    if (level < g_log_level) {
        return;
    }

    /* Timestamp prefix: 2026-05-21T06:42:23 */
    char time_buf[32];
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    /* Build the log line into a buffer ONCE — no double va_list iteration */
    char line_buf[CBM_SZ_512];
    int pos =
        snprintf(line_buf, sizeof(line_buf), "time=%s level=%s msg=%s", time_buf, level_str(level), msg ? msg : "");

    va_list args;
    va_start(args, msg);
    for (;;) {
        const char *key = va_arg(args, const char *);
        if (!key) {
            break;
        }
        const char *val = va_arg(args, const char *);
        if (!val) {
            val = "";
        }
        if ((size_t)pos < sizeof(line_buf) - SKIP_ONE) {
            pos += snprintf(line_buf + pos, sizeof(line_buf) - (size_t)pos, " %s=%s", key, val);
        }
    }
    va_end(args);

    /* When a sink is registered it takes over all output (exclusive).
     * Otherwise write structured log to stderr. */
    if (g_log_sink) {
        g_log_sink(line_buf);
    } else {
        (void)fprintf(stderr, "%s\n", line_buf);
    }
}

void cbm_log_int(CBMLogLevel level, const char *msg, const char *key, int64_t value) {
    if (level < g_log_level) {
        return;
    }

    char time_buf[32];
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    char line_buf[CBM_SZ_256];
    snprintf(line_buf, sizeof(line_buf), "time=%s level=%s msg=%s %s=%" PRId64, time_buf, level_str(level),
             msg ? msg : "", key ? key : "?", value);

    if (g_log_sink) {
        g_log_sink(line_buf);
    } else {
        (void)fprintf(stderr, "%s\n", line_buf);
    }
}
