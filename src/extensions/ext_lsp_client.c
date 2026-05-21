/*
 * ext_lsp_client.c — JSON-RPC 2.0 LSP client over stdio pipes.
 */
#include "extensions/ext_lsp_client.h"
#include "foundation/log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <yyjson/yyjson.h>

/* ── Content-Length framing ───────────────────────────────────────── */

char *cbm_lsp_frame(const char *json) {
    if (!json) return NULL;
    size_t jlen = strlen(json);
    int hdr_len = snprintf(NULL, 0, "Content-Length: %zu\r\n\r\n", jlen);
    char *buf = malloc((size_t)hdr_len + jlen + 1);
    if (!buf) return NULL;
    snprintf(buf, (size_t)hdr_len + 1, "Content-Length: %zu\r\n\r\n", jlen);
    memcpy(buf + hdr_len, json, jlen + 1);
    return buf;
}

int cbm_lsp_parse_content_length(const char *header, size_t len) {
    if (!header || len < 16) return -1;
    const char *prefix = "Content-Length: ";
    size_t plen = 16;
    if (strncasecmp(header, prefix, plen) != 0) return -1;
    int val = atoi(header + plen);
    return val > 0 ? val : -1;
}

/* ── Response reading ────────────────────────────────────────────── */

int cbm_lsp_read_response(cbm_bidir_t *proc, char **json_out, int timeout_ms) {
    if (!proc || !json_out) return -1;
    *json_out = NULL;

    char hdr_buf[256];
    int hdr_pos = 0;
    int content_length = -1;

    /* Read headers byte-by-byte until \r\n\r\n */
    while (hdr_pos < (int)sizeof(hdr_buf) - 1) {
        int r = cbm_bidir_read_timeout(proc, &hdr_buf[hdr_pos], 1, timeout_ms);
        if (r <= 0) return -1;
        hdr_pos++;
        if (hdr_pos >= 4 &&
            hdr_buf[hdr_pos - 4] == '\r' && hdr_buf[hdr_pos - 3] == '\n' &&
            hdr_buf[hdr_pos - 2] == '\r' && hdr_buf[hdr_pos - 1] == '\n') {
            hdr_buf[hdr_pos] = '\0';
            break;
        }
    }

    /* Parse Content-Length from headers (non-destructive: use length-delimited scan) */
    char *line = hdr_buf;
    while (line && line < hdr_buf + hdr_pos) {
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        size_t line_len = (size_t)(eol - line);
        if (content_length < 0 && line_len >= 16) {
            /* Temporarily null-terminate for parsing */
            char saved = *eol;
            *eol = '\0';
            content_length = cbm_lsp_parse_content_length(line, line_len);
            *eol = saved;
        }
        line = eol + 2;
    }

    if (content_length <= 0 || content_length > 10 * 1024 * 1024) return -1;

    /* Read body */
    char *body = malloc((size_t)content_length + 1);
    if (!body) return -1;

    int total = 0;
    while (total < content_length) {
        int remaining = content_length - total;
        int r = cbm_bidir_read_timeout(proc, body + total, (size_t)remaining, timeout_ms);
        if (r <= 0) {
            free(body);
            return -1;
        }
        total += r;
    }
    body[content_length] = '\0';
    *json_out = body;
    return 0;
}

/* ── JSON-RPC helpers ────────────────────────────────────────────── */

static char *build_request(int id, const char *method, const char *params_json) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_int(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "method", method);

    if (params_json && params_json[0]) {
        yyjson_doc *params_doc = yyjson_read(params_json, strlen(params_json), 0);
        if (params_doc) {
            yyjson_val *pval = yyjson_doc_get_root(params_doc);
            yyjson_mut_val *mparams = yyjson_val_mut_copy(doc, pval);
            yyjson_mut_obj_add_val(doc, root, "params", mparams);
            yyjson_doc_free(params_doc);
        }
    } else {
        yyjson_mut_obj_add_obj(doc, root, "params");
    }

    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *build_notification(const char *method, const char *params_json) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_str(doc, root, "method", method);

    if (params_json && params_json[0]) {
        yyjson_doc *params_doc = yyjson_read(params_json, strlen(params_json), 0);
        if (params_doc) {
            yyjson_val *pval = yyjson_doc_get_root(params_doc);
            yyjson_mut_val *mparams = yyjson_val_mut_copy(doc, pval);
            yyjson_mut_obj_add_val(doc, root, "params", mparams);
            yyjson_doc_free(params_doc);
        }
    } else {
        yyjson_mut_obj_add_obj(doc, root, "params");
    }

    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

/* ── Core request/notify ─────────────────────────────────────────── */

int cbm_lsp_client_request(cbm_lsp_client_t *c, const char *method,
                            const char *params_json, char **response, int timeout_ms) {
    if (!c || !method || !response) return -1;
    *response = NULL;

    int id = c->next_id++;
    char *json = build_request(id, method, params_json);
    if (!json) return -1;

    char *frame = cbm_lsp_frame(json);
    free(json);
    if (!frame) return -1;

    int wr = cbm_bidir_write(c->proc, frame, strlen(frame));
    free(frame);
    if (wr < 0) return -1;

    /* Read responses, skip notifications (method field present, no id) */
    for (int attempts = 0; attempts < 100; attempts++) {
        char *resp_json = NULL;
        if (cbm_lsp_read_response(c->proc, &resp_json, timeout_ms) != 0) return -1;

        yyjson_doc *rdoc = yyjson_read(resp_json, strlen(resp_json), 0);
        if (!rdoc) {
            free(resp_json);
            return -1;
        }
        yyjson_val *rroot = yyjson_doc_get_root(rdoc);
        yyjson_val *rid = yyjson_obj_get(rroot, "id");

        if (rid && yyjson_get_int(rid) == id) {
            /* Check for LSP error response */
            yyjson_val *err = yyjson_obj_get(rroot, "error");
            if (err && yyjson_is_obj(err)) {
                yyjson_val *emsg = yyjson_obj_get(err, "message");
                const char *estr = emsg ? yyjson_get_str(emsg) : "unknown";
                cbm_log_warn("lsp.request.error", "method", method, "error", estr ? estr : "?");
                yyjson_doc_free(rdoc);
                free(resp_json);
                *response = NULL;
                return -1;
            }
            yyjson_doc_free(rdoc);
            *response = resp_json;
            return 0;
        }
        /* Not our response (notification or different id) — discard and retry */
        yyjson_doc_free(rdoc);
        free(resp_json);
    }
    return -1;
}

int cbm_lsp_client_notify(cbm_lsp_client_t *c, const char *method,
                           const char *params_json) {
    if (!c || !method) return -1;

    char *json = build_notification(method, params_json);
    if (!json) return -1;

    char *frame = cbm_lsp_frame(json);
    free(json);
    if (!frame) return -1;

    int wr = cbm_bidir_write(c->proc, frame, strlen(frame));
    free(frame);
    return wr < 0 ? -1 : 0;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

cbm_lsp_client_t *cbm_lsp_client_new(cbm_lsp_client_opts_t *opts) {
    if (!opts || !opts->clangd_path || !opts->root_uri) return NULL;

    const char *argv[] = {
        opts->clangd_path,
        "--header-insertion=never",
        "--background-index=true",
        NULL
    };

    cbm_bidir_t *proc = cbm_bidir_spawn(argv, CBM_BIDIR_STDERR_LOG);
    if (!proc) return NULL;

    cbm_lsp_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        cbm_bidir_free(proc);
        return NULL;
    }
    c->proc = proc;
    c->next_id = 1;

    int timeout = opts->timeout_ms > 0 ? opts->timeout_ms : 30000;

    /* Build initialize params */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_int(doc, root, "processId", (int64_t)getpid());
    yyjson_mut_obj_add_str(doc, root, "rootUri", opts->root_uri);

    yyjson_mut_val *caps = yyjson_mut_obj_add_obj(doc, root, "capabilities");
    yyjson_mut_val *text_doc = yyjson_mut_obj_add_obj(doc, caps, "textDocument");
    yyjson_mut_obj_add_obj(doc, text_doc, "callHierarchy");
    yyjson_mut_obj_add_obj(doc, text_doc, "documentSymbol");

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    char *init_resp = NULL;
    int rc = cbm_lsp_client_request(c, "initialize", params, &init_resp, timeout);
    free(params);

    if (rc != 0) {
        cbm_log_error("lsp.client", "error", "initialize failed");
        free(init_resp);
        cbm_bidir_free(proc);
        free(c);
        return NULL;
    }
    free(init_resp);

    /* Send initialized notification */
    cbm_lsp_client_notify(c, "initialized", "{}");
    return c;
}

int cbm_lsp_client_shutdown(cbm_lsp_client_t *c) {
    if (!c) return -1;

    char *resp = NULL;
    int rc = cbm_lsp_client_request(c, "shutdown", NULL, &resp, 5000);
    free(resp);

    cbm_lsp_client_notify(c, "exit", NULL);
    int exit_code = cbm_bidir_shutdown(c->proc);
    (void)rc;
    return exit_code;
}

void cbm_lsp_client_free(cbm_lsp_client_t *c) {
    if (!c) return;
    cbm_bidir_free(c->proc);
    free(c);
}

int cbm_lsp_client_is_alive(cbm_lsp_client_t *c) {
    if (!c || !c->proc) return 0;
    pid_t pid = (pid_t)cbm_bidir_pid(c->proc);
    return (kill(pid, 0) == 0) ? 1 : 0;
}

/* ── High-level LSP methods ──────────────────────────────────────── */

int cbm_lsp_client_did_open(cbm_lsp_client_t *c, const char *uri,
                             const char *language_id, int version, const char *text) {
    if (!c || !uri || !language_id || !text) return -1;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *td = yyjson_mut_obj_add_obj(doc, root, "textDocument");
    yyjson_mut_obj_add_str(doc, td, "uri", uri);
    yyjson_mut_obj_add_str(doc, td, "languageId", language_id);
    yyjson_mut_obj_add_int(doc, td, "version", version);
    yyjson_mut_obj_add_str(doc, td, "text", text);

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    int rc = cbm_lsp_client_notify(c, "textDocument/didOpen", params);
    free(params);
    return rc;
}

int cbm_lsp_client_did_close(cbm_lsp_client_t *c, const char *uri) {
    if (!c || !uri) return -1;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *td = yyjson_mut_obj_add_obj(doc, root, "textDocument");
    yyjson_mut_obj_add_str(doc, td, "uri", uri);

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    int rc = cbm_lsp_client_notify(c, "textDocument/didClose", params);
    free(params);
    return rc;
}

int cbm_lsp_client_document_symbol(cbm_lsp_client_t *c, const char *uri,
                                    char **response_json, int timeout_ms) {
    if (!c || !uri || !response_json) return -1;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *td = yyjson_mut_obj_add_obj(doc, root, "textDocument");
    yyjson_mut_obj_add_str(doc, td, "uri", uri);

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    int rc = cbm_lsp_client_request(c, "textDocument/documentSymbol", params,
                                     response_json, timeout_ms);
    free(params);
    return rc;
}

int cbm_lsp_client_prepare_call_hierarchy(cbm_lsp_client_t *c,
                                           const char *uri, int line, int character,
                                           char **response_json, int timeout_ms) {
    if (!c || !uri || !response_json) return -1;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val *td = yyjson_mut_obj_add_obj(doc, root, "textDocument");
    yyjson_mut_obj_add_str(doc, td, "uri", uri);

    yyjson_mut_val *pos = yyjson_mut_obj_add_obj(doc, root, "position");
    yyjson_mut_obj_add_int(doc, pos, "line", line);
    yyjson_mut_obj_add_int(doc, pos, "character", character);

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    int rc = cbm_lsp_client_request(c, "textDocument/prepareCallHierarchy", params,
                                     response_json, timeout_ms);
    free(params);
    return rc;
}

int cbm_lsp_client_outgoing_calls(cbm_lsp_client_t *c, const char *item_json,
                                   char **response_json, int timeout_ms) {
    if (!c || !item_json || !response_json) return -1;

    /* params = {"item": <item_json>} */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_doc *item_doc = yyjson_read(item_json, strlen(item_json), 0);
    if (!item_doc) {
        yyjson_mut_doc_free(doc);
        return -1;
    }
    yyjson_val *item_val = yyjson_doc_get_root(item_doc);
    yyjson_mut_val *mitem = yyjson_val_mut_copy(doc, item_val);
    yyjson_mut_obj_add_val(doc, root, "item", mitem);
    yyjson_doc_free(item_doc);

    char *params = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    int rc = cbm_lsp_client_request(c, "callHierarchy/outgoingCalls", params,
                                     response_json, timeout_ms);
    free(params);
    return rc;
}
