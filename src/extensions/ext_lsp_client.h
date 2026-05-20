/*
 * ext_lsp_client.h — JSON-RPC 2.0 LSP client over stdio.
 */
#ifndef CBM_EXT_LSP_CLIENT_H
#define CBM_EXT_LSP_CLIENT_H

#include "extensions/ext_bidir.h"

typedef struct {
    cbm_bidir_t *proc;
    int next_id;
} cbm_lsp_client_t;

typedef struct {
    const char *root_uri;
    const char *clangd_path;
    int timeout_ms;
} cbm_lsp_client_opts_t;

/* Lifecycle */
cbm_lsp_client_t *cbm_lsp_client_new(cbm_lsp_client_opts_t *opts);
int cbm_lsp_client_shutdown(cbm_lsp_client_t *c);
void cbm_lsp_client_free(cbm_lsp_client_t *c);

/* Low-level */
char *cbm_lsp_frame(const char *json);
int cbm_lsp_parse_content_length(const char *header, size_t len);
int cbm_lsp_read_response(cbm_bidir_t *proc, char **json_out, int timeout_ms);
int cbm_lsp_client_request(cbm_lsp_client_t *c, const char *method,
                            const char *params_json, char **response, int timeout_ms);
int cbm_lsp_client_notify(cbm_lsp_client_t *c, const char *method,
                           const char *params_json);

/* High-level LSP methods */
int cbm_lsp_client_did_open(cbm_lsp_client_t *c, const char *uri,
                             const char *language_id, int version, const char *text);
int cbm_lsp_client_did_close(cbm_lsp_client_t *c, const char *uri);
int cbm_lsp_client_document_symbol(cbm_lsp_client_t *c, const char *uri,
                                    char **response_json, int timeout_ms);
int cbm_lsp_client_prepare_call_hierarchy(cbm_lsp_client_t *c,
                                           const char *uri, int line, int character,
                                           char **response_json, int timeout_ms);
int cbm_lsp_client_outgoing_calls(cbm_lsp_client_t *c, const char *item_json,
                                   char **response_json, int timeout_ms);

/* Check if clangd process is still alive */
int cbm_lsp_client_is_alive(cbm_lsp_client_t *c);

#endif /* CBM_EXT_LSP_CLIENT_H */
