/*
 * pass_param_reads.c — PARAM_READ edge generation from whitelist.
 *
 * For each field name in userconfig->param_whitelist:
 *   1. Upsert a virtual ConfigParam node (label="ConfigParam")
 *   2. Scan every indexed source file for occurrences of the field name
 *   3. For each match line, find the enclosing Function/Method node in gbuf
 *   4. Insert a PARAM_READ edge: Function -> ConfigParam
 *
 * No AST re-parsing — operates on already-loaded gbuf nodes + raw source.
 * Runs after pass_definitions so Function/Method nodes are already present.
 *
 * Edge type: PARAM_READ
 * Node label: ConfigParam
 * QN format:  {project}.__param__{field_name}
 */
#include "foundation/constants.h"
#include "foundation/compat.h"
#include "pipeline/pipeline_internal.h"
#include "discover/userconfig.h"
#include "graph_buffer/graph_buffer.h"
#include "foundation/log.h"
#include "foundation/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────── */

enum {
    PR_LINE_BUF   = 4096,   /* max line length to scan */
    PR_MAX_HITS   = 256,    /* max hit lines per file per field */
    PR_PROPS_BUF  = 256,    /* edge properties JSON buffer */
    PR_QN_BUF     = 512,    /* qualified name buffer */
    PR_ITOA_BUFS  = 4,
    PR_ITOA_SZ    = 32,
};

/* Thread-local int-to-string for log calls. */
static const char *itoa_log(int val) {
    static CBM_TLS char bufs[PR_ITOA_BUFS][PR_ITOA_SZ];
    static CBM_TLS int  idx = 0;
    int i = idx;
    idx = (idx + 1) & (PR_ITOA_BUFS - 1);
    snprintf(bufs[i], PR_ITOA_SZ, "%d", val);
    return bufs[i];
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* Build ConfigParam QN: {project}.__param__{field_name} */
static void make_param_qn(const char *project, const char *field,
                           char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s.__param__%s", project, field);
}

/* Scan source file for lines containing `field`. Writes matching line
 * numbers to `hits` (up to max_hits). Returns count. */
static int scan_file_for_field(const char *abs_path, const char *field,
                                int *hits, int max_hits) {
    FILE *f = fopen(abs_path, "r");
    if (!f) return 0;

    int count = 0;
    int lineno = 1;
    char buf[PR_LINE_BUF];

    while (count < max_hits && fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, field)) {
            hits[count++] = lineno;
        }
        lineno++;
    }
    fclose(f);
    return count;
}

/* Context for finding the enclosing function at a given file+line. */
typedef struct {
    const char *file_path; /* relative path to match */
    int target_line;
    int64_t best_id;
    int best_start;        /* start_line of best candidate */
} find_fn_ctx_t;

static void find_fn_visitor(const cbm_gbuf_node_t *node, void *userdata) {
    find_fn_ctx_t *ctx = (find_fn_ctx_t *)userdata;

    /* Only Function or Method nodes */
    if (!node->label) return;
    if (strcmp(node->label, "Function") != 0 &&
        strcmp(node->label, "Method")   != 0) return;

    /* Must be in the same file */
    if (!node->file_path) return;
    if (strcmp(node->file_path, ctx->file_path) != 0) return;

    /* Target line must fall within [start_line, end_line] */
    if (ctx->target_line < node->start_line) return;
    if (node->end_line > 0 && ctx->target_line > node->end_line) return;

    /* Among overlapping candidates, prefer the innermost (largest start_line) */
    if (node->start_line > ctx->best_start) {
        ctx->best_start = node->start_line;
        ctx->best_id    = node->id;
    }
}

/* Find the Function/Method node in gbuf that encloses (file_path, line). */
static int64_t find_enclosing_function(const cbm_gbuf_t *gbuf,
                                       const char *rel_path, int line) {
    find_fn_ctx_t ctx = {
        .file_path   = rel_path,
        .target_line = line,
        .best_id     = 0,
        .best_start  = -1,
    };
    cbm_gbuf_foreach_node(gbuf, find_fn_visitor, &ctx);
    return ctx.best_id;
}

/* ── Pass entry point ───────────────────────────────────────────── */

int cbm_pipeline_pass_param_reads(cbm_pipeline_ctx_t *ctx,
                                   const cbm_file_info_t *files, int file_count,
                                   const cbm_userconfig_t *ucfg) {
    if (!ucfg || ucfg->param_whitelist_count == 0) {
        return 0; /* nothing configured — fast exit */
    }

    const char *project = ctx->project_name;
    cbm_gbuf_t *gbuf    = ctx->gbuf;
    int total_edges     = 0;

    cbm_log_info("pass.start", "pass", "param_reads",
                 "fields", itoa_log(ucfg->param_whitelist_count));

    for (int fi = 0; fi < ucfg->param_whitelist_count; fi++) {
        const char *field = ucfg->param_whitelist[fi];
        if (!field || !field[0]) continue;

        /* 1. Upsert virtual ConfigParam node */
        char param_qn[PR_QN_BUF];
        make_param_qn(project, field, param_qn, sizeof(param_qn));

        char props[PR_PROPS_BUF];
        snprintf(props, sizeof(props),
                 "{\"source\":\"whitelist\",\"field\":\"%s\"}", field);

        int64_t param_id = cbm_gbuf_upsert_node(gbuf, "ConfigParam",
                                                  field, param_qn,
                                                  NULL, 0, 0, props);
        if (!param_id) {
            cbm_log_warn("param_reads.skip_node", "field", field);
            continue;
        }

        int field_edges = 0;

        /* 2. Scan each file */
        for (int i = 0; i < file_count; i++) {
            if (cbm_pipeline_check_cancel(ctx)) return 0;

            const cbm_file_info_t *finfo = &files[i];
            if (!finfo->path || !finfo->rel_path) continue;

            int hits[PR_MAX_HITS];
            int nhits = scan_file_for_field(finfo->path, field,
                                             hits, PR_MAX_HITS);
            if (nhits == 0) continue;

            for (int h = 0; h < nhits; h++) {
                int line = hits[h];

                /* 3. Find enclosing Function/Method */
                int64_t func_id = find_enclosing_function(gbuf,
                                                           finfo->rel_path,
                                                           line);
                if (!func_id) continue;

                /* 4. Insert PARAM_READ edge: Function -> ConfigParam */
                char eprops[PR_PROPS_BUF];
                snprintf(eprops, sizeof(eprops),
                         "{\"line\":%d,\"file\":\"%s\"}", line, finfo->rel_path);

                int64_t eid = cbm_gbuf_insert_edge(gbuf, func_id, param_id,
                                                    "PARAM_READ", eprops);
                if (eid) field_edges++;
            }
        }

        cbm_log_info("param_reads.field_done", "field", field,
                     "edges", itoa_log(field_edges));
        total_edges += field_edges;
    }

    cbm_log_info("pass.done", "pass", "param_reads",
                 "edges", itoa_log(total_edges));
    return 0;
}
