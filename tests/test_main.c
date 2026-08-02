/*
 * test_main.c — Test runner entry point for pure C rewrite.
 *
 * Includes all test suites and runs them sequentially.
 */
/* Global test counters (declared extern in test_framework.h) */
int tf_pass_count = 0;
int tf_fail_count = 0;
int tf_skip_count = 0;

#include "test_framework.h"
<<<<<<< ours
#include <sqlite3.h>
=======
#include "test_helpers.h"
#include "foundation/compat.h"    /* cbm_setenv — #845 supervisor kill switch */
#include "foundation/compat_fs.h" /* cbm_fopen — worker response file */
#include "foundation/mem.h"       /* cbm_mem_init — worker budget */
#include "mcp/index_supervisor.h" /* cbm_index_set_worker_role */
#include "mcp/mcp.h"              /* cbm_mcp_handle_tool — act as a real worker */
#include "ui/http_server.h"       /* deleted-self executable probe */
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h> /* #798 follow-up: socket-isolation re-exec probe */
#else
#include <unistd.h>
#endif

/* Test handlers that exercise the production index_repository flow must never
 * inherit the user's real cache. Individual tests may temporarily override
 * this sentinel and restore it; atexit removes anything left behind by an
 * assertion that returns before fixture cleanup. */
static char tf_home_sentinel[512];

static void tf_cleanup_cache_sentinel(void) {
    if (tf_home_sentinel[0]) {
        th_rmtree(tf_home_sentinel);
    }
}

static bool tf_setup_cache_sentinel(void) {
    snprintf(tf_home_sentinel, sizeof(tf_home_sentinel), "/tmp/cbm-test-home-XXXXXX");
    if (!cbm_mkdtemp(tf_home_sentinel)) {
        return false;
    }
    /* Legacy integration fixtures derive DB paths from HOME, while production
     * cache_dir() prefers CBM_CACHE_DIR. A private HOME plus no inherited cache
     * override keeps both conventions pointed at the same isolated tree. */
    cbm_setenv("HOME", tf_home_sentinel, 1);
    cbm_unsetenv("CBM_CACHE_DIR");
    atexit(tf_cleanup_cache_sentinel);
    return true;
}

/* #832 guard support: when the index supervisor spawns THIS binary as
 * `<self> cli --index-worker index_repository <args_json> --response-out <file>`
 * (exactly the argv cbm_index_spawn_worker builds), act as a faithful in-process
 * index worker instead of re-running the test suites. This lets the deterministic
 * gating guard (test_mcp.c) spawn a REAL worker child that indexes the fixture and
 * writes its response back, using only public APIs — no production test seam.
 * Returns an exit code (>=0) when it handled a worker invocation, else -1. */
static int tf_maybe_run_index_worker(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "cli") != 0) {
        return -1;
    }
    bool is_worker = false;
    const char *tool = NULL;
    const char *args_json = "{}";
    const char *response_out = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--index-worker") == 0) {
            is_worker = true;
        } else if (strcmp(argv[i], "--response-out") == 0) {
            if (i + 1 < argc) {
                response_out = argv[++i];
            }
        } else if (argv[i][0] == '{') {
            args_json = argv[i];
        } else if (argv[i][0] != '-' && !tool) {
            tool = argv[i];
        }
    }
    if (!is_worker) {
        return -1;
    }
    if (!tool) {
        tool = "index_repository";
    }

    cbm_mem_init(0.5);
    cbm_index_set_worker_role(true, response_out); /* worker role → index in-process */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        return 1;
    }
    char *result = cbm_mcp_handle_tool(srv, tool, args_json);
    if (result) {
        const char *ro = cbm_index_worker_response_out();
        if (ro) {
            FILE *rf = cbm_fopen(ro, "wb");
            if (rf) {
                (void)fputs(result, rf);
                (void)fclose(rf);
            }
        }
    }
    /* Faithful worker exit: mirror run_cli's supervised-worker fast path.
     * The worker-role pipeline deliberately skips its teardown (the OS
     * reclaims everything wholesale on process death), so a normal return
     * through main() lets LeakSanitizer run at exit, report the
     * intentionally-unfreed pipeline, and force exit code 1 — the
     * supervisor then reads a HEALTHY index as worker_failed (the
     * Linux-only IDX832 red: LSan is active in Linux gcc ASan builds,
     * absent on macOS/Windows). _Exit skips atexit/LSan by design,
     * exactly like the production worker in run_cli. */
    fflush(NULL);
    _Exit(result ? 0 : 1);
}

/* #798 follow-up: socket-isolation probe. The parent test
 * (popen_isolates_listening_socket, test_security.c) spawns THIS binary through
 * cbm_popen — the same cmd.exe-grandchild path git takes — passing the numeric
 * value of an inheritable listening-socket handle. If cbm_popen correctly
 * isolates handles, that socket is NOT present in this child and getsockopt
 * fails; a regression to raw _popen leaks it (bInheritHandles=TRUE propagates it
 * transitively through cmd.exe) and getsockopt succeeds. We report via exit code
 * so the verdict survives `cmd.exe /c` (proven by popen_isolated_propagates_exit_code).
 * Returns an exit code (>=0) when it handled a probe invocation, else -1. */
static int tf_maybe_run_socket_probe(int argc, char **argv) {
#ifdef _WIN32
    if (argc < 3 || strcmp(argv[1], "__cbm_sockprobe") != 0) {
        return -1;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0; /* no winsock in child ⇒ cannot observe a socket ⇒ not leaked */
    }
    unsigned long long hv = strtoull(argv[2], NULL, 10);
    SOCKET s = (SOCKET)(uintptr_t)hv;
    int type = 0;
    int len = (int)sizeof(type);
    int rc = getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len);
    /* rc==0 ⇒ the handle is a live socket in THIS child ⇒ it was inherited. */
    return rc == 0 ? 42 : 0;
#else
    (void)argc;
    (void)argv;
    return -1;
#endif
}

static int tf_maybe_run_deleted_self_probe(int argc, char **argv) {
#if defined(__linux__)
    if (argc != 5 || strcmp(argv[1], "__cbm_deleted_self_probe") != 0) {
        return -1;
    }
    int ready_fd = atoi(argv[2]);
    int continue_fd = atoi(argv[3]);
    cbm_http_server_set_binary_path(argv[4]);
    if (write(ready_fd, "R", 1) != 1) {
        return 41;
    }
    char go = '\0';
    if (read(continue_fd, &go, 1) != 1) {
        return 42;
    }
    char resolved[1024];
    if (!cbm_http_server_resolve_binary_path(NULL, resolved, sizeof(resolved))) {
        return 43;
    }
    return strcmp(resolved, argv[4]) == 0 && access(resolved, X_OK) == 0 ? 0 : 44;
#else
    (void)argc;
    (void)argv;
    return -1;
#endif
}

static int g_suite_argc = 0;
static char **g_suite_argv = NULL;
static bool *g_suite_arg_matched = NULL;

static bool suite_requested(const char *name) {
    if (g_suite_argc <= 1) {
        return true;
    }
    bool requested = false;
    for (int i = 1; i < g_suite_argc; i++) {
        if (strcmp(g_suite_argv[i], name) == 0) {
            g_suite_arg_matched[i] = true;
            requested = true;
        }
    }
    return requested;
}

/* --list-suites: print every registered suite name, one per line, without
 * running anything. The list and the run share this ONE macro table, so the
 * list can never drift from what actually executes — shard runners (make
 * test-par, the CI shard matrix) enumerate suites from here and their union
 * guard compares against it. */
static bool g_list_only = false;

#define RUN_SELECTED_SUITE(name)             \
    do {                                     \
        if (g_list_only) {                   \
            printf("%s\n", #name);           \
        } else if (suite_requested(#name)) { \
            RUN_SUITE(name);                 \
        }                                    \
    } while (0)
>>>>>>> theirs

/* Forward declarations of suite functions */
extern void suite_arena(void);
extern void suite_hash_table(void);
extern void suite_dyn_array(void);
extern void suite_str_intern(void);
extern void suite_log(void);
extern void suite_str_util(void);
extern void suite_platform(void);
extern void suite_extraction(void);
extern void suite_extraction_inheritance(void);
extern void suite_extraction_imports(void);
extern void suite_grammar_regression(void);
extern void suite_grammar_labels(void);
extern void suite_grammar_imports(void);
extern void suite_ac(void);
extern void suite_store_nodes(void);
extern void suite_store_edges(void);
extern void suite_store_search(void);
extern void suite_cypher(void);
extern void suite_mcp(void);
extern void suite_language(void);
extern void suite_userconfig(void);
extern void suite_gitignore(void);
extern void suite_discover(void);
extern void suite_graph_buffer(void);
extern void suite_registry(void);
extern void suite_pipeline(void);

    RUN_SUITE(index_format);
extern void suite_fqn(void);
extern void suite_path_alias(void);
extern void suite_watcher(void);
extern void suite_lz4(void);
extern void suite_zstd(void);
extern void suite_artifact(void);
extern void suite_sqlite_writer(void);
extern void suite_go_lsp(void);
extern void suite_c_lsp(void);
extern void suite_php_lsp(void);
extern void suite_cs_lsp(void);
extern void suite_cs_lsp_bench(void);
extern void suite_scope(void);
extern void suite_type_rep(void);
extern void suite_py_lsp(void);
extern void suite_py_lsp_bench(void);
extern void suite_py_lsp_stress(void);
extern void suite_py_lsp_scale(void);
extern void suite_ts_lsp(void);
extern void suite_java_lsp(void);
extern void suite_java_lsp_coverage(void);
extern void suite_kotlin_lsp(void);
extern void suite_rust_lsp(void);
extern void suite_store_arch(void);
extern void suite_store_bulk(void);
extern void suite_store_pragmas(void);
extern void suite_store_checkpoint(void);
extern void suite_traces(void);
extern void suite_configlink(void);
extern void suite_infrascan(void);
extern void suite_cli(void);
extern void suite_system_info(void);
extern void suite_worker_pool(void);
extern void suite_parallel(void);
extern void suite_mem(void);
extern void suite_ui(void);
extern void suite_httpd(void);
extern void suite_security(void);
extern void suite_yaml(void);
extern void suite_integration(void);
extern void suite_lang_contract(void);
extern void suite_edge_imports(void);
extern void suite_edge_structural(void);
extern void suite_lsp_resolution_probe(void);
extern void suite_node_creation_probe(void);
extern void suite_edge_types_probe(void);
extern void suite_convergence_probe(void);
extern void suite_matrix_known_classes(void);
extern void suite_matrix_new_constructs(void);
extern void suite_grammar_probe_a(void);
extern void suite_grammar_probe_b(void);
extern void suite_grammar_probe_c(void);
extern void suite_grammar_probe_d(void);
extern void suite_grammar_probe_e(void);
extern void suite_grammar_probe_f(void);
extern void suite_grammar_probe_g(void);
extern void suite_incremental(void);
extern void suite_simhash(void);
extern void suite_stack_overflow(void);

/* Free the main thread's thread-local node-type bitset cache before exit so
 * LeakSanitizer (Linux x64) doesn't report it. Worker threads free their own
 * caches at thread teardown (pass_parallel.c). */
extern void cbm_kind_in_set_free_cache(void);

<<<<<<< ours
int main(void) {
    printf("\n  codebase-memory-mcp  C test suite\n");
=======
int main(int argc, char **argv) {
    int deleted_self_rc = tf_maybe_run_deleted_self_probe(argc, argv);
    if (deleted_self_rc >= 0) {
        return deleted_self_rc;
    }

    /* #798 follow-up: if spawned as the socket-isolation probe, report whether an
     * inheritable socket handle crossed into this child and exit before any suite. */
    int probe_rc = tf_maybe_run_socket_probe(argc, argv);
    if (probe_rc >= 0) {
        return probe_rc;
    }

    /* #832: if spawned as a supervised index worker, do the real work and exit
     * before any suite runs (see tf_maybe_run_index_worker). */
    int worker_rc = tf_maybe_run_index_worker(argc, argv);
    if (worker_rc >= 0) {
        return worker_rc;
    }

    /* #845 belt-and-suspenders: this binary EMBEDS cbm_mcp_handle_tool. The
     * supervisor gate already ignores unmarked hosts, but pin the kill switch
     * too so even a future supervisor-marked test host can never resolve THIS
     * binary as `<self> cli --index-worker …` and recursively re-run suites.
     * A test that exercises the supervisor must explicitly re-enable it. */
    cbm_setenv("CBM_INDEX_SUPERVISOR", "0", 1);
    if (!tf_setup_cache_sentinel()) {
        fprintf(stderr, "failed to create isolated test cache\n");
        return 2;
    }

    if (argc == 2 && strcmp(argv[1], "--list-suites") == 0) {
        g_list_only = true;
        g_suite_argc = 1; /* no suite-name args to match */
    } else {
        g_suite_argc = argc;
        g_suite_argv = argv;
    }
    if (g_suite_argc > 1) {
        g_suite_arg_matched = calloc((size_t)argc, sizeof(*g_suite_arg_matched));
        if (!g_suite_arg_matched) {
            fprintf(stderr, "Failed to allocate test-suite argument tracking\n");
            return 1;
        }
    }
    if (!g_list_only) {
        printf("\n  codebase-memory-mcp  C test suite\n");
    }
>>>>>>> theirs

    /* Foundation */
    RUN_SUITE(arena);
    RUN_SUITE(hash_table);
    RUN_SUITE(dyn_array);
    RUN_SUITE(str_intern);
    RUN_SUITE(log);
    RUN_SUITE(str_util);
    RUN_SUITE(platform);

    /* Existing C code regression tests */
    RUN_SUITE(ac);
    RUN_SUITE(extraction);
    RUN_SUITE(extraction_inheritance);
    RUN_SUITE(extraction_imports);
    RUN_SUITE(grammar_regression);
    RUN_SUITE(grammar_labels);
    RUN_SUITE(grammar_imports);

    /* Store (M5) */
    RUN_SUITE(store_nodes);
    RUN_SUITE(store_edges);
    RUN_SUITE(store_search);
    RUN_SUITE(store_bulk);
    RUN_SUITE(store_pragmas);
    RUN_SUITE(store_checkpoint);

    /* Cypher (M6) */
    RUN_SUITE(cypher);

    /* MCP Server (M9) */
    RUN_SUITE(mcp);

    /* Discover (M2) */
    RUN_SUITE(language);
    RUN_SUITE(userconfig);
    RUN_SUITE(gitignore);
    RUN_SUITE(discover);

    /* Graph Buffer (M7) */
    RUN_SUITE(graph_buffer);

    /* Pipeline (M8) */

    RUN_SUITE(index_format);

    /* Watcher (M10) */
    RUN_SUITE(watcher);

    /* LZ4 + zstd + SQLite writer */
    RUN_SUITE(lz4);
    RUN_SUITE(zstd);
    RUN_SUITE(sqlite_writer);

    /* Persistent artifact export/import */
    RUN_SUITE(artifact);

    /* LSP resolvers */
    RUN_SUITE(scope);
    RUN_SUITE(type_rep);
    RUN_SUITE(go_lsp);
    RUN_SUITE(c_lsp);
    RUN_SUITE(php_lsp);
    RUN_SUITE(cs_lsp);
    RUN_SUITE(cs_lsp_bench);
    RUN_SUITE(py_lsp);
    RUN_SUITE(kotlin_lsp);
    RUN_SUITE(rust_lsp);
    RUN_SUITE(py_lsp_bench);
    RUN_SUITE(py_lsp_stress);
    RUN_SUITE(py_lsp_scale);
    RUN_SUITE(ts_lsp);
    RUN_SUITE(java_lsp);
    RUN_SUITE(java_lsp_coverage);

    /* Architecture + ADR + Louvain */
    RUN_SUITE(store_arch);

    /* HTTP link */

    /* Traces helpers */
    RUN_SUITE(traces);

    /* Config link */
    RUN_SUITE(configlink);

    /* Infrastructure scanning */
    RUN_SUITE(infrascan);

    /* CLI (install, update, config) */
    RUN_SUITE(cli);

    /* System info + worker pool (parallelism) */
    RUN_SUITE(system_info);
    RUN_SUITE(worker_pool);

    /* Parallel pipeline */
    RUN_SUITE(parallel);

    /* mem + arena + slab integration */
    RUN_SUITE(mem);

    /* UI (config, embedded assets, layout) */
    RUN_SUITE(ui);

    /* UI HTTP server (transport + routing) */
    RUN_SUITE(httpd);

    /* Security defenses */
    RUN_SUITE(security);

    /* YAML parser */
    RUN_SUITE(yaml);

    /* SimHash / SIMILAR_TO */
    RUN_SUITE(simhash);

    /* Stack overflow regression (GitHub #199) */
    RUN_SUITE(stack_overflow);

    /* Integration (end-to-end) */
    RUN_SUITE(integration);

    /* Per-language graph contracts (node/edge types, attribution, no-crash) */
    RUN_SUITE(lang_contract);
    RUN_SUITE(edge_imports);
    RUN_SUITE(edge_structural);
    RUN_SUITE(lsp_resolution_probe);
    RUN_SUITE(node_creation_probe);
    RUN_SUITE(edge_types_probe);
    RUN_SUITE(convergence_probe);
    RUN_SUITE(matrix_known_classes);
    RUN_SUITE(matrix_new_constructs);
    RUN_SUITE(grammar_probe_a);
    RUN_SUITE(grammar_probe_b);
    RUN_SUITE(grammar_probe_c);
    RUN_SUITE(grammar_probe_d);
    RUN_SUITE(grammar_probe_e);
    RUN_SUITE(grammar_probe_f);
    RUN_SUITE(grammar_probe_g);

    RUN_SUITE(incremental);

    /* Release process-lifetime caches so LeakSanitizer reports no leaks. */
    cbm_kind_in_set_free_cache();
    sqlite3_shutdown();
    TEST_SUMMARY();
}
