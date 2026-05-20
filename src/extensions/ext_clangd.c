/*
 * ext_clangd.c — Clangd LSP semantic enhancement pass.
 *
 * Self-registers into the pipeline hook framework at AFTER_LSP_CROSS phase.
 * When clangd + compile_commands.json are available, augments CALLS edges
 * via callHierarchy and discovers GflagDef nodes via documentSymbol.
 */
#include "extensions/ext_lsp_client.h"
#include "pipeline/pipeline_hooks.h"
#include "pipeline/pipeline_internal.h"
#include "foundation/log.h"
#include "foundation/compat_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <yyjson/yyjson.h>

/* ── Configuration ───────────────────────────────────────────────── */

enum {
    CLANGD_TIMEOUT_MS     = 60000,
    CLANGD_CANCEL_INTERVAL = 10,
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static bool clangd_available(const char *repo_path) {
    if (getenv("CBM_NO_CLANGD")) return false;
    char cc_path[4096];
    snprintf(cc_path, sizeof(cc_path), "%s/compile_commands.json", repo_path);
    struct stat st;
    if (stat(cc_path, &st) != 0) return false;
    char *clangd = cbm_which("clangd");
    if (!clangd) return false;
    free(clangd);
    return true;
}

static char *build_file_uri(const char *repo_path, const char *rel_path) {
    size_t len = 7 + strlen(repo_path) + 1 + strlen(rel_path) + 1;
    char *uri = malloc(len);
    if (!uri) return NULL;
    snprintf(uri, len, "file://%s/%s", repo_path, rel_path);
    return uri;
}

static char *uri_to_rel_path(const char *uri, const char *repo_path) {
    if (!uri || !repo_path) return NULL;
    /* Expected: file:///abs/repo/path/rel/file.cpp */
    const char *prefix = "file://";
    if (strncmp(uri, prefix, 7) != 0) return NULL;
    const char *abs_path = uri + 7;
    size_t repo_len = strlen(repo_path);
    if (strncmp(abs_path, repo_path, repo_len) != 0) return NULL;
    if (abs_path[repo_len] != '/') return NULL;
    return strdup(abs_path + repo_len + 1);
}

static bool is_cpp_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return (strcmp(ext, ".c") == 0 || strcmp(ext, ".cc") == 0 ||
            strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cxx") == 0 ||
            strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0 ||
            strcmp(ext, ".hxx") == 0 || strcmp(ext, ".cu") == 0);
}

static const char *lang_id_for_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "cpp";
    if (strcmp(ext, ".c") == 0) return "c";
    if (strcmp(ext, ".cu") == 0) return "cuda";
    return "cpp";
}

/* ── GflagDef discovery ──────────────────────────────────────────── */

static bool is_gflag_macro(const char *name) {
    return (strncmp(name, "DEFINE_bool", 11) == 0 ||
            strncmp(name, "DEFINE_int32", 12) == 0 ||
            strncmp(name, "DEFINE_int64", 12) == 0 ||
            strncmp(name, "DEFINE_uint64", 13) == 0 ||
            strncmp(name, "DEFINE_double", 13) == 0 ||
            strncmp(name, "DEFINE_string", 13) == 0);
}

static int process_document_symbols(cbm_pipeline_ctx_t *ctx, const char *file_path,
                                    const char *response_json) {
    if (!ctx || !response_json) return 0;

    yyjson_doc *doc = yyjson_read(response_json, strlen(response_json), 0);
    if (!doc) return 0;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (!result || !yyjson_is_arr(result)) {
        yyjson_doc_free(doc);
        return 0;
    }

    int created = 0;
    size_t idx, max;
    yyjson_val *sym;
    yyjson_arr_foreach(result, idx, max, sym) {
        yyjson_val *name_val = yyjson_obj_get(sym, "name");
        if (!name_val) continue;
        const char *name = yyjson_get_str(name_val);
        if (!name || !is_gflag_macro(name)) continue;

        /* Extract flag name from the symbol detail or name itself.
         * clangd typically reports the macro call as the name. */
        yyjson_val *detail = yyjson_obj_get(sym, "detail");
        const char *flag_name = detail ? yyjson_get_str(detail) : name;
        if (!flag_name) flag_name = name;

        /* Build QN: project.__gflag__FLAG_NAME */
        char qn[512];
        snprintf(qn, sizeof(qn), "%s.__gflag__%s", ctx->project_name, flag_name);

        yyjson_val *range = yyjson_obj_get(sym, "range");
        int start_line = 0, end_line = 0;
        if (range) {
            yyjson_val *start = yyjson_obj_get(range, "start");
            yyjson_val *end = yyjson_obj_get(range, "end");
            if (start) start_line = (int)yyjson_get_int(yyjson_obj_get(start, "line")) + 1;
            if (end) end_line = (int)yyjson_get_int(yyjson_obj_get(end, "line")) + 1;
        }

        cbm_gbuf_upsert_node(ctx->gbuf, "GflagDef", flag_name, qn,
                             file_path, start_line, end_line,
                             "{\"strategy\":\"clangd_document_symbol\"}");
        created++;
    }

    yyjson_doc_free(doc);
    return created;
}

/* ── CALLS edge enhancement ──────────────────────────────────────── */

static int process_outgoing_calls(cbm_pipeline_ctx_t *ctx, cbm_lsp_client_t *lsp,
                                  const cbm_gbuf_node_t *caller_node,
                                  const char *prepare_response) {
    if (!prepare_response) return 0;

    yyjson_doc *doc = yyjson_read(prepare_response, strlen(prepare_response), 0);
    if (!doc) return 0;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *result = yyjson_obj_get(root, "result");
    if (!result || !yyjson_is_arr(result) || yyjson_arr_size(result) == 0) {
        yyjson_doc_free(doc);
        return 0;
    }

    int edges_created = 0;

    /* For each CallHierarchyItem returned by prepare */
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(result, idx, max, item) {
        /* Serialize item for outgoingCalls request */
        char *item_str = yyjson_val_write(item, 0, NULL);
        if (!item_str) continue;

        char *outgoing_resp = NULL;
        int rc = cbm_lsp_client_outgoing_calls(lsp, item_str, &outgoing_resp, CLANGD_TIMEOUT_MS);
        free(item_str);
        if (rc != 0 || !outgoing_resp) continue;

        /* Parse outgoing calls response */
        yyjson_doc *out_doc = yyjson_read(outgoing_resp, strlen(outgoing_resp), 0);
        if (!out_doc) {
            free(outgoing_resp);
            continue;
        }

        yyjson_val *out_root = yyjson_doc_get_root(out_doc);
        yyjson_val *out_result = yyjson_obj_get(out_root, "result");
        if (out_result && yyjson_is_arr(out_result)) {
            size_t oidx, omax;
            yyjson_val *call;
            yyjson_arr_foreach(out_result, oidx, omax, call) {
                yyjson_val *to = yyjson_obj_get(call, "to");
                if (!to) continue;

                yyjson_val *to_name = yyjson_obj_get(to, "name");
                yyjson_val *to_uri = yyjson_obj_get(to, "uri");
                if (!to_name || !to_uri) continue;

                const char *callee_name = yyjson_get_str(to_name);
                const char *callee_uri = yyjson_get_str(to_uri);
                if (!callee_name || !callee_uri) continue;

                /* Resolve callee to a node in gbuf */
                char *callee_rel = uri_to_rel_path(callee_uri, ctx->repo_path);
                if (!callee_rel) continue;

                const cbm_gbuf_node_t **matches = NULL;
                int match_count = 0;
                cbm_gbuf_find_by_name(ctx->gbuf, callee_name, &matches, &match_count);

                const cbm_gbuf_node_t *best = NULL;
                for (int m = 0; m < match_count; m++) {
                    if (matches[m]->file_path &&
                        strcmp(matches[m]->file_path, callee_rel) == 0) {
                        /* Get line number for tie-breaking */
                        yyjson_val *to_range = yyjson_obj_get(to, "range");
                        if (to_range && matches[m]->start_line > 0) {
                            yyjson_val *to_start = yyjson_obj_get(to_range, "start");
                            int to_line = to_start ?
                                (int)yyjson_get_int(yyjson_obj_get(to_start, "line")) + 1 : 0;
                            if (to_line == matches[m]->start_line) {
                                best = matches[m];
                                break;
                            }
                        }
                        if (!best) best = matches[m];
                    }
                }

                if (best && best->id != caller_node->id) {
                    cbm_gbuf_insert_edge(ctx->gbuf, caller_node->id, best->id, "CALLS",
                                         "{\"strategy\":\"clangd_call_hierarchy\","
                                         "\"confidence\":0.95}");
                    edges_created++;
                }
                free(callee_rel);
            }
        }

        yyjson_doc_free(out_doc);
        free(outgoing_resp);
    }

    yyjson_doc_free(doc);
    return edges_created;
}

/* ── Main pass ───────────────────────────────────────────────────── */

static int pass_clangd(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                       int file_count, CBMFileResult **cache) {
    (void)cache;
    if (!ctx || !files || file_count <= 0) return 0;

    if (!clangd_available(ctx->repo_path)) {
        cbm_log_info("pass.clangd.skip", "reason", "no clangd or compile_commands.json");
        return 0;
    }

    char *clangd_path = cbm_which("clangd");
    if (!clangd_path) return 0;

    char root_uri[4096];
    snprintf(root_uri, sizeof(root_uri), "file://%s", ctx->repo_path);

    cbm_lsp_client_opts_t opts = {
        .root_uri = root_uri,
        .clangd_path = clangd_path,
        .timeout_ms = CLANGD_TIMEOUT_MS,
    };

    cbm_lsp_client_t *lsp = cbm_lsp_client_new(&opts);
    free(clangd_path);
    if (!lsp) {
        cbm_log_error("pass.clangd.skip", "reason", "initialize failed");
        return 0;
    }

    cbm_log_info("pass.clangd.found", "files", "scanning");

    int total_symbols = 0, total_edges = 0, files_processed = 0;

    for (int i = 0; i < file_count; i++) {
        if (i % CLANGD_CANCEL_INTERVAL == 0 && cbm_pipeline_check_cancel(ctx)) break;

        const char *rel_path = files[i].rel_path;
        if (!rel_path || !is_cpp_file(rel_path)) continue;

        /* Check if clangd is still alive */
        if (!cbm_lsp_client_is_alive(lsp)) {
            cbm_log_error("pass.clangd.crash", "file", rel_path);
            /* Try restart once */
            cbm_lsp_client_free(lsp);
            clangd_path = cbm_which("clangd");
            if (!clangd_path) break;
            opts.clangd_path = clangd_path;
            lsp = cbm_lsp_client_new(&opts);
            free(clangd_path);
            if (!lsp) break;
        }

        /* Read source file */
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", ctx->repo_path, rel_path);
        FILE *fp = fopen(full_path, "r");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (fsize <= 0 || fsize > 5000000) { fclose(fp); continue; }
        char *source = malloc((size_t)fsize + 1);
        if (!source) { fclose(fp); continue; }
        size_t read_sz = fread(source, 1, (size_t)fsize, fp);
        fclose(fp);
        source[read_sz] = '\0';

        char *uri = build_file_uri(ctx->repo_path, rel_path);
        if (!uri) { free(source); continue; }

        /* didOpen */
        cbm_lsp_client_did_open(lsp, uri, lang_id_for_ext(rel_path), 1, source);
        free(source);

        /* documentSymbol → GflagDef discovery */
        char *sym_resp = NULL;
        if (cbm_lsp_client_document_symbol(lsp, uri, &sym_resp, CLANGD_TIMEOUT_MS) == 0) {
            total_symbols += process_document_symbols(ctx, rel_path, sym_resp);
            free(sym_resp);
        }

        /* callHierarchy for each Function/Method node in this file */
        const cbm_gbuf_node_t **nodes = NULL;
        int node_count = 0;
        /* We iterate by finding nodes for this file via label search */
        const cbm_gbuf_node_t **func_nodes = NULL;
        int func_count = 0;
        cbm_gbuf_find_by_label(ctx->gbuf, "Function", &func_nodes, &func_count);

        for (int f = 0; f < func_count; f++) {
            if (!func_nodes[f]->file_path ||
                strcmp(func_nodes[f]->file_path, rel_path) != 0) continue;
            if (func_nodes[f]->start_line <= 0) continue;

            char *prep_resp = NULL;
            int rc = cbm_lsp_client_prepare_call_hierarchy(
                lsp, uri, func_nodes[f]->start_line - 1, 0,
                &prep_resp, CLANGD_TIMEOUT_MS);
            if (rc == 0 && prep_resp) {
                total_edges += process_outgoing_calls(ctx, lsp, func_nodes[f], prep_resp);
                free(prep_resp);
            }
        }

        /* Also check Method nodes */
        const cbm_gbuf_node_t **method_nodes = NULL;
        int method_count = 0;
        cbm_gbuf_find_by_label(ctx->gbuf, "Method", &method_nodes, &method_count);

        for (int m = 0; m < method_count; m++) {
            if (!method_nodes[m]->file_path ||
                strcmp(method_nodes[m]->file_path, rel_path) != 0) continue;
            if (method_nodes[m]->start_line <= 0) continue;

            char *prep_resp = NULL;
            int rc = cbm_lsp_client_prepare_call_hierarchy(
                lsp, uri, method_nodes[m]->start_line - 1, 0,
                &prep_resp, CLANGD_TIMEOUT_MS);
            if (rc == 0 && prep_resp) {
                total_edges += process_outgoing_calls(ctx, lsp, method_nodes[m], prep_resp);
                free(prep_resp);
            }
        }

        /* didClose */
        cbm_lsp_client_did_close(lsp, uri);
        free(uri);
        files_processed++;

        (void)nodes;
        (void)node_count;
    }

    /* Shutdown */
    cbm_lsp_client_shutdown(lsp);
    cbm_lsp_client_free(lsp);

    char files_buf[32], sym_buf[32], edge_buf[32];
    snprintf(files_buf, sizeof(files_buf), "%d", files_processed);
    snprintf(sym_buf, sizeof(sym_buf), "%d", total_symbols);
    snprintf(edge_buf, sizeof(edge_buf), "%d", total_edges);
    cbm_log_info("pass.clangd.done", "files", files_buf,
                 "gflag_nodes", sym_buf, "edges", edge_buf);
    return 0;
}

/* ── Self-registration ───────────────────────────────────────────── */

static void __attribute__((constructor)) register_clangd(void) {
    cbm_pipeline_hook_register(CBM_HOOK_AFTER_LSP_CROSS, pass_clangd, "clangd");
}

/* ── Testing helpers ─────────────────────────────────────────────── */

bool cbm_clangd_available_test(const char *p) { return clangd_available(p); }
char *cbm_uri_to_rel_path_test(const char *uri, const char *rp) { return uri_to_rel_path(uri, rp); }
char *cbm_build_file_uri_test(const char *rp, const char *rel) { return build_file_uri(rp, rel); }
