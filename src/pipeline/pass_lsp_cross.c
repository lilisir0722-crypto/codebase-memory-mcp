/*
 * pass_lsp_cross.c — Cross-file LSP type-aware call resolution pass.
 *
 * See pass_lsp_cross.h for the high-level contract. This file is the
 * pipeline glue that converts the existing per-file extraction state
 * (CBMDefinition / CBMImport / IMPORTS-edge gbuf state) into the input
 * shape each language LSP's cbm_run_X_lsp_cross expects, then merges
 * the resulting CBMResolvedCall entries back into per-file results.
 *
 * The pass is a no-op for any file whose CBMFileResult is missing or
 * whose language has no cross-file LSP entry registered (e.g. Rust /
 * Java today). Per-LSP emit functions dedup against entries already in
 * resolved_calls, so this pass is also idempotent — safe to invoke
 * multiple times if the pipeline gains a re-run path later.
 *
 * Parallelism: each file is processed independently (cache[i] is
 * per-file, gbuf / all_defs are read-only during the loop). The main
 * loop is dispatched via cbm_parallel_for. Counters are atomic.
 * A watchdog thread logs files that appear stuck (> 30 s) without
 * interrupting them.
 */
#include "pipeline/pass_lsp_cross.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/worker_pool.h"
#include "lsp/go_lsp.h"
#include "lsp/c_lsp.h"
#include "lsp/py_lsp.h"
#include "lsp/ts_lsp.h"
#include "lsp/php_lsp.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/compat_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────────────── */

enum {
    PXC_MAX_FILE_BYTES_FACTOR = 100,
    PXC_ITOA_BUF = 16,
    PXC_WATCHDOG_INTERVAL_SEC = 10,
    PXC_WATCHDOG_WARN_SEC = 30,
};

static const char *itoa_buf(int val) {
    static _Thread_local char bufs[PXC_ITOA_BUF][PXC_ITOA_BUF];
    static _Thread_local int slot = 0;
    char *out = bufs[slot];
    slot = (slot + 1) & (PXC_ITOA_BUF - 1);
    snprintf(out, PXC_ITOA_BUF, "%d", val);
    return out;
}

/* ── Watchdog ───────────────────────────────────────────────────── */

#define PXC_MAX_WORKERS 64

typedef struct {
    _Atomic long    start_sec;   /* 0 = idle */
    _Atomic int     file_idx;    /* current file index being processed */
} pxc_worker_slot_t;

typedef struct {
    pxc_worker_slot_t   slots[PXC_MAX_WORKERS];
    int                 nslots;
    _Atomic int         done;          /* set to 1 to stop watchdog */
    const cbm_file_info_t *files;
    int                 file_count;
    _Atomic int        *processed;
} pxc_watchdog_ctx_t;

static long pxc_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static _Thread_local int g_worker_slot = -1;
static pxc_watchdog_ctx_t *g_watchdog_ctx = NULL;

static void pxc_worker_begin(int file_idx) {
    if (!g_watchdog_ctx || g_worker_slot < 0) return;
    pxc_worker_slot_t *s = &g_watchdog_ctx->slots[g_worker_slot];
    atomic_store_explicit(&s->file_idx, file_idx, memory_order_relaxed);
    atomic_store_explicit(&s->start_sec, pxc_now_sec(), memory_order_release);
}

static void pxc_worker_end(void) {
    if (!g_watchdog_ctx || g_worker_slot < 0) return;
    pxc_worker_slot_t *s = &g_watchdog_ctx->slots[g_worker_slot];
    atomic_store_explicit(&s->start_sec, 0, memory_order_release);
}

static void *pxc_watchdog_thread(void *arg) {
    pxc_watchdog_ctx_t *ctx = (pxc_watchdog_ctx_t *)arg;
    while (!atomic_load_explicit(&ctx->done, memory_order_relaxed)) {
        struct timespec ts = {PXC_WATCHDOG_INTERVAL_SEC, 0};
        nanosleep(&ts, NULL);

        if (atomic_load_explicit(&ctx->done, memory_order_relaxed)) break;

        long now = pxc_now_sec();
        int total = atomic_load_explicit(ctx->processed, memory_order_relaxed);
        cbm_log_info("pass.lsp_cross.progress",
                     "processed", itoa_buf(total),
                     "total", itoa_buf(ctx->file_count));

        for (int i = 0; i < ctx->nslots; i++) {
            pxc_worker_slot_t *s = &ctx->slots[i];
            long start = atomic_load_explicit(&s->start_sec, memory_order_acquire);
            if (start == 0) continue;
            long elapsed = now - start;
            if (elapsed >= PXC_WATCHDOG_WARN_SEC) {
                int fi = atomic_load_explicit(&s->file_idx, memory_order_relaxed);
                const char *path = (fi >= 0 && fi < ctx->file_count)
                                    ? ctx->files[fi].rel_path : "?";
                cbm_log_warn("pass.lsp_cross.slow",
                             "worker", itoa_buf(i),
                             "elapsed_sec", itoa_buf((int)elapsed),
                             "path", path);
            }
        }
    }
    return NULL;
}

/* ── Local helpers ─────────────────────────────────────────────── */

static char *pxc_read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    (void)fseek(f, 0, SEEK_END);
    long size = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (size <= 0 ||
        size > (long)PXC_MAX_FILE_BYTES_FACTOR * (long)CBM_SZ_1K * (long)CBM_SZ_1K) {
        (void)fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        (void)fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    (void)fclose(f);
    if (nread > (size_t)size) nread = (size_t)size;
    buf[nread] = '\0';
    *out_len = (int)nread;
    return buf;
}

static const char *pxc_map_label(const char *label) {
    if (!label) return NULL;
    if (strcmp(label, "Class") == 0 ||
        strcmp(label, "Interface") == 0 ||
        strcmp(label, "Trait") == 0 ||
        strcmp(label, "Enum") == 0 ||
        strcmp(label, "Type") == 0 ||
        strcmp(label, "Protocol") == 0 ||
        strcmp(label, "Function") == 0 ||
        strcmp(label, "Method") == 0) {
        return label;
    }
    return NULL;
}

static const char *pxc_join_pipe(CBMArena *arena, const char *const *items) {
    if (!items || !items[0]) return NULL;
    int count = 0;
    size_t total = 0;
    for (int i = 0; items[i]; i++) {
        count++;
        total += strlen(items[i]);
    }
    if (count == 0) return NULL;
    size_t bufsz = total + (size_t)(count - 1) + 1;
    char *buf = (char *)cbm_arena_alloc(arena, bufsz);
    if (!buf) return NULL;
    char *p = buf;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(items[i]);
        memcpy(p, items[i], n);
        p += n;
        if (i + 1 < count) *p++ = '|';
    }
    *p = '\0';
    return buf;
}

static int pxc_build_lsp_def(CBMArena *arena, const CBMDefinition *src,
                              const char *module_qn, CBMLSPDef *dst) {
    const char *label = pxc_map_label(src->label);
    if (!label || !src->qualified_name || !src->name) return -1;
    memset(dst, 0, sizeof(*dst));
    dst->qualified_name = src->qualified_name;
    dst->short_name = src->name;
    dst->label = label;
    dst->receiver_type = src->parent_class;
    dst->def_module_qn = module_qn;
    dst->is_interface = (strcmp(label, "Interface") == 0 ||
                          strcmp(label, "Protocol") == 0);
    dst->return_types = src->return_type;
    dst->embedded_types = pxc_join_pipe(arena, src->base_classes);
    return 0;
}

static CBMLSPDef *pxc_collect_all_defs(CBMFileResult **cache,
                                       const cbm_file_info_t *files, int file_count,
                                       const char *project_name,
                                       char **def_modules, int *out_count) {
    int total = 0;
    for (int i = 0; i < file_count; i++) {
        if (cache[i]) total += cache[i]->defs.count;
    }
    if (total == 0) {
        *out_count = 0;
        return NULL;
    }
    CBMLSPDef *defs = (CBMLSPDef *)calloc((size_t)total, sizeof(CBMLSPDef));
    if (!defs) {
        *out_count = 0;
        return NULL;
    }
    int idx = 0;
    for (int fi = 0; fi < file_count; fi++) {
        if (!cache[fi]) continue;
        if (!def_modules[fi]) {
            def_modules[fi] = cbm_pipeline_fqn_module(project_name, files[fi].rel_path);
        }
        for (int di = 0; di < cache[fi]->defs.count; di++) {
            if (pxc_build_lsp_def(&cache[fi]->arena,
                                   &cache[fi]->defs.items[di],
                                   def_modules[fi],
                                   &defs[idx]) == 0) {
                idx++;
            }
        }
    }
    *out_count = idx;
    return defs;
}

static int pxc_build_import_map(const cbm_gbuf_t *gbuf, const char *project_name,
                                 const char *rel_path, const char ***out_keys,
                                 const char ***out_vals, int *out_count) {
    *out_keys = NULL;
    *out_vals = NULL;
    *out_count = 0;

    char *file_qn = cbm_pipeline_fqn_compute(project_name, rel_path, "__file__");
    if (!file_qn) return 0;
    const cbm_gbuf_node_t *file_node = cbm_gbuf_find_by_qn(gbuf, file_qn);
    free(file_qn);
    if (!file_node) return 0;

    const cbm_gbuf_edge_t **edges = NULL;
    int edge_count = 0;
    int rc = cbm_gbuf_find_edges_by_source_type(gbuf, file_node->id, "IMPORTS",
                                                  &edges, &edge_count);
    if (rc != 0 || edge_count == 0) return 0;

    const char **keys = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    const char **vals = (const char **)calloc((size_t)edge_count, sizeof(const char *));
    if (!keys || !vals) {
        free(keys);
        free(vals);
        return 0;
    }
    int count = 0;
    for (int i = 0; i < edge_count; i++) {
        const cbm_gbuf_edge_t *e = edges[i];
        const cbm_gbuf_node_t *target = cbm_gbuf_find_by_id(gbuf, e->target_id);
        if (!target || !e->properties_json) continue;
        const char *start = strstr(e->properties_json, "\"local_name\":\"");
        if (!start) continue;
        start += strlen("\"local_name\":\"");
        const char *end = strchr(start, '"');
        if (!end || end <= start) continue;
        size_t n = (size_t)(end - start);
        char *local = (char *)malloc(n + 1);
        if (!local) continue;
        memcpy(local, start, n);
        local[n] = '\0';
        keys[count] = local;
        vals[count] = target->qualified_name;
        count++;
    }
    *out_keys = keys;
    *out_vals = vals;
    *out_count = count;
    return 0;
}

static void pxc_free_import_map(const char **keys, const char **vals, int count) {
    if (keys) {
        for (int i = 0; i < count; i++) free((void *)keys[i]);
        free((void *)keys);
    }
    free((void *)vals);
}

static void pxc_ts_modes(CBMLanguage lang, const char *rel_path,
                          bool *out_js, bool *out_jsx, bool *out_dts) {
    *out_js = (lang == CBM_LANG_JAVASCRIPT);
    *out_jsx = (lang == CBM_LANG_TSX);
    *out_dts = false;
    if (!rel_path) return;
    size_t rl = strlen(rel_path);
    if (lang == CBM_LANG_JAVASCRIPT && rl >= 4 &&
        strcmp(rel_path + rl - 4, ".jsx") == 0) {
        *out_jsx = true;
    }
    if (lang == CBM_LANG_TYPESCRIPT && rl >= 5 &&
        strcmp(rel_path + rl - 5, ".d.ts") == 0) {
        *out_dts = true;
    }
}

static bool pxc_has_cross_lsp(CBMLanguage lang) {
    switch (lang) {
    case CBM_LANG_GO:
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA:
    case CBM_LANG_PYTHON:
    case CBM_LANG_JAVASCRIPT:
    case CBM_LANG_TYPESCRIPT:
    case CBM_LANG_TSX:
    case CBM_LANG_PHP:
        return true;
    default:
        return false;
    }
}

static bool pxc_already_resolved(const CBMResolvedCallArray *arr,
                                  const char *caller_qn, const char *callee_qn) {
    if (!arr || !caller_qn || !callee_qn) return false;
    for (int i = 0; i < arr->count; i++) {
        const CBMResolvedCall *rc = &arr->items[i];
        if (rc->caller_qn && rc->callee_qn &&
            strcmp(rc->caller_qn, caller_qn) == 0 &&
            strcmp(rc->callee_qn, callee_qn) == 0) {
            return true;
        }
    }
    return false;
}

static void pxc_append_results(CBMArena *dst_arena, CBMResolvedCallArray *dst_calls,
                                const CBMResolvedCallArray *src_out) {
    if (!dst_calls || !src_out) return;
    for (int j = 0; j < src_out->count; j++) {
        const CBMResolvedCall *src = &src_out->items[j];
        if (!src->caller_qn || !src->callee_qn) continue;
        if (pxc_already_resolved(dst_calls, src->caller_qn, src->callee_qn)) continue;
        CBMResolvedCall dst;
        memset(&dst, 0, sizeof(dst));
        dst.caller_qn = cbm_arena_strdup(dst_arena, src->caller_qn);
        dst.callee_qn = cbm_arena_strdup(dst_arena, src->callee_qn);
        dst.strategy = src->strategy ? cbm_arena_strdup(dst_arena, src->strategy) : NULL;
        dst.confidence = src->confidence;
        dst.reason = src->reason ? cbm_arena_strdup(dst_arena, src->reason) : NULL;
        cbm_resolvedcall_push(dst_calls, dst_arena, dst);
    }
}

static void pxc_run_one(CBMLanguage lang, CBMFileResult *r, const char *source,
                         int source_len, const char *module_qn,
                         CBMLSPDef *defs, int def_count,
                         const char **imp_names, const char **imp_qns, int imp_count) {
    TSTree *tree = r->cached_tree;

    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    switch (lang) {
    case CBM_LANG_GO:
        cbm_run_go_lsp_cross(&scratch, source, source_len, module_qn,
                              defs, def_count, imp_names, imp_qns, imp_count, tree,
                              &out);
        break;
    case CBM_LANG_C:
    case CBM_LANG_CPP:
    case CBM_LANG_CUDA: {
        bool cpp_mode = (lang != CBM_LANG_C);
        cbm_run_c_lsp_cross(&scratch, source, source_len, module_qn, cpp_mode,
                             defs, def_count, NULL, NULL, 0, tree,
                             &out);
        break;
    }
    case CBM_LANG_PYTHON:
        cbm_run_py_lsp_cross(&scratch, source, source_len, module_qn,
                              defs, def_count, imp_names, imp_qns, imp_count, tree,
                              &out);
        break;
    case CBM_LANG_PHP:
        cbm_run_php_lsp_cross(&scratch, source, source_len, module_qn,
                               defs, def_count, imp_names, imp_qns, imp_count, tree,
                               &out);
        break;
    default:
        break;
    }

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

static void pxc_run_one_ts(CBMFileResult *r, const char *source, int source_len,
                            const char *module_qn,
                            CBMLSPDef *defs, int def_count,
                            const char **imp_names, const char **imp_qns, int imp_count,
                            bool js_mode, bool jsx_mode, bool dts_mode) {
    CBMArena scratch;
    cbm_arena_init(&scratch);
    CBMResolvedCallArray out;
    memset(&out, 0, sizeof(out));

    cbm_run_ts_lsp_cross(&scratch, source, source_len, module_qn,
                          js_mode, jsx_mode, dts_mode,
                          defs, def_count, imp_names, imp_qns, imp_count,
                          r->cached_tree, &out);

    pxc_append_results(&r->arena, &r->resolved_calls, &out);
    cbm_arena_destroy(&scratch);
}

/* ── Parallel worker context ─────────────────────────────────────── */

typedef struct {
    /* read-only inputs */
    const cbm_file_info_t  *files;
    int                     file_count;
    CBMFileResult         **cache;
    CBMLSPDef              *all_defs;
    int                     def_count;
    const cbm_gbuf_t       *gbuf;
    const char             *project_name;
    char                  **def_modules; /* per-file, written once per slot */

    /* atomic counters */
    _Atomic int             processed;
    _Atomic int             skipped_no_lsp;
    _Atomic int             skipped_no_source;
    _Atomic int             per_lang_calls;

    /* watchdog */
    pxc_watchdog_ctx_t     *watchdog;
    _Atomic int             worker_slot_counter;
} pxc_parallel_ctx_t;

static void pxc_process_file(int i, void *vctx) {
    pxc_parallel_ctx_t *ctx = (pxc_parallel_ctx_t *)vctx;

    /* Assign a watchdog slot on first call from this thread */
    if (g_worker_slot < 0) {
        g_worker_slot = atomic_fetch_add_explicit(
            &ctx->worker_slot_counter, 1, memory_order_relaxed);
        if (g_worker_slot >= PXC_MAX_WORKERS) g_worker_slot = PXC_MAX_WORKERS - 1;
    }

    if (!ctx->cache[i]) return;
    CBMLanguage lang = ctx->files[i].language;

    if (!pxc_has_cross_lsp(lang)) {
        atomic_fetch_add_explicit(&ctx->skipped_no_lsp, 1, memory_order_relaxed);
        return;
    }

    int source_len = 0;
    char *source = pxc_read_file(ctx->files[i].path, &source_len);
    if (!source || source_len <= 0) {
        free(source);
        atomic_fetch_add_explicit(&ctx->skipped_no_source, 1, memory_order_relaxed);
        return;
    }

    /* def_modules[i] written at most once per slot (no contention for same i) */
    if (!ctx->def_modules[i]) {
        ctx->def_modules[i] = cbm_pipeline_fqn_module(ctx->project_name,
                                                       ctx->files[i].rel_path);
    }

    const char **imp_keys = NULL;
    const char **imp_vals = NULL;
    int imp_count = 0;
    pxc_build_import_map(ctx->gbuf, ctx->project_name, ctx->files[i].rel_path,
                          &imp_keys, &imp_vals, &imp_count);

    pxc_worker_begin(i);

    if (lang == CBM_LANG_JAVASCRIPT || lang == CBM_LANG_TYPESCRIPT ||
        lang == CBM_LANG_TSX) {
        bool js, jsx, dts;
        pxc_ts_modes(lang, ctx->files[i].rel_path, &js, &jsx, &dts);
        pxc_run_one_ts(ctx->cache[i], source, source_len, ctx->def_modules[i],
                        ctx->all_defs, ctx->def_count,
                        imp_keys, imp_vals, imp_count,
                        js, jsx, dts);
    } else {
        pxc_run_one(lang, ctx->cache[i], source, source_len, ctx->def_modules[i],
                     ctx->all_defs, ctx->def_count,
                     imp_keys, imp_vals, imp_count);
    }

    pxc_worker_end();

    pxc_free_import_map(imp_keys, imp_vals, imp_count);
    free(source);

    atomic_fetch_add_explicit(&ctx->per_lang_calls, 1, memory_order_relaxed);
    int done = atomic_fetch_add_explicit(&ctx->processed, 1, memory_order_relaxed) + 1;

    cbm_log_info("pass.lsp_cross.file.done",
                 "pos", itoa_buf(i),
                 "done", itoa_buf(done),
                 "total", itoa_buf(ctx->file_count),
                 "path", ctx->files[i].rel_path);
}

/* ── Public entry point ─────────────────────────────────────────── */

int cbm_pipeline_pass_lsp_cross(cbm_pipeline_ctx_t *ctx,
                                const cbm_file_info_t *files,
                                int file_count,
                                CBMFileResult **cache) {
    if (!ctx || !files || file_count <= 0 || !cache) return 0;

    cbm_log_info("pass.start", "pass", "lsp_cross", "files",
                 itoa_buf(file_count));

    char **def_modules = (char **)calloc((size_t)file_count, sizeof(char *));
    if (!def_modules) {
        cbm_log_error("pass.err", "pass", "lsp_cross", "phase", "alloc");
        return 0;
    }

    int def_count = 0;
    CBMLSPDef *all_defs = pxc_collect_all_defs(cache, files, file_count,
                                                ctx->project_name, def_modules,
                                                &def_count);

    /* Set up parallel context */
    pxc_parallel_ctx_t pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.files        = files;
    pctx.file_count   = file_count;
    pctx.cache        = cache;
    pctx.all_defs     = all_defs;
    pctx.def_count    = def_count;
    pctx.gbuf         = ctx->gbuf;
    pctx.project_name = ctx->project_name;
    pctx.def_modules  = def_modules;

    /* Set up watchdog */
    pxc_watchdog_ctx_t watchdog;
    memset(&watchdog, 0, sizeof(watchdog));
    watchdog.files      = files;
    watchdog.file_count = file_count;
    watchdog.processed  = &pctx.processed;
    watchdog.nslots     = PXC_MAX_WORKERS;
    pctx.watchdog       = &watchdog;
    g_watchdog_ctx      = &watchdog;

    cbm_thread_t watchdog_thread;
    int wd_started = cbm_thread_create(&watchdog_thread, 0,
                                        pxc_watchdog_thread, &watchdog);

    /* Dispatch parallel */
    cbm_parallel_for(file_count, pxc_process_file, &pctx,
                     (cbm_parallel_for_opts_t){.max_workers = 0});

    /* Stop watchdog */
    atomic_store_explicit(&watchdog.done, 1, memory_order_relaxed);
    if (wd_started == 0) cbm_thread_join(&watchdog_thread);
    g_watchdog_ctx = NULL;

    free(all_defs);
    for (int i = 0; i < file_count; i++) free(def_modules[i]);
    free(def_modules);

    cbm_log_info("pass.done", "pass", "lsp_cross",
                 "files_processed",    itoa_buf(atomic_load(&pctx.processed)),
                 "files_skipped_no_lsp",    itoa_buf(atomic_load(&pctx.skipped_no_lsp)),
                 "files_skipped_no_source", itoa_buf(atomic_load(&pctx.skipped_no_source)),
                 "defs_total",         itoa_buf(def_count),
                 "lsp_calls",          itoa_buf(atomic_load(&pctx.per_lang_calls)));
    return 0;
}
