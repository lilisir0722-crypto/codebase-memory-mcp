/*
 * test_bidir.c — Tests for bidirectional subprocess + cbm_which.
 */
#include "test_framework.h"
#include "extensions/ext_bidir.h"
#include "pipeline/pipeline_hooks.h"

TEST(bidir_echo_cat) {
    const char *argv[] = {"cat", NULL};
    cbm_bidir_t *b = cbm_bidir_spawn(argv, CBM_BIDIR_STDERR_DEVNULL);
    ASSERT_NOT_NULL(b);

    const char *msg = "ping\n";
    int wr = cbm_bidir_write(b, msg, 5);
    ASSERT_EQ(wr, 5);

    int exit_code = cbm_bidir_shutdown(b);
    ASSERT_EQ(exit_code, 0);

    char buf[64] = {0};
    int rd = cbm_bidir_read(b, buf, sizeof(buf) - 1);
    ASSERT_GT(rd, 0);
    ASSERT_STR_EQ(buf, "ping\n");

    cbm_bidir_free(b);
    PASS();
}

TEST(bidir_read_timeout) {
    const char *argv[] = {"sleep", "10", NULL};
    cbm_bidir_t *b = cbm_bidir_spawn(argv, CBM_BIDIR_STDERR_DEVNULL);
    ASSERT_NOT_NULL(b);

    char buf[16];
    int rd = cbm_bidir_read_timeout(b, buf, sizeof(buf), 100);
    ASSERT_EQ(rd, 0);

    cbm_bidir_free(b);
    PASS();
}

TEST(bidir_which_ls) {
    char *p = cbm_which("ls");
    ASSERT_NOT_NULL(p);
    ASSERT(access(p, X_OK) == 0);
    free(p);
    PASS();
}

TEST(bidir_which_notfound) {
    char *p = cbm_which("xyz_nonexistent_bin_12345");
    ASSERT_NULL(p);
    PASS();
}

TEST(bidir_spawn_null) {
    cbm_bidir_t *b = cbm_bidir_spawn(NULL, 0);
    ASSERT_NULL(b);
    PASS();
}

TEST(bidir_pid) {
    const char *argv[] = {"cat", NULL};
    cbm_bidir_t *b = cbm_bidir_spawn(argv, CBM_BIDIR_STDERR_DEVNULL);
    ASSERT_NOT_NULL(b);
    long pid = cbm_bidir_pid(b);
    ASSERT(pid > 0);
    cbm_bidir_free(b);
    PASS();
}

SUITE(bidir) {
    RUN_TEST(bidir_echo_cat);
    RUN_TEST(bidir_read_timeout);
    RUN_TEST(bidir_which_ls);
    RUN_TEST(bidir_which_notfound);
    RUN_TEST(bidir_spawn_null);
    RUN_TEST(bidir_pid);
}

static void __attribute__((constructor)) register_bidir_tests(void) {
    cbm_ext_test_register(suite_bidir, "bidir");
}
