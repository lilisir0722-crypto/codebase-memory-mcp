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
    CLANGD_TIMEOUT_MS     = 45000,
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

    /* Stack-based traversal to handle nested DocumentSymbol (children) */
    yyjson_val *stack[256];
    int stack_top = 0;
    size_t idx, max;
    yyjson_val *sym;
    yyjson_arr_foreach(result, idx, max, sym) {
        if (stack_top < 256) stack[stack_top++] = sym;
    }

    while (stack_top > 0) {
        yyjson_val *cur = stack[--stack_top];

        /* Push children for recursive traversal */
        yyjson_val *children = yyjson_obj_get(cur, "children");
        if (children && yyjson_is_arr(children)) {
            size_t ci, cmax;
            yyjson_val *child;
            yyjson_arr_foreach(children, ci, cmax, child) {
                if (stack_top < 256) stack[stack_top++] = child;
            }
        }

        yyjson_val *name_val = yyjson_obj_get(cur, "name");
        if (!name_val) continue;
        const char *name = yyjson_get_str(name_val);
        if (!name || !is_gflag_macro(name)) continue;

        yyjson_val *detail = yyjson_obj_get(cur, "detail");
        const char *flag_name = detail ? yyjson_get_str(detail) : name;
        if (!flag_name) flag_name = name;

        char qn[512];
        snprintf(qn, sizeof(qn), "%s.__gflag__%s", ctx->project_name, flag_name);

        yyjson_val *range = yyjson_obj_get(cur, "range");
        if (!range) {
            yyjson_val *loc = yyjson_obj_get(cur, "location");
            if (loc) range = yyjson_obj_get(loc, "range");
        }
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
                        /* Always fallback to first file-matching node */
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

/* Parse compile_commands.json and return a set of file paths (as sorted array).
 * Caller must free *out_paths and each entry. Returns count. */
static int load_compile_db_files(const char *repo_path, char ***out_paths) {
    *out_paths = NULL;
    char cc_path[4096];
    snprintf(cc_path, sizeof(cc_path), "%s/compile_commands.json", repo_path);

    FILE *fp = fopen(cc_path, "r");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 50 * 1024 * 1024) { fclose(fp); return 0; }
    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(fp); return 0; }
    size_t rd = fread(data, 1, (size_t)fsize, fp);
    fclose(fp);
    data[rd] = '\0';

    yyjson_doc *doc = yyjson_read(data, rd, 0);
    free(data);
    if (!doc) return 0;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) { yyjson_doc_free(doc); return 0; }

    size_t arr_sz = yyjson_arr_size(root);
    if (arr_sz == 0 || arr_sz > 100000) { yyjson_doc_free(doc); return 0; }
    int cap = (int)arr_sz;
    char **paths = calloc((size_t)cap, sizeof(char *));
    int count = 0;

    size_t idx, max;
    yyjson_val *entry;
    yyjson_arr_foreach(root, idx, max, entry) {
        yyjson_val *file_val = yyjson_obj_get(entry, "file");
        if (!file_val) continue;
        const char *file_str = yyjson_get_str(file_val);
        if (!file_str) continue;
        /* Store relative path */
        if (file_str[0] == '/') {
            /* Absolute path: strip repo_path prefix */
            size_t rlen = strlen(repo_path);
            if (strncmp(file_str, repo_path, rlen) == 0 && file_str[rlen] == '/') {
                paths[count++] = strdup(file_str + rlen + 1);
            } else {
                paths[count++] = strdup(file_str);
            }
        } else {
            paths[count++] = strdup(file_str);
        }
    }
    yyjson_doc_free(doc);
    *out_paths = paths;
    return count;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static bool in_compile_db(const char *rel_path, char **sorted_paths, int count) {
    return bsearch(&rel_path, sorted_paths, (size_t)count, sizeof(char *), cmp_str) != NULL;
}

static int pass_clangd(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                       int file_count, CBMFileResult **cache) {
    (void)cache;
    if (!ctx || !files || file_count <= 0) return 0;

    if (!clangd_available(ctx->repo_path)) {
        cbm_log_info("pass.clangd.skip", "reason", "no clangd or compile_commands.json");
        return 0;
    }

    /* Load compile_commands.json file set — only process listed files */
    char **db_paths = NULL;
    int db_count = load_compile_db_files(ctx->repo_path, &db_paths);
    if (db_count <= 0) {
        cbm_log_info("pass.clangd.skip", "reason", "compile_commands.json empty or unreadable");
        return 0;
    }
    qsort(db_paths, (size_t)db_count, sizeof(char *), cmp_str);

    char *clangd_path = cbm_which("clangd");
    if (!clangd_path) {
        for (int j = 0; j < db_count; j++) free(db_paths[j]);
        free(db_paths);
        return 0;
    }

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
        for (int j = 0; j < db_count; j++) free(db_paths[j]);
        free(db_paths);
        return 0;
    }

    char db_count_buf[32];
    snprintf(db_count_buf, sizeof(db_count_buf), "%d", db_count);
    cbm_log_info("pass.clangd.found", "compile_db_files", db_count_buf);

    int total_symbols = 0, total_edges = 0, files_processed = 0;

    char progress_buf[32], total_buf[32], edges_buf[32], elapsed_buf[32];
    snprintf(total_buf, sizeof(total_buf), "%d", db_count);

    struct timespec clangd_start;
    clock_gettime(CLOCK_MONOTONIC, &clangd_start);

    for (int i = 0; i < file_count; i++) {
        if (i % CLANGD_CANCEL_INTERVAL == 0 && cbm_pipeline_check_cancel(ctx)) break;

        const char *rel_path = files[i].rel_path;
        if (!rel_path || !is_cpp_file(rel_path)) continue;

        /* Only process files listed in compile_commands.json */
        if (!in_compile_db(rel_path, db_paths, db_count)) continue;

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

        /* Poll documentSymbol until clangd finishes parsing (adaptive wait).
         * Strategy: request documentSymbol repeatedly with backoff until we get
         * meaningful symbols (functions/methods), or hit max retries.
         * First file may need longer (preamble parsing); subsequent files reuse
         * the preamble cache and resolve quickly. */
        char *sym_resp = NULL;
        int poll_ready = 0;
        {
            enum { MAX_POLL_NORMAL = 15, MAX_POLL_FIRST = 25, INITIAL_WAIT_MS = 500, MAX_WAIT_MS = 5000 };
            int max_attempts = (files_processed < 3) ? MAX_POLL_FIRST : MAX_POLL_NORMAL;
            int wait_ms = INITIAL_WAIT_MS;
            for (int attempt = 0; attempt < max_attempts; attempt++) {
                if (!cbm_lsp_client_is_alive(lsp)) {
                    cbm_log_warn("pass.clangd.poll", "event", "clangd_died",
                                 "file", rel_path, "attempt", "dead");
                    break;
                }
                struct timespec ts = {wait_ms / 1000, (wait_ms % 1000) * 1000000L};
                nanosleep(&ts, NULL);

                char *probe = NULL;
                if (cbm_lsp_client_document_symbol(lsp, uri, &probe, CLANGD_TIMEOUT_MS) != 0 || !probe) {
                    char attempt_buf[16];
                    snprintf(attempt_buf, sizeof(attempt_buf), "%d", attempt + 1);
                    cbm_log_warn("pass.clangd.poll", "event", "no_response",
                                 "file", rel_path, "attempt", attempt_buf);
                    wait_ms = (wait_ms * 2 > MAX_WAIT_MS) ? MAX_WAIT_MS : wait_ms * 2;
                    continue;
                }

                /* Count symbols with kind=Function(12) or Method(6) */
                size_t probe_len = strlen(probe);
                if (probe_len == 0) {
                    free(probe);
                    wait_ms = (wait_ms * 2 > MAX_WAIT_MS) ? MAX_WAIT_MS : wait_ms * 2;
                    continue;
                }
                yyjson_doc *pdoc = yyjson_read(probe, probe_len, 0);
                int func_syms = 0;
                if (pdoc) {
                    yyjson_val *proot = yyjson_doc_get_root(pdoc);
                    if (proot && yyjson_is_obj(proot)) {
                        yyjson_val *presult = yyjson_obj_get(proot, "result");
                        if (presult && yyjson_is_arr(presult)) {
                            size_t sidx, smax;
                            yyjson_val *sym;
                            yyjson_arr_foreach(presult, sidx, smax, sym) {
                                yyjson_val *kind_val = yyjson_obj_get(sym, "kind");
                                int kind = kind_val ? (int)yyjson_get_int(kind_val) : 0;
                                if (kind == 6 || kind == 12) func_syms++;
                            }
                        }
                    }
                    yyjson_doc_free(pdoc);
                }

                if (func_syms > 0) {
                    char attempt_buf[16], sym_buf[16];
                    snprintf(attempt_buf, sizeof(attempt_buf), "%d", attempt + 1);
                    snprintf(sym_buf, sizeof(sym_buf), "%d", func_syms);
                    cbm_log_info("pass.clangd.poll", "event", "ready",
                                 "file", rel_path, "attempt", attempt_buf,
                                 "func_syms", sym_buf);
                    sym_resp = probe;
                    poll_ready = 1;
                    break;
                }

                free(probe);
                /* Log wait on first file (preamble) */
                if (files_processed == 0) {
                    char attempt_buf[16], wait_buf[16];
                    snprintf(attempt_buf, sizeof(attempt_buf), "%d", attempt + 1);
                    snprintf(wait_buf, sizeof(wait_buf), "%d", wait_ms);
                    cbm_log_info("pass.clangd.poll", "event", "waiting_preamble",
                                 "file", rel_path, "attempt", attempt_buf,
                                 "next_wait_ms", wait_buf);
                }
                wait_ms = (wait_ms * 2 > MAX_WAIT_MS) ? MAX_WAIT_MS : wait_ms * 2;
            }

            if (!poll_ready && !sym_resp) {
                cbm_log_warn("pass.clangd.poll", "event", "timeout",
                             "file", rel_path, "reason", "max_attempts_exceeded");
            }
        }

        /* Process GflagDef nodes from documentSymbol response */
        if (sym_resp) {
            total_symbols += process_document_symbols(ctx, rel_path, sym_resp);
        }

        /* callHierarchy — only attempt if documentSymbol resolved functions.
         * Use selectionRange from documentSymbol for precise cursor position. */
        int file_edges = 0;
        if (!poll_ready || !sym_resp) {
            cbm_log_warn("pass.clangd.file_skip", "file", rel_path,
                         "reason", "documentSymbol_not_ready");
        } else {
            /* Parse sym_resp to get function positions from documentSymbol */
            yyjson_doc *sym_doc = yyjson_read(sym_resp, strlen(sym_resp), 0);
            if (!sym_doc) {
                cbm_log_warn("pass.clangd.error", "event", "sym_parse_failed", "file", rel_path);
            } else {
                yyjson_val *sroot = yyjson_doc_get_root(sym_doc);
                yyjson_val *sresult = NULL;
                if (sroot && yyjson_is_obj(sroot))
                    sresult = yyjson_obj_get(sroot, "result");
                if (!sresult || !yyjson_is_arr(sresult)) {
                    cbm_log_warn("pass.clangd.error", "event", "no_result_array", "file", rel_path);
                } else {
                    /* Flatten hierarchical DocumentSymbol: collect all symbols
                     * including nested children into a flat iteration */
                    size_t si, smax;
                    yyjson_val *sym;
                    /* Stack-based traversal for nested DocumentSymbol */
                    yyjson_val *stack[256];
                    int stack_top = 0;
                    yyjson_arr_foreach(sresult, si, smax, sym) {
                        if (stack_top < 256) stack[stack_top++] = sym;
                    }
                    while (stack_top > 0) {
                        yyjson_val *cur = stack[--stack_top];
                        /* Push children first */
                        yyjson_val *children = yyjson_obj_get(cur, "children");
                        if (children && yyjson_is_arr(children)) {
                            size_t ci, cmax;
                            yyjson_val *child;
                            yyjson_arr_foreach(children, ci, cmax, child) {
                                if (stack_top < 256) stack[stack_top++] = child;
                            }
                        }
                        yyjson_val *kind_val = yyjson_obj_get(cur, "kind");
                        int kind = kind_val ? (int)yyjson_get_int(kind_val) : 0;
                        /* kind 6=Method, 9=Constructor, 12=Function */
                        if (kind != 6 && kind != 9 && kind != 12) continue;

                        /* Get precise position from selectionRange / range / location.range */
                        yyjson_val *sel = yyjson_obj_get(cur, "selectionRange");
                        if (!sel) sel = yyjson_obj_get(cur, "range");
                        if (!sel) {
                            yyjson_val *loc = yyjson_obj_get(cur, "location");
                            if (loc) sel = yyjson_obj_get(loc, "range");
                        }
                        if (!sel) continue;
                        yyjson_val *start = yyjson_obj_get(sel, "start");
                        if (!start) continue;
                        int sym_line = (int)yyjson_get_int(yyjson_obj_get(start, "line"));
                        int sym_char = (int)yyjson_get_int(yyjson_obj_get(start, "character"));

                        /* For SymbolInformation format, character is often 0 (line start).
                         * prepareCallHierarchy needs cursor ON the function name.
                         * Heuristic: if char==0 and name contains "::", use offset past "::" */
                        yyjson_val *nv2 = yyjson_obj_get(cur, "name");
                        const char *fname = nv2 ? yyjson_get_str(nv2) : NULL;
                        if (sym_char == 0 && fname) {
                            const char *colon = strstr(fname, "::");
                            if (colon) {
                                sym_char = (int)(colon - fname) + 2;
                            }
                        }

                        char *prep_resp = NULL;
                        int rc = cbm_lsp_client_prepare_call_hierarchy(
                            lsp, uri, sym_line, sym_char,
                            &prep_resp, CLANGD_TIMEOUT_MS);

                        if (rc == 0 && prep_resp) {
                            /* Find matching node in gbuf for edge source */
                            const cbm_gbuf_node_t *src_node = NULL;
                            yyjson_val *sym_name = yyjson_obj_get(cur, "name");
                            const char *sname = sym_name ? yyjson_get_str(sym_name) : NULL;
                            if (sname) {
                                /* Try to find by name + file match */
                                const cbm_gbuf_node_t **matches = NULL;
                                int match_count = 0;
                                cbm_gbuf_find_by_name(ctx->gbuf, sname, &matches, &match_count);
                                for (int mi = 0; mi < match_count; mi++) {
                                    if (matches[mi]->file_path &&
                                        strcmp(matches[mi]->file_path, rel_path) == 0) {
                                        src_node = matches[mi];
                                        break;
                                    }
                                }
                                /* Fallback: strip class prefix (e.g. "Class::Method" -> "Method") */
                                if (!src_node) {
                                    const char *colon = strstr(sname, "::");
                                    if (colon) {
                                        const char *short_name = colon + 2;
                                        cbm_gbuf_find_by_name(ctx->gbuf, short_name, &matches, &match_count);
                                        for (int mi = 0; mi < match_count; mi++) {
                                            if (matches[mi]->file_path &&
                                                strcmp(matches[mi]->file_path, rel_path) == 0) {
                                                src_node = matches[mi];
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                            if (src_node) {
                                file_edges += process_outgoing_calls(ctx, lsp, src_node, prep_resp);
                            }
                            free(prep_resp);
                        }
                    }
                }
                yyjson_doc_free(sym_doc);
            }
        }
        total_edges += file_edges;
        free(sym_resp);

        /* didClose */
        cbm_lsp_client_did_close(lsp, uri);
        free(uri);
        files_processed++;

        if (files_processed % 10 == 0 || files_processed == db_count) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int elapsed_s = (int)(now.tv_sec - clangd_start.tv_sec);
            snprintf(progress_buf, sizeof(progress_buf), "%d", files_processed);
            snprintf(edges_buf, sizeof(edges_buf), "%d", total_edges);
            snprintf(elapsed_buf, sizeof(elapsed_buf), "%ds", elapsed_s);
            cbm_log_info("pass.clangd.progress", "done", progress_buf,
                         "total", total_buf, "edges", edges_buf,
                         "elapsed", elapsed_buf, "file", rel_path);
        }
    }

    /* Shutdown */
    cbm_lsp_client_shutdown(lsp);
    cbm_lsp_client_free(lsp);

    for (int j = 0; j < db_count; j++) free(db_paths[j]);
    free(db_paths);

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

#ifdef CBM_TESTING
bool cbm_clangd_available_test(const char *p) { return clangd_available(p); }
char *cbm_uri_to_rel_path_test(const char *uri, const char *rp) { return uri_to_rel_path(uri, rp); }
char *cbm_build_file_uri_test(const char *rp, const char *rel) { return build_file_uri(rp, rel); }
#endif
