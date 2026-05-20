/*
 * test_hooks.c — Tests for the pipeline hook framework.
 */
#include "test_framework.h"
#include "pipeline/pipeline_hooks.h"

static int dummy_hook(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                      int file_count, CBMFileResult **cache) {
    (void)ctx; (void)files; (void)file_count; (void)cache;
    return 0;
}

static int failing_hook(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                        int file_count, CBMFileResult **cache) {
    (void)ctx; (void)files; (void)file_count; (void)cache;
    return -1;
}

TEST(hooks_register_and_count) {
    cbm_pipeline_hooks_reset();
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_AFTER_LSP_CROSS), 0);
    int rc = cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "test1");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_AFTER_LSP_CROSS), 1);
    rc = cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "test2");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_AFTER_LSP_CROSS), 2);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_POST_RESOLVE), 0);
    cbm_pipeline_hooks_reset();
    PASS();
}

TEST(hooks_run_empty) {
    cbm_pipeline_hooks_reset();
    int rc = cbm_pipeline_hooks_run(CBM_HOOK_AFTER_LSP_CROSS, NULL, NULL, 0, NULL);
    ASSERT_EQ(rc, 0);
    cbm_pipeline_hooks_reset();
    PASS();
}

TEST(hooks_run_calls_fn) {
    cbm_pipeline_hooks_reset();
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "ok");
    int rc = cbm_pipeline_hooks_run(CBM_HOOK_AFTER_LSP_CROSS, NULL, NULL, 0, NULL);
    ASSERT_EQ(rc, 0);
    cbm_pipeline_hooks_reset();
    PASS();
}

TEST(hooks_run_returns_first_error) {
    cbm_pipeline_hooks_reset();
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, failing_hook, "fail");
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "ok");
    int rc = cbm_pipeline_hooks_run(CBM_HOOK_AFTER_LSP_CROSS, NULL, NULL, 0, NULL);
    ASSERT_EQ(rc, -1);
    cbm_pipeline_hooks_reset();
    PASS();
}

TEST(hooks_phase_isolation) {
    cbm_pipeline_hooks_reset();
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "a");
    cbm_pipeline_hook_register(CBM_HOOK_POST_RESOLVE, dummy_hook, "b");
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_AFTER_LSP_CROSS), 1);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_POST_RESOLVE), 1);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_POST_EXTRACTION), 0);
    cbm_pipeline_hooks_reset();
    PASS();
}

TEST(hooks_reset_clears_all) {
    cbm_pipeline_hooks_reset();
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, dummy_hook, "x");
    cbm_pipeline_hook_register(CBM_HOOK_POST_RESOLVE, dummy_hook, "y");
    cbm_pipeline_hooks_reset();
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_AFTER_LSP_CROSS), 0);
    ASSERT_EQ(cbm_pipeline_hooks_count(CBM_HOOK_POST_RESOLVE), 0);
    PASS();
}

SUITE(hooks) {
    RUN_TEST(hooks_register_and_count);
    RUN_TEST(hooks_run_empty);
    RUN_TEST(hooks_run_calls_fn);
    RUN_TEST(hooks_run_returns_first_error);
    RUN_TEST(hooks_phase_isolation);
    RUN_TEST(hooks_reset_clears_all);
}
