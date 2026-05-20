/*
 * pipeline_hooks.h — Self-registering extension hook framework.
 *
 * Extensions call cbm_pipeline_hook_register() from an
 * __attribute__((constructor)) to inject passes into the pipeline
 * at declared phases. Upstream pipeline.c only needs stable
 * cbm_pipeline_hooks_run() calls at insertion points.
 */
#ifndef CBM_PIPELINE_HOOKS_H
#define CBM_PIPELINE_HOOKS_H

#include "pipeline/pipeline_internal.h"

/* ── Hook phases ─────────────────────────────────────────────────── */

typedef enum {
    CBM_HOOK_AFTER_LSP_CROSS = 0,
    CBM_HOOK_POST_RESOLVE    = 1,
    CBM_HOOK_POST_EXTRACTION = 2,
    CBM_HOOK_PHASE_COUNT     = 3,
} cbm_hook_phase_t;

/* ── Hook function type ──────────────────────────────────────────── */

typedef int (*cbm_pipeline_hook_fn)(cbm_pipeline_ctx_t *ctx,
                                    const cbm_file_info_t *files,
                                    int file_count,
                                    CBMFileResult **cache);

/* ── Registration ────────────────────────────────────────────────── */

#define CBM_MAX_HOOKS_PER_PHASE 16

int cbm_pipeline_hook_register(cbm_hook_phase_t phase, cbm_pipeline_hook_fn fn,
                               const char *name);

/* ── Execution (called from pipeline.c) ──────────────────────────── */

int cbm_pipeline_hooks_run(cbm_hook_phase_t phase, cbm_pipeline_ctx_t *ctx,
                           const cbm_file_info_t *files, int file_count,
                           CBMFileResult **cache);

/* ── Diagnostics / testing ───────────────────────────────────────── */

int cbm_pipeline_hooks_count(cbm_hook_phase_t phase);
void cbm_pipeline_hooks_reset(void);

/* ── Extension test suite auto-registration ──────────────────────── */

typedef void (*cbm_test_suite_fn)(void);

#define CBM_MAX_EXT_TEST_SUITES 16

int cbm_ext_test_register(cbm_test_suite_fn fn, const char *name);
void cbm_ext_tests_run(void);
int cbm_ext_tests_count(void);

#endif /* CBM_PIPELINE_HOOKS_H */
