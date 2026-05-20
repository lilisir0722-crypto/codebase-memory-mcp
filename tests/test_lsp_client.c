/*
 * test_lsp_client.c — Unit tests for LSP client framing.
 */
#include "test_framework.h"
#include "extensions/ext_lsp_client.h"
#include "pipeline/pipeline_hooks.h"

#include <string.h>
#include <stdlib.h>

TEST(lsp_frame_format) {
    char *f = cbm_lsp_frame("{\"jsonrpc\":\"2.0\"}");
    ASSERT_NOT_NULL(f);
    /* {"jsonrpc":"2.0"} is 17 bytes */
    ASSERT(strncmp(f, "Content-Length: 17\r\n\r\n", 22) == 0);
    ASSERT(strcmp(f + 22, "{\"jsonrpc\":\"2.0\"}") == 0);
    free(f);
    PASS();
}

TEST(lsp_frame_empty) {
    char *f = cbm_lsp_frame("{}");
    ASSERT_NOT_NULL(f);
    /* {} is 2 bytes, header "Content-Length: 2\r\n\r\n" is 21 chars */
    ASSERT(strncmp(f, "Content-Length: 2\r\n\r\n", 21) == 0);
    ASSERT(strcmp(f + 21, "{}") == 0);
    free(f);
    PASS();
}

TEST(lsp_frame_null) {
    char *f = cbm_lsp_frame(NULL);
    ASSERT_NULL(f);
    PASS();
}

TEST(lsp_parse_content_length_valid) {
    const char *hdr = "Content-Length: 42";
    int val = cbm_lsp_parse_content_length(hdr, strlen(hdr));
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(lsp_parse_content_length_large) {
    const char *hdr = "Content-Length: 12345";
    int val = cbm_lsp_parse_content_length(hdr, strlen(hdr));
    ASSERT_EQ(val, 12345);
    PASS();
}

TEST(lsp_parse_content_length_missing) {
    const char *hdr = "X-Custom: foo";
    int val = cbm_lsp_parse_content_length(hdr, strlen(hdr));
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(lsp_parse_content_length_zero) {
    const char *hdr = "Content-Length: 0";
    int val = cbm_lsp_parse_content_length(hdr, strlen(hdr));
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(lsp_parse_content_length_short) {
    const char *hdr = "short";
    int val = cbm_lsp_parse_content_length(hdr, strlen(hdr));
    ASSERT_EQ(val, -1);
    PASS();
}

SUITE(lsp_client) {
    RUN_TEST(lsp_frame_format);
    RUN_TEST(lsp_frame_empty);
    RUN_TEST(lsp_frame_null);
    RUN_TEST(lsp_parse_content_length_valid);
    RUN_TEST(lsp_parse_content_length_large);
    RUN_TEST(lsp_parse_content_length_missing);
    RUN_TEST(lsp_parse_content_length_zero);
    RUN_TEST(lsp_parse_content_length_short);
}

static void __attribute__((constructor)) register_lsp_client_tests(void) {
    cbm_ext_test_register(suite_lsp_client, "lsp_client");
}
