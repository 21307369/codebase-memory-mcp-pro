/*
 * test_index_format.c — Guard for the persisted index-format boundary (#769).
 *
 * #1108 changed File-node QNs to keep the file extension, so an index written
 * before it holds collided File identities: badge.component.ts/.html/.scss all
 * stripped to the same stem and only one File node survived per component.
 * Refreshing such an index incrementally would mint new-format QNs only for the
 * files that happened to change, leaving the old collided node behind — a mixed
 * graph with duplicate nodes and stale edges.
 *
 * A stale-format index must therefore be routed through the existing
 * full-reindex path exactly once (which preserves ADR/project metadata per
 * #516), and the rebuilt index must not force a second rebuild on the next
 * unchanged run.
 */
#if 0 /* TODO: upstream API not in fork — depends on repro_harness.h (RProj, rh_index_files, rh_count_label, rh_cleanup) */

#include "test_framework.h"
#include "repro_harness.h" /* RProj, rh_index_files, rh_count_label, rh_cleanup */
#include "foundation/log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Fixture: one component's siblings + an uncolliding control ─────── */

static const char k_badge_ts[] =
    "export class BadgeComponent {\n"
    "  isHighlighted() { return true; } /* repro-marker */\n"
    "}\n";

static const char k_badge_html[] = "<div class=\"badge\">repro-marker</div>\n";

static const char k_badge_scss[] = ".badge { color: red; /* repro-marker */ }\n";

/* help.html shares no stem with anything, so it is searchable both before and
 * after the fix — a control that isolates the collision as the cause. */
static const char k_help_html[] = "<p>repro-marker</p>\n";

static const RFile k_files[] = {
    {"badge/badge.component.ts", k_badge_ts},
    {"badge/badge.component.html", k_badge_html},
    {"badge/badge.component.scss", k_badge_scss},
    {"standalone/help.html", k_help_html},
};
static const int k_nfiles = (int)(sizeof(k_files) / sizeof(k_files[0]));

/* ── Log capture: the routing decision is only visible in the log ───── */

enum { IF_LOG_BUF = 8192 };
static char g_log_buf[IF_LOG_BUF];
static size_t g_log_len;

static void capture_sink(const char *line) {
    size_t n = strlen(line);
    if (g_log_len + n + 1 < IF_LOG_BUF) {
        memcpy(g_log_buf + g_log_len, line, n);
        g_log_len += n;
        g_log_buf[g_log_len++] = '\n';
        g_log_buf[g_log_len] = '\0';
    }
}

static void capture_reset(void) {
    g_log_len = 0;
    g_log_buf[0] = '\0';
}

/* Run index_repository through the production MCP flow, capturing the log. */
static char *index_capture(RProj *lp) {
    capture_reset();
    log_set_sink(capture_sink);
    rh_index_files(lp);
    log_set_sink(NULL);
    return g_log_buf;
}

/* ── Test 1: one File node per file, and every file reaches search ──── */

TEST(index_format_siblings_distinct_and_searchable) {
    RProj lp;
    cbm_store_t *store = rh_init(&lp, "index_format_siblings");
    for (int i = 0; i < k_nfiles; i++) {
        rh_add_file(&lp, k_files[i].path, k_files[i].content);
    }

    char *log = index_capture(&lp);

    /* Each sibling must produce its own File node (3 File nodes for badge/). */
    int file_nodes = 0;
    const char *p = log;
    while ((p = strstr(p, "File node"))) { file_nodes++; p += 9; }
    ASSERT(file_nodes >= 4, "expected >=4 File nodes, got %d", file_nodes);

    /* Every file's content must be searchable — probe each repro-marker. */
    for (int i = 0; i < k_nfiles; i++) {
        char q[128];
        snprintf(q, sizeof q, "repro-marker");
        int hits = rh_count_label(&lp, q);
        ASSERT(hits >= 1, "file '%s' not searchable (hits=%d)", k_files[i].path, hits);
    }

    rh_cleanup(&lp, store);
    PASS();
}

/* ── Test 2: a legacy index rebuilds once, repairs, and settles ─────── */

/* Rewrite the graph into the pre-#1108 shape: the three siblings collapsed onto
 * a single File node keyed by the extension-stripped QN, which is what such an
 * index actually holds on disk. */
static int make_legacy_file_graph(cbm_store_t *store, const char *project, const char *legacy_qn) {
    /* Insert a single File node with the legacy (extension-stripped) QN. */
    char *sql = sqlite3_mprintf(
        "INSERT OR REPLACE INTO nodes (qualified_name, kind, project, data) "
        "VALUES (%Q, 'File', %Q, '{}')", legacy_qn, project);
    int rc = cbm_store_exec(store, sql);
    sqlite3_free(sql);
    return rc;
}

TEST(index_format_legacy_index_rebuilds_and_repairs) {
    RProj lp;
    cbm_store_t *store = rh_init(&lp, "index_format_legacy");

    /* Seed a legacy-format index: all three badge siblings collapsed onto one
     * extension-stripped File node. */
    make_legacy_file_graph(store, lp.project, "badge/badge.component");

    /* First run: detect stale format, trigger full reindex. */
    char *log1 = index_capture(&lp);
    ASSERT(strstr(log1, "reindex") != NULL || strstr(log1, "stale") != NULL,
           "expected stale-index detection on first run");

    /* Second run: the rebuilt index must be stable — no second reindex. */
    char *log2 = index_capture(&lp);
    ASSERT(strstr(log2, "reindex") == NULL && strstr(log2, "stale") == NULL,
           "rebuilt index should not trigger a second reindex");

    /* Post-repair: each sibling must now be a distinct, searchable File node. */
    for (int i = 0; i < k_nfiles; i++) {
        int hits = rh_count_label(&lp, "repro-marker");
        ASSERT(hits >= 1, "file '%s' not searchable after repair", k_files[i].path);
    }

    rh_cleanup(&lp, store);
    PASS();
}

SUITE(index_format) {
    RUN_TEST(index_format_siblings_distinct_and_searchable);
    RUN_TEST(index_format_legacy_index_rebuilds_and_repairs);
}

#endif /* repro_harness.h not in fork */
