/*
 * pipeline_hooks.c — Self-registering extension hook framework.
 */
#include "pipeline/pipeline_hooks.h"
#include "foundation/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── Static hook table ───────────────────────────────────────────── */

typedef struct {
    cbm_pipeline_hook_fn fn;
    const char *name;
} cbm_hook_entry_t;

static struct {
    cbm_hook_entry_t hooks[CBM_MAX_HOOKS_PER_PHASE];
    int count;
} g_hook_table[CBM_HOOK_PHASE_COUNT];

static const char *phase_name(cbm_hook_phase_t phase) {
    switch (phase) {
    case CBM_HOOK_AFTER_LSP_CROSS: return "AFTER_LSP_CROSS";
    case CBM_HOOK_POST_RESOLVE:    return "POST_RESOLVE";
    case CBM_HOOK_POST_EXTRACTION: return "POST_EXTRACTION";
    default:                       return "UNKNOWN";
    }
}

/* ── Registration ────────────────────────────────────────────────── */

int cbm_pipeline_hook_register(cbm_hook_phase_t phase, cbm_pipeline_hook_fn fn,
                               const char *name) {
    if ((int)phase < 0 || phase >= CBM_HOOK_PHASE_COUNT) {
        cbm_log_error("hooks.register", "error", "invalid phase", "name", name ? name : "?");
        return -1;
    }
    if (g_hook_table[phase].count >= CBM_MAX_HOOKS_PER_PHASE) {
        cbm_log_error("hooks.register", "error", "table full",
                      "phase", phase_name(phase), "name", name ? name : "?");
        return -1;
    }
    cbm_hook_entry_t *e = &g_hook_table[phase].hooks[g_hook_table[phase].count++];
    e->fn = fn;
    e->name = name;
    cbm_log_info("hooks.register", "phase", phase_name(phase), "name", name ? name : "?");
    return 0;
}

/* ── Execution ───────────────────────────────────────────────────── */

int cbm_pipeline_hooks_run(cbm_hook_phase_t phase, cbm_pipeline_ctx_t *ctx,
                           const cbm_file_info_t *files, int file_count,
                           CBMFileResult **cache) {
    if ((int)phase < 0 || phase >= CBM_HOOK_PHASE_COUNT) return 0;
    if (g_hook_table[phase].count == 0) return 0;

    int first_err = 0;
    for (int i = 0; i < g_hook_table[phase].count; i++) {
        const cbm_hook_entry_t *e = &g_hook_table[phase].hooks[i];
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int rc = e->fn(ctx, files, file_count, cache);

        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        char ms_buf[32];
        snprintf(ms_buf, sizeof(ms_buf), "%d", (int)ms);
        cbm_log_info("pass.timing", "pass", e->name ? e->name : "hook",
                     "elapsed_ms", ms_buf);

        if (rc != 0) {
            cbm_log_error("hooks.run", "hook", e->name ? e->name : "?",
                          "phase", phase_name(phase));
            if (first_err == 0) first_err = rc;
        }
    }
    return first_err;
}

/* ── Diagnostics ─────────────────────────────────────────────────── */

int cbm_pipeline_hooks_count(cbm_hook_phase_t phase) {
    if ((int)phase < 0 || phase >= CBM_HOOK_PHASE_COUNT) return 0;
    return g_hook_table[phase].count;
}

void cbm_pipeline_hooks_reset(void) {
    memset(g_hook_table, 0, sizeof(g_hook_table));
}

/* ── Extension test registry ─────────────────────────────────────── */

static struct {
    cbm_test_suite_fn fn;
    const char *name;
} g_ext_tests[CBM_MAX_EXT_TEST_SUITES];
static int g_ext_test_count;

int cbm_ext_test_register(cbm_test_suite_fn fn, const char *name) {
    if (g_ext_test_count >= CBM_MAX_EXT_TEST_SUITES) return -1;
    g_ext_tests[g_ext_test_count].fn = fn;
    g_ext_tests[g_ext_test_count].name = name;
    g_ext_test_count++;
    return 0;
}

void cbm_ext_tests_run(void) {
    for (int i = 0; i < g_ext_test_count; i++) {
        printf("\n\033[90m=== %s ===\033[0m\n", g_ext_tests[i].name);
        g_ext_tests[i].fn();
    }
}

int cbm_ext_tests_count(void) {
    return g_ext_test_count;
}
