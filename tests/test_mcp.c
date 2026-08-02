/*
 * test_mcp.c — Tests for the MCP server module.
 *
 * Covers: JSON-RPC parsing, MCP protocol, tool dispatch, tool handlers.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h" /* cbm_unlink / cbm_rmdir */
#include "test_framework.h"
#include <mcp/mcp.h>
#include <pipeline/pipeline.h>
#include <semantic/semantic.h>
#include <store/store.h>
#include <yyjson/yyjson.h>
#include <string.h>
#include <sys/stat.h> /* chmod / stat for read-only query reproductions */
#ifdef _WIN32
#include <direct.h>
#define cbm_chdir _chdir
#define cbm_getcwd _getcwd
#else
#include <sys/wait.h> /* waitpid — #845 fork+alarm harness */
#include <unistd.h>
#define cbm_chdir chdir
#define cbm_getcwd getcwd
#endif

static bool mcp_response_has_exact_tool(const char *response, const char *expected_name) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    bool found = false;
    if (tools && yyjson_is_arr(tools)) {
        size_t index, max;
        yyjson_val *tool;
        yyjson_arr_foreach(tools, index, max, tool) {
            yyjson_val *name = yyjson_obj_get(tool, "name");
            if (name && yyjson_is_str(name) && strcmp(yyjson_get_str(name), expected_name) == 0) {
                found = true;
                break;
            }
        }
    }
    yyjson_doc_free(doc);
    return found;
}

static size_t mcp_response_tool_count(const char *response) {
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
    yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
    size_t count = tools && yyjson_is_arr(tools) ? yyjson_arr_size(tools) : 0U;
    yyjson_doc_free(doc);
    return count;
}

static char mcp_log_buf[4096];
static bool mcp_saw_autoindex_log;

static void mcp_capture_log(const char *line) {
    snprintf(mcp_log_buf, sizeof(mcp_log_buf), "%s", line ? line : "");
    if (line && strstr(line, "msg=autoindex.")) {
        mcp_saw_autoindex_log = true;
    }
}

static int semantic915_fetch_limit = -1;

static void semantic915_capture_vector_log(const char *line) {
    if (!line || !strstr(line, "msg=vector_search.exec")) {
        return;
    }
    const char *value = strstr(line, "fetch_limit=");
    if (value) {
        semantic915_fetch_limit = atoi(value + strlen("fetch_limit="));
    }
}

static bool response_contains_json_fragment(const char *response, const char *fragment) {
    if (!response || !fragment) {
        return false;
    }
    if (strstr(response, fragment)) {
        return true;
    }

    char escaped[512];
    size_t out = 0;
    for (size_t i = 0; fragment[i] && out + 2 < sizeof(escaped); i++) {
        if (fragment[i] == '"') {
            escaped[out++] = '\\';
        }
        escaped[out++] = fragment[i];
    }
    escaped[out] = '\0';
    return strstr(response, escaped) != NULL;
}

static void restore_cache_dir(const char *saved_copy) {
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
}

static void cleanup_project_db(const char *cache, const char *project) {
    if (!cache || !project) {
        return;
    }

    char path[CBM_SZ_4K];
    snprintf(path, sizeof(path), "%s/%s.db", cache, project);
    cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/%s.db-wal", cache, project);
    cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/%s.db-shm", cache, project);
    cbm_unlink(path);
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_request) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"capabilities\":{}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_EQ(req.id, 1);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_notification) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "notifications/initialized");
    ASSERT_FALSE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_invalid) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("not json", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_tools_call) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"search_graph\","
                       "\"arguments\":{\"label\":\"Function\",\"limit\":5}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "tools/call");
    ASSERT_EQ(req.id, 42);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* issue #253: JSON-RPC 2.0 §4 permits string ids (Claude Desktop sends them
 * for "initialize"). Previously strtol-coerced to 0; must be preserved. */
TEST(jsonrpc_parse_string_id_issue253) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"init-abc\",\"method\":\"initialize\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "init-abc");
    cbm_jsonrpc_request_free(&req);

    /* A purely non-numeric string would have become 0 under strtol. */
    const char *line2 = "{\"jsonrpc\":\"2.0\",\"id\":\"xyz\",\"method\":\"ping\"}";
    cbm_jsonrpc_request_t req2 = {0};
    ASSERT_EQ(cbm_jsonrpc_parse(line2, &req2), 0);
    ASSERT_NOT_NULL(req2.id_str);
    ASSERT_STR_EQ(req2.id_str, "xyz");
    cbm_jsonrpc_request_free(&req2);
    PASS();
}

/* issue #253: the response must echo the string id verbatim, not as a number. */
TEST(jsonrpc_format_response_string_id_issue253) {
    cbm_jsonrpc_response_t resp = {
        .id_str = "init-abc",
        .result_json = "{\"ok\":true}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":\"init-abc\""));
    /* Must NOT have coerced to a numeric id. */
    ASSERT_NULL(strstr(json, "\"id\":0"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_format_response) {
    cbm_jsonrpc_response_t resp = {
        .id = 1,
        .result_json = "{\"name\":\"codebase-memory-mcp\"}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    /* Should contain jsonrpc, id, and result */
    ASSERT_NOT_NULL(strstr(json, "\"jsonrpc\":\"2.0\""));
    ASSERT_NOT_NULL(strstr(json, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(json, "\"result\""));
    free(json);
    PASS();
}

TEST(jsonrpc_format_error) {
    char *json = cbm_jsonrpc_format_error(5, -32600, "Invalid Request");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":5"));
    ASSERT_NOT_NULL(strstr(json, "\"error\""));
    ASSERT_NOT_NULL(strstr(json, "-32600"));
    ASSERT_NOT_NULL(strstr(json, "Invalid Request"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_initialize_response) {
    /* Default (no params): returns latest supported version */
    char *json = cbm_mcp_initialize_response(NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(json, "capabilities"));
    ASSERT_NOT_NULL(strstr(json, "tools"));
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);

    /* Client requests a supported version: server echoes it */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2024-11-05"));
    free(json);

    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2025-06-18\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-06-18"));
    free(json);

    /* Client requests unknown version: server returns its latest */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"9999-01-01\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);
    PASS();
}

TEST(mcp_tools_list) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    /* Should contain all 14 tools */
    ASSERT_NOT_NULL(strstr(json, "index_repository"));
    ASSERT_NOT_NULL(strstr(json, "search_graph"));
    ASSERT_NOT_NULL(strstr(json, "query_graph"));
    ASSERT_NOT_NULL(strstr(json, "trace_path"));
    ASSERT_NOT_NULL(strstr(json, "get_code_snippet"));
    ASSERT_NOT_NULL(strstr(json, "get_graph_schema"));
    ASSERT_NOT_NULL(strstr(json, "get_architecture"));
    ASSERT_NOT_NULL(strstr(json, "search_code"));
    ASSERT_NOT_NULL(strstr(json, "list_projects"));
    ASSERT_NOT_NULL(strstr(json, "delete_project"));
    ASSERT_NOT_NULL(strstr(json, "index_status"));
    ASSERT_NOT_NULL(strstr(json, "detect_changes"));
    ASSERT_NOT_NULL(strstr(json, "manage_adr"));
    ASSERT_NOT_NULL(strstr(json, "ingest_traces"));
    free(json);
    PASS();
}

TEST(mcp_tools_array_schemas_have_items) {
    /* VS Code 1.112+ rejects array schemas without "items" (see
     * https://github.com/microsoft/vscode/issues/248810).
     * Walk every tool's inputSchema and verify that every "type":"array"
     * property also contains "items". */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    /* Scan for all occurrences of "type":"array" — each must be followed
     * by "items" before the next closing brace of that property. */
    const char *p = json;
    while ((p = strstr(p, "\"type\":\"array\"")) != NULL) {
        /* Find the enclosing '}' for this property object */
        const char *end = strchr(p, '}');
        ASSERT_NOT_NULL(end);
        /* "items" must appear between p and end */
        size_t span = (size_t)(end - p);
        char *segment = malloc(span + 1);
        memcpy(segment, p, span);
        segment[span] = '\0';
        ASSERT_NOT_NULL(strstr(segment, "\"items\"")); /* array missing items */
        free(segment);
        p = end;
    }

    free(json);
    PASS();
}

TEST(mcp_text_result) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"type\":\"text\""));
    /* The text value is JSON-escaped inside the "text" field */
    ASSERT_NOT_NULL(strstr(json, "total"));
    ASSERT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_text_result_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(json, "something failed"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_tool_name) {
    const char *params = "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\"}}";
    char *name = cbm_mcp_get_tool_name(params);
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "search_graph");
    free(name);
    PASS();
}

TEST(mcp_get_arguments) {
    const char *params =
        "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\",\"limit\":5}}";
    char *args = cbm_mcp_get_arguments(params);
    ASSERT_NOT_NULL(args);
    ASSERT_NOT_NULL(strstr(args, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(args, "\"limit\":5"));
    free(args);
    PASS();
}

TEST(mcp_get_string_arg) {
    const char *args = "{\"label\":\"Function\",\"name_pattern\":\".*Order.*\"}";
    char *val = cbm_mcp_get_string_arg(args, "label");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "Function");
    free(val);

    val = cbm_mcp_get_string_arg(args, "name_pattern");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, ".*Order.*");
    free(val);

    val = cbm_mcp_get_string_arg(args, "nonexistent");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg) {
    const char *args = "{\"limit\":10,\"offset\":5}";
    int val = cbm_mcp_get_int_arg(args, "limit", 0);
    ASSERT_EQ(val, 10);
    val = cbm_mcp_get_int_arg(args, "offset", 0);
    ASSERT_EQ(val, 5);
    val = cbm_mcp_get_int_arg(args, "missing", 42);
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(mcp_get_bool_arg) {
    const char *args = "{\"include_connected\":true,\"regex\":false}";
    bool val = cbm_mcp_get_bool_arg(args, "include_connected");
    ASSERT_TRUE(val);
    val = cbm_mcp_get_bool_arg(args, "regex");
    ASSERT_FALSE(val);
    val = cbm_mcp_get_bool_arg(args, "missing");
    ASSERT_FALSE(val);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — PROTOCOL FLOW
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_initialize) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                   "\"params\":{\"capabilities\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(resp, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(resp, "capabilities"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_initialized_notification) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Notification has no id → no response */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    ASSERT_NOT_NULL(strstr(resp, "query_graph"));
    ASSERT_NOT_NULL(strstr(resp, "\"explore\"")); /* registered + its inputSchema is valid JSON */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_unknown_method) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"unknown/method\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32601")); /* Method not found */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS (via server_handle)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: create a server with an in-memory store populated with test data */
static cbm_mcp_server_t *setup_mcp_with_data(void) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL); /* NULL = in-memory */
    return srv;
}

TEST(tool_list_projects_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":10"));
    /* Should return a result (possibly empty list) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_graph_schema_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_graph_schema\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_unknown_tool) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"nonexistent_tool\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return result with isError */
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* explore (WS1): missing query → clean required-field error, no crash. */
TEST(tool_explore_requires_query) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"explore\",\"arguments\":{\"project\":\"none\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    ASSERT_NOT_NULL(strstr(resp, "query is required"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* explore (WS1): unindexed project → clean error envelope, no crash (exercises the
 * store-resolution + cleanup paths of handle_explore on an empty server). */
TEST(tool_explore_unindexed_no_crash) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"explore\",\"arguments\":{\"query\":\"foo bar\",\"project\":\"nope\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    /* search_graph with no project → should work on empty store */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"label\":\"Function\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Forward declarations for helpers defined later in this file */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz);
static void cleanup_snippet_dir(const char *tmp_dir);
static char *extract_text_content(const char *mcp_result);

TEST(tool_search_graph_includes_node_properties) {
    /* search_graph results must surface each node's properties_json
     * payload so callers don't have to round-trip through get_code_snippet
     * just to read them. The setup_snippet_server inserts HandleRequest
     * with a signature/return_type/is_exported property blob; this test
     * pins that those keys reach the MCP response. */
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project\":\"test-project\",\"label\":\"Function\","
             "\"name_pattern\":\"HandleRequest\",\"limit\":5}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* Properties from HandleRequest's properties_json must appear. */
    ASSERT_NOT_NULL(strstr(inner, "signature"));
    ASSERT_NOT_NULL(strstr(inner, "func HandleRequest"));
    ASSERT_NOT_NULL(strstr(inner, "is_exported"));
    free(inner);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_search_graph_query_honors_file_pattern_issue552) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "issue-552";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/issue-552");

    cbm_node_t lib_status = {0};
    lib_status.project = proj;
    lib_status.label = "Function";
    lib_status.name = "status";
    lib_status.qualified_name = "issue-552.src.lib.status";
    lib_status.file_path = "src/lib/status.c";
    lib_status.start_line = 1;
    lib_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &lib_status), 0);

    cbm_node_t component_status = {0};
    component_status.project = proj;
    component_status.label = "Function";
    component_status.name = "status";
    component_status.qualified_name = "issue-552.src.components.status";
    component_status.file_path = "src/components/status.c";
    component_status.start_line = 1;
    component_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &component_status), 0);

    cbm_store_exec(st, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');");
    ASSERT_EQ(cbm_store_exec(st,
                             "INSERT INTO nodes_fts(rowid, name, qualified_name, label, "
                             "file_path) "
                             "SELECT id, cbm_camel_split(name), qualified_name, label, file_path "
                             "FROM nodes;"),
              CBM_STORE_OK);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":552,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"project\":\"issue-552\",\"query\":\"status\","
                                   "\"file_pattern\":\"src/lib/*\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "search_mode: bm25"));
    ASSERT_NOT_NULL(strstr(inner, "src/lib/status.c"));
    ASSERT_NULL(strstr(inner, "src/components/status.c"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

static int semantic915_insert_node_vector(sqlite3 *db, int64_t node_id, const char *project,
                                          const int8_t vector[CBM_SEM_DIM]) {
    const char *sql = "INSERT INTO node_vectors(node_id, project, vector) VALUES (?1, ?2, ?3)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    sqlite3_bind_int64(stmt, 1, node_id);
    sqlite3_bind_text(stmt, 2, project, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, vector, CBM_SEM_DIM, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static int semantic915_insert_token_vector(sqlite3 *db, const char *project, const char *token,
                                           const int8_t vector[CBM_SEM_DIM]) {
    const char *sql =
        "INSERT INTO token_vectors(project, token, vector, idf) VALUES (?1, ?2, ?3, ?4)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return SQLITE_ERROR;
    }
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, vector, CBM_SEM_DIM, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, 1);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

TEST(tool_search_graph_semantic_only_uses_vector_results_issue915) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-sem915-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        FAIL("mkdtemp cache failed");
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "issue-915-semantic";
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);

    cbm_store_t *st = cbm_store_open_path(db_path);
    bool setup_ok = st != NULL;
    if (setup_ok &&
        cbm_store_upsert_project(st, project, "/tmp/issue-915-semantic") != CBM_STORE_OK) {
        setup_ok = false;
    }

    cbm_node_t registry = {.project = project,
                           .label = "Function",
                           .name = "AAARegistryKey",
                           .qualified_name = "issue915.registry.AAARegistryKey",
                           .file_path = "registry.c",
                           .start_line = 1,
                           .end_line = 3};
    if (setup_ok && cbm_store_upsert_node(st, &registry) <= 0) {
        setup_ok = false;
    }

    cbm_node_t debt = {.project = project,
                       .label = "Function",
                       .name = "DebtCalculator",
                       .qualified_name = "issue915.finance.DebtCalculator",
                       .file_path = "debt.c",
                       .start_line = 10,
                       .end_line = 20};
    cbm_node_t resolver = {.project = project,
                           .label = "Function",
                           .name = "DebtResolver",
                           .qualified_name = "issue915.finance.DebtResolver",
                           .file_path = "resolver.c",
                           .start_line = 30,
                           .end_line = 40};
    int64_t debt_id = setup_ok ? cbm_store_upsert_node(st, &debt) : 0;
    int64_t resolver_id = setup_ok ? cbm_store_upsert_node(st, &resolver) : 0;
    setup_ok = setup_ok && debt_id > 0 && resolver_id > debt_id;

    if (setup_ok) {
        setup_ok =
            cbm_store_exec(st, "CREATE TABLE node_vectors (node_id INTEGER PRIMARY KEY, "
                               "project TEXT NOT NULL, vector BLOB NOT NULL)") == CBM_STORE_OK;
    }
    if (setup_ok) {
        setup_ok =
            cbm_store_exec(st, "CREATE TABLE token_vectors (id INTEGER PRIMARY KEY, "
                               "project TEXT NOT NULL, token TEXT NOT NULL, "
                               "vector BLOB NOT NULL, idf INTEGER NOT NULL)") == CBM_STORE_OK;
    }

    int8_t debt_vec[CBM_SEM_DIM] = {0};
    debt_vec[0] = 127;
    if (setup_ok) {
        setup_ok = semantic915_insert_token_vector(cbm_store_get_db(st), project, "debt",
                                                   debt_vec) == SQLITE_OK;
    }
    if (setup_ok) {
        setup_ok = semantic915_insert_node_vector(cbm_store_get_db(st), debt_id, project,
                                                  debt_vec) == SQLITE_OK;
    }
    if (setup_ok) {
        setup_ok = semantic915_insert_node_vector(cbm_store_get_db(st), resolver_id, project,
                                                  debt_vec) == SQLITE_OK;
    }

    /* More candidates than the hard window, all below the query threshold. */
    int8_t tail_vec[CBM_SEM_DIM] = {0};
    tail_vec[0] = 1;
    tail_vec[1] = 127;
    for (int i = 0; setup_ok && i < 260; i++) {
        char name[32];
        char qn[64];
        char file[32];
        snprintf(name, sizeof(name), "NearZeroTail%03d", i);
        snprintf(qn, sizeof(qn), "issue915.tail.%s", name);
        snprintf(file, sizeof(file), "tail_%03d.c", i);
        cbm_node_t tail = {.project = project,
                           .label = "Function",
                           .name = name,
                           .qualified_name = qn,
                           .file_path = file,
                           .start_line = i + 1,
                           .end_line = i + 1};
        int64_t tail_id = cbm_store_upsert_node(st, &tail);
        setup_ok = tail_id > 0 && semantic915_insert_node_vector(cbm_store_get_db(st), tail_id,
                                                                 project, tail_vec) == SQLITE_OK;
    }
    if (st) {
        (void)cbm_store_checkpoint(st);
        cbm_store_close(st);
    }
    if (!setup_ok) {
        cleanup_project_db(cache, project);
        restore_cache_dir(saved_copy);
        free(saved_copy);
        cbm_rmdir(cache);
        FAIL("semantic fixture setup failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    CBMLogLevel previous_log_level = cbm_log_get_level();
    semantic915_fetch_limit = -1;
    cbm_log_set_level(CBM_LOG_INFO);
    cbm_log_set_sink_ex(semantic915_capture_vector_log, CBM_LOG_SINK_REPLACE);
    char *resp =
        cbm_mcp_handle_tool(srv, "search_graph",
                            "{\"project\":\"issue-915-semantic\",\"semantic_query\":[\"debt\"],"
                            "\"format\":\"json\",\"limit\":1,\"offset\":0}");
    cbm_log_set_sink(NULL);
    cbm_log_set_level(previous_log_level);
    int primary_fetch_limit = semantic915_fetch_limit;

    char *inner = extract_text_content(resp);
    bool first_page_ok =
        inner && response_contains_json_fragment(inner, "\"search_mode\":\"semantic\"") &&
        response_contains_json_fragment(inner, "\"total\":2") &&
        response_contains_json_fragment(inner, "\"has_more\":true") &&
        response_contains_json_fragment(inner, "\"name\":\"DebtCalculator\"") &&
        strstr(inner, "DebtResolver") == NULL && strstr(inner, "NearZeroTail") == NULL &&
        strstr(inner, "AAARegistryKey") == NULL && strstr(inner, "semantic_results") == NULL;
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"semantic_query\":[\"debt\"],"
                               "\"format\":\"json\",\"limit\":1,\"offset\":1}");
    inner = extract_text_content(resp);
    bool second_page_ok = inner && response_contains_json_fragment(inner, "\"total\":2") &&
                          response_contains_json_fragment(inner, "\"has_more\":false") &&
                          response_contains_json_fragment(inner, "\"name\":\"DebtResolver\"") &&
                          strstr(inner, "DebtCalculator") == NULL;
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"semantic_query\":[\"debt\"],"
                               "\"format\":\"json\",\"limit\":1,\"offset\":2}");
    inner = extract_text_content(resp);
    bool exhausted_page_ok = inner && response_contains_json_fragment(inner, "\"total\":2") &&
                             response_contains_json_fragment(inner, "\"results\":[]") &&
                             response_contains_json_fragment(inner, "\"has_more\":false");
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"semantic_query\":[\"debt\"],"
                               "\"limit\":1,\"offset\":1}");
    inner = extract_text_content(resp);
    bool toon_page_ok = inner && strstr(inner, "search_mode: semantic") &&
                        strstr(inner, "total: 2") && strstr(inner, "semantic[1]") &&
                        strstr(inner, "DebtResolver") && strstr(inner, "has_more: false") &&
                        strstr(inner, "results[") == NULL && strstr(inner, "NearZeroTail") == NULL;
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"label\":\"\","
                               "\"semantic_query\":[\"debt\"],\"format\":\"json\",\"limit\":1}");
    inner = extract_text_content(resp);
    bool empty_label_omitted_ok =
        inner && response_contains_json_fragment(inner, "\"search_mode\":\"semantic\"") &&
        response_contains_json_fragment(inner, "\"total\":2") &&
        response_contains_json_fragment(inner, "\"name\":\"DebtCalculator\"") &&
        strstr(inner, "semantic_results") == NULL && strstr(inner, "AAARegistryKey") == NULL;
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"label\":\"\","
                               "\"semantic_query\":[\"debt\"],\"limit\":1}");
    inner = extract_text_content(resp);
    bool empty_label_toon_ok = inner && strstr(inner, "search_mode: semantic") &&
                               strstr(inner, "total: 2") && strstr(inner, "semantic[1]") &&
                               strstr(inner, "DebtCalculator") && strstr(inner, "results[") == NULL;
    free(inner);
    free(resp);

    semantic915_fetch_limit = -1;
    cbm_log_set_level(CBM_LOG_INFO);
    cbm_log_set_sink_ex(semantic915_capture_vector_log, CBM_LOG_SINK_REPLACE);
    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"label\":\"Function\","
                               "\"semantic_query\":[\"debt\"],\"format\":\"json\",\"limit\":60}");
    cbm_log_set_sink(NULL);
    cbm_log_set_level(previous_log_level);
    int combined_fetch_limit = semantic915_fetch_limit;
    inner = extract_text_content(resp);
    yyjson_doc *combined_doc = inner ? yyjson_read(inner, strlen(inner), 0) : NULL;
    yyjson_val *combined_root = combined_doc ? yyjson_doc_get_root(combined_doc) : NULL;
    yyjson_val *semantic_results =
        combined_root ? yyjson_obj_get(combined_root, "semantic_results") : NULL;
    bool combined_has_tail = false;
    size_t combined_idx = 0;
    size_t combined_max = 0;
    yyjson_val *combined_item;
    yyjson_arr_foreach(semantic_results, combined_idx, combined_max, combined_item) {
        const char *name = yyjson_get_str(yyjson_obj_get(combined_item, "name"));
        if (name && strncmp(name, "NearZeroTail", strlen("NearZeroTail")) == 0) {
            combined_has_tail = true;
        }
    }
    bool combined_ok = semantic_results && yyjson_is_arr(semantic_results) &&
                       yyjson_arr_size(semantic_results) == 60 && combined_has_tail;
    if (combined_doc) {
        yyjson_doc_free(combined_doc);
    }
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "search_graph",
                               "{\"project\":\"issue-915-semantic\",\"semantic_query\":[\"debt\"],"
                               "\"format\":\"json\",\"limit\":1,\"offset\":2147483647}");
    inner = extract_text_content(resp);
    bool large_offset_ok = inner && response_contains_json_fragment(inner, "\"results\":[]") &&
                           response_contains_json_fragment(inner, "\"has_more\":false");
    free(inner);
    free(resp);

    resp = cbm_mcp_handle_tool(
        srv, "search_graph",
        "{\"project\":\"issue-915-semantic\",\"semantic_query\":\"debt\",\"limit\":6}");
    bool type_error_unchanged =
        resp && strstr(resp, "semantic_query must be an array of keyword strings") != NULL;
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);

    ASSERT_EQ(primary_fetch_limit, CBM_VECTOR_SEARCH_CANDIDATE_CAP);
    ASSERT_EQ(combined_fetch_limit, 300);
    ASSERT_TRUE(first_page_ok);
    ASSERT_TRUE(second_page_ok);
    ASSERT_TRUE(exhausted_page_ok);
    ASSERT_TRUE(toon_page_ok);
    ASSERT_TRUE(empty_label_omitted_ok);
    ASSERT_TRUE(empty_label_toon_ok);
    ASSERT_TRUE(combined_ok);
    ASSERT_TRUE(large_offset_ok);
    ASSERT_TRUE(type_error_unchanged);
    PASS();
}

/* Resource discovery methods this server doesn't populate must return EMPTY
 * lists, not -32601 Method-not-found: clients like Cline probe them on connect
 * and surface the errors as a failed connection (#958). */
TEST(mcp_resource_discovery_methods_return_empty_lists) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    struct {
        const char *method;
        const char *want;
    } cases[] = {
        {"resources/list", "\"resources\":[]"},
        {"resources/templates/list", "\"resourceTemplates\":[]"},
    };
    for (int i = 0; i < 2; i++) {
        char reqbuf[256];
        snprintf(reqbuf, sizeof(reqbuf), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                 100 + i, cases[i].method);
        char *resp = cbm_mcp_server_handle(srv, reqbuf);
        ASSERT_NOT_NULL(resp);
        ASSERT_NULL(strstr(resp, "Method not found"));
        ASSERT_NOT_NULL(strstr(resp, cases[i].want));
        free(resp);
    }

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\","
             "\"arguments\":{\"query\":\"MATCH (f:Function) RETURN f.name\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_no_project) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or empty status */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_includes_git_metadata) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\","
                                   "\"arguments\":{\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"root_path\""));
    ASSERT_NOT_NULL(strstr(inner, "\"git\""));
    ASSERT_NOT_NULL(strstr(inner, "\"is_git\":false"));
    ASSERT_NOT_NULL(strstr(inner, "\"root_exists\":true"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS WITH DATA
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_trace_call_path_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{\"function_name\":\"NonExistent\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about project not found */
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_trace_missing_function_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: two same-named definitions with equal rank must be reported
 * ambiguous, not silently traced (trace_path previously took nodes[0]). */
TEST(tool_trace_call_path_ambiguous) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "amb-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/amb");
    cbm_node_t a = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.a.amb",
                    .file_path = "a.c",
                    .start_line = 10,
                    .end_line = 20};
    cbm_node_t b = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.b.amb",
                    .file_path = "b.c",
                    .start_line = 10,
                    .end_line = 20}; /* equal span -> genuine tie */
    ASSERT_GT(cbm_store_upsert_node(st, &a), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &b), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\","
             "\"arguments\":{\"function_name\":\"amb\",\"project\":\"amb-proj\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "\"callees\""));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: when same-named nodes differ in rank, trace must pick the real
 * definition (callable, larger body) — NOT nodes[0]. The Module is inserted
 * first; if trace took nodes[0] the outbound trace would be empty. */
TEST(tool_trace_call_path_prefers_definition) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "pref-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/pref");
    /* nodes[0]: the WRONG match (a Module, tiny span), inserted first. */
    cbm_node_t wrong = {.project = proj,
                        .label = "Module",
                        .name = "dup",
                        .qualified_name = "pref-proj.dup",
                        .file_path = "dup.x",
                        .start_line = 1,
                        .end_line = 1};
    /* the real definition: a Function with a body. */
    cbm_node_t def = {.project = proj,
                      .label = "Function",
                      .name = "dup",
                      .qualified_name = "pref-proj.src.dup",
                      .file_path = "src/dup.c",
                      .start_line = 10,
                      .end_line = 50};
    cbm_node_t callee = {.project = proj,
                         .label = "Function",
                         .name = "callee",
                         .qualified_name = "pref-proj.src.callee",
                         .file_path = "src/dup.c",
                         .start_line = 60,
                         .end_line = 70};
    ASSERT_GT(cbm_store_upsert_node(st, &wrong), 0);
    int64_t id_def = cbm_store_upsert_node(st, &def);
    int64_t id_callee = cbm_store_upsert_node(st, &callee);
    ASSERT_GT(id_def, 0);
    ASSERT_GT(id_callee, 0);
    cbm_edge_t e = {.project = proj, .source_id = id_def, .target_id = id_callee, .type = "CALLS"};
    cbm_store_insert_edge(st, &e);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"dup\","
             "\"project\":\"pref-proj\",\"direction\":\"outbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    /* picked the Function definition -> its outbound CALLS edge to "callee" shows */
    ASSERT_NOT_NULL(strstr(inner, "callee"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_delete_project_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"delete_project\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not_found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_architecture_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No store for nonexistent project — should return project error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #281: handle_get_architecture must actually call
 * cbm_store_get_architecture and surface its sections. Before the fix
 * only label/edge histograms were emitted regardless of which aspects
 * were requested. The store-side arch_entry_points query reads
 * properties.is_entry_point on Function nodes, so we tag one node and
 * assert the resulting JSON surfaces an "entry_points" array containing
 * the tagged function — which is impossible without the wiring. */
TEST(tool_get_architecture_emits_populated_sections) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-test";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-test");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch-test.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-test\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The handler always emits node/edge counts and schema histograms;
     * those existed before #281. The "entry_points" array only appears
     * when cbm_store_get_architecture is actually called and its result
     * is serialized — which is exactly what #281 wires up. */
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "main"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed") ||
                strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_code_multi_word) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Multi-word query "HandleRequest error" — should find the line
     * "func HandleRequest() error {" via regex conversion. */
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest error\","
             "\"project\":\"test-project\"}}}");

    char *resp = cbm_mcp_server_handle(srv, req);
    ASSERT_NOT_NULL(resp);
    /* Should find at least one result (not zero) */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL);
    /* Should NOT contain an error about "not found" */
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #283: search_code with regex=true and a syntactically invalid pattern
 * must return an explicit error, not an empty result indistinguishable from a
 * legitimate no-match. */
TEST(search_code_invalid_regex_errors_issue283) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Unclosed group under regex=true → must be flagged as an error. */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func(\",\"regex\":true,"
                                   "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "invalid regex"));
    free(resp);

    /* Same pattern as a literal (regex=false) must NOT error. */
    resp = cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
                                      "\"params\":{\"name\":\"search_code\","
                                      "\"arguments\":{\"pattern\":\"func(\",\"regex\":false,"
                                      "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid regex") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #282: a literal '|' under regex=false is a silent 0-match trap. It must
 * now be surfaced as a warning (and the result carries elapsed_ms). */
TEST(search_code_literal_pipe_warns_issue282) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest|Nope\","
                                   "\"regex\":false,\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "warnings"));   /* surfaced, not silent */
    ASSERT_NOT_NULL(strstr(resp, "regex=true")); /* the hint names the fix */
    ASSERT_NOT_NULL(strstr(resp, "elapsed_ms")); /* timing is reported */
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #272: '&' in a path / file_pattern is neutralised by the command's
 * quoting and must no longer be rejected as "invalid characters". */
TEST(search_code_ampersand_accepted_issue272) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest\","
                                   "\"file_pattern\":\"*R&D*.go\",\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid characters") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"manage_adr\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression test for use-after-free in handle_manage_adr (get path).
 * MUST FAIL before fix: free(buf) is called before yy_doc_to_str serializes doc,
 * so result field is missing or contains garbage. MUST PASS after fix. */
TEST(tool_manage_adr_get_with_existing_adr) {
    /* Create a temp directory with .codebase-memory/adr.md */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS(); /* skip if mkdtemp fails */
    }

    char adr_dir[512];
    snprintf(adr_dir, sizeof(adr_dir), "%s/.codebase-memory", tmp_dir);
    cbm_mkdir(adr_dir);

    char adr_path[512];
    snprintf(adr_path, sizeof(adr_path), "%s/adr.md", adr_dir);
    FILE *fp = fopen(adr_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("## PURPOSE\nTest ADR content for regression test.\n\n"
          "## STACK\nC, SQLite.\n\n"
          "## ARCHITECTURE\nMCP server.\n",
          fp);
    fclose(fp);

    /* Create server and register the project */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "test-adr-uaf", tmp_dir);
    cbm_mcp_server_set_project(srv, "test-adr-uaf");

    /* Call manage_adr via full JSON-RPC path to exercise cbm_jsonrpc_format_response.
     * The bug: free(buf) before yy_doc_to_str causes garbage JSON; format_response
     * then fails to parse the result and omits the "result" field entirely. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\","
             "\"arguments\":{\"project\":\"test-adr-uaf\",\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    /* JSON-RPC response must include a "result" field (absent when use-after-free) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    /* ADR content must appear in response */
    ASSERT_NOT_NULL(strstr(resp, "PURPOSE"));
    /* Must not be an error */
    ASSERT_NULL(strstr(resp, "isError"));
    free(resp);

    /* Clean up */
    cbm_mcp_server_free(srv);
    remove(adr_path);
    rmdir(adr_dir);
    rmdir(tmp_dir);
    PASS();
}

/* issue #256: manage_adr (MCP) and the UI /api/adr endpoints must share ONE
 * backend. A manage_adr(update) write must be readable via cbm_store_adr_get
 * (the exact API the UI's /api/adr GET uses). */
TEST(tool_manage_adr_unified_backend_issue256) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-unify", "/tmp/adr-unify");
    cbm_mcp_server_set_project(srv, "adr-unify");

    /* Write via the MCP tool. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":120,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"update\",\"content\":\"## PURPOSE\\nUnified ADR backend.\\n\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    /* Read DIRECTLY via the store API the UI /api/adr uses — must see it. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-unify", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_NOT_NULL(strstr(adr.content, "Unified ADR backend."));
    cbm_store_adr_free(&adr);

    /* And manage_adr(get) round-trips the same content. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":121,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Unified ADR backend."));
    ASSERT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"ingest_traces\","
             "\"arguments\":{\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    ASSERT_NOT_NULL(strstr(resp, "traces_received"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":38,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"ingest_traces\","
                                   "\"arguments\":{\"traces\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Caller passes `project_name` (the natural guess) instead of `project`. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":640,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project_name\":\"alias640\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* RED before the alias: inner is the "project not found" error.
     * GREEN after: the alias resolves and architecture sections surface. */
    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "entry_points:"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first for #640: the alias must apply across query handlers, not
 * just get_architecture. search_graph with `project_name` must resolve too. */
TEST(tool_search_graph_accepts_project_name_alias_issue640) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "alias640b";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/alias640b");

    cbm_node_t fn = {0};
    fn.project = proj;
    fn.label = "Function";
    fn.name = "WidgetHandler";
    fn.qualified_name = "alias640b.svc.WidgetHandler";
    fn.file_path = "svc/widget.go";
    fn.start_line = 1;
    fn.end_line = 2;
    ASSERT_GT(cbm_store_upsert_node(st, &fn), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":641,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project_name\":\"alias640b\",\"name_pattern\":\"Widget.*\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "WidgetHandler"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* #1025: agents pass the repo FOLDER name ("codebase-memory-mcp"), but
 * indexed project names derive from the full path
 * (E:\project\graph\x -> "E-project-graph-x"), so exact lookup fails with
 * "project not found" while list_projects clearly shows the project. A
 * passed name that matches exactly ONE indexed project as a segment-aligned
 * tail ("-<name>" suffix) must resolve to it; zero or several matches keep
 * the existing error. Runs against real cache-dir .db files (the resolution
 * scans filenames), so this test indexes real fixtures under an overridden
 * CBM_CACHE_DIR. */
static void i1025_write_repo(const char *dir, const char *fn_name) {
    char path[CBM_SZ_512];
    snprintf(path, sizeof(path), "%s/mod.py", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "def %s(x):\n    return x + 1\n", fn_name);
    fclose(f);
}

TEST(tool_project_arg_resolves_unique_tail_issue1025) {
    char repo_a[CBM_SZ_256];
    char repo_b[CBM_SZ_256];
    char repo_c[CBM_SZ_256];
    char cache[CBM_SZ_256];
    snprintf(repo_a, sizeof(repo_a), "/tmp/cbm-i1025a-XXXXXX");
    snprintf(repo_b, sizeof(repo_b), "/tmp/cbm-i1025b-XXXXXX");
    snprintf(repo_c, sizeof(repo_c), "/tmp/cbm-i1025c-XXXXXX");
    snprintf(cache, sizeof(cache), "/tmp/cbm-i1025d-XXXXXX");
    if (!cbm_mkdtemp(repo_a) || !cbm_mkdtemp(repo_b) || !cbm_mkdtemp(repo_c) ||
        !cbm_mkdtemp(cache)) {
        FAIL("mkdtemp failed");
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? cbm_strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_setenv("CBM_INDEX_SUPERVISOR", "0", 1);

    i1025_write_repo(repo_a, "unique_tail_target");
    i1025_write_repo(repo_b, "amb_one");
    i1025_write_repo(repo_c, "amb_two");

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[CBM_SZ_1K];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"name\":\"E-project-graph-suffix1025\"}",
             repo_a);
    char *r = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(r);
    free(r);
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"name\":\"F-alpha-amb1025\"}", repo_b);
    r = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(r);
    free(r);
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"name\":\"G-beta-amb1025\"}", repo_c);
    r = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(r);
    free(r);

    /* 1. Unique tail resolves (RED today: "project not found"). */
    r = cbm_mcp_handle_tool(srv, "search_graph",
                            "{\"project\":\"suffix1025\",\"name_pattern\":\".*target.*\"}");
    ASSERT_NOT_NULL(r);
    if (strstr(r, "project not found")) {
        fprintf(stderr, "  [1025] FAIL unique tail did not resolve: %.200s\n", r);
    }
    ASSERT_NULL(strstr(r, "project not found"));
    ASSERT_NOT_NULL(strstr(r, "unique_tail_target"));
    free(r);

    /* 2. Ambiguous tail stays an error (never guess between projects). */
    r = cbm_mcp_handle_tool(srv, "search_graph",
                            "{\"project\":\"amb1025\",\"name_pattern\":\".*\"}");
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(strstr(r, "project not found"));
    free(r);

    /* 3. Exact full name keeps working unchanged. */
    r = cbm_mcp_handle_tool(srv, "search_graph",
                            "{\"project\":\"E-project-graph-suffix1025\","
                            "\"name_pattern\":\".*target.*\"}");
    ASSERT_NOT_NULL(r);
    ASSERT_NULL(strstr(r, "project not found"));
    free(r);

    cbm_mcp_server_free(srv);
    if (saved_cache_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache_copy, 1);
        free(saved_cache_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_rmtree(repo_a);
    th_rmtree(repo_b);
    th_rmtree(repo_c);
    th_rmtree(cache);
    PASS();
}

/* Regression for #604: path scopes architecture totals and content. */
TEST(tool_get_architecture_path_scoping) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-path";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-path");

    cbm_node_t pkg_global = {.project = proj,
                             .label = "Package",
                             .name = "Django",
                             .qualified_name = "arch-path.Django",
                             .file_path = "vendor/django/__init__.py"};
    cbm_store_upsert_node(st, &pkg_global);

    cbm_node_t pkg_local = {.project = proj,
                            .label = "Package",
                            .name = "hoa",
                            .qualified_name = "arch-path.hoa",
                            .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &pkg_local);

    cbm_node_t f_hoa = {.project = proj,
                        .label = "File",
                        .name = "main.go",
                        .qualified_name = "arch-path.apps.hoa.main.go",
                        .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &f_hoa);

    cbm_node_t f_other = {.project = proj,
                          .label = "File",
                          .name = "other.go",
                          .qualified_name = "arch-path.other.go",
                          .file_path = "lib/other.go"};
    cbm_store_upsert_node(st, &f_other);

    char *resp_root = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-path\",\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_root);
    char *inner_root = extract_text_content(resp_root);
    ASSERT_NOT_NULL(inner_root);
    ASSERT_NOT_NULL(strstr(inner_root, "Django"));

    char *resp_scoped =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"arch-path\",\"path\":\"apps/hoa\","
                                   "\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_scoped);
    char *inner_scoped = extract_text_content(resp_scoped);
    ASSERT_NOT_NULL(inner_scoped);

    ASSERT_NOT_NULL(strstr(inner_scoped, "root_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "scoped_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "path: "));
    ASSERT_NOT_NULL(strstr(inner_scoped, "hoa"));
    ASSERT_NULL(strstr(inner_scoped, "Django"));

    int root_nodes = 0;
    int scoped_nodes = 0;
    /* TOON scalar form (`key: N`) with JSON fallback for format:"json". */
    const char *rt = strstr(inner_scoped, "root_total_nodes: ");
    const char *stn = strstr(inner_scoped, "scoped_total_nodes: ");
    if (rt) {
        sscanf(rt, "root_total_nodes: %d", &root_nodes);
    } else if ((rt = strstr(inner_scoped, "\"root_total_nodes\":")) != NULL) {
        sscanf(rt, "\"root_total_nodes\":%d", &root_nodes);
    }
    if (stn) {
        sscanf(stn, "scoped_total_nodes: %d", &scoped_nodes);
    } else if ((stn = strstr(inner_scoped, "\"scoped_total_nodes\":")) != NULL) {
        sscanf(stn, "\"scoped_total_nodes\":%d", &scoped_nodes);
    }
    ASSERT_TRUE(root_nodes > scoped_nodes);
    ASSERT_TRUE(scoped_nodes > 0);

    free(inner_scoped);
    free(resp_scoped);
    free(inner_root);
    free(resp_root);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed") ||
                strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_code_multi_word) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Multi-word query "HandleRequest error" — should find the line
     * "func HandleRequest() error {" via regex conversion. */
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest error\","
             "\"project\":\"test-project\"}}}");

    char *resp = cbm_mcp_server_handle(srv, req);
    ASSERT_NOT_NULL(resp);
    /* Should find at least one result (not zero) */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL);
    /* Should NOT contain an error about "not found" */
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#687): scoped content search over a repo whose ROOT PATH
 * contains a space. write_scoped_filelist emits "<root>/<file>" records that the
 * Unix pipeline pipes to grep via xargs. With plain `xargs` (newline-split) the
 * space splits one path into several bogus args -> grep finds nothing ->
 * total_grep_matches == 0 (RED on the unfixed code). The fix writes NUL-separated
 * records + uses `xargs -0`, so the path stays a single argument -> match found
 * (GREEN). On Windows the scoped path uses PowerShell Get-Content -LiteralPath,
 * which already handles spaces, so this asserts correct behavior there too. */
TEST(search_code_scoped_path_with_spaces_issue687) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_srch_space_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        FAIL("cbm_mkdtemp failed");
    }

    /* Project root deliberately contains a space. */
    char proj_dir[640];
    snprintf(proj_dir, sizeof(proj_dir), "%s/my project", tmp);
    cbm_mkdir(proj_dir);

    char src_path[768];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        rmdir(proj_dir);
        rmdir(tmp);
        FAIL("cannot write source file under spaced path");
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "space-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    /* A node so the file is "indexed" (cbm_store_list_files -> scoped grep path)
     * and the grep hit classifies to a result. */
    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "HandleRequest",
                    .qualified_name = "space-search.main.HandleRequest",
                    .file_path = "main.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(st, &n), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"space-search\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* grep must have found the match despite the space in the root path. */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    } else if ((g = strstr(inner, "total_grep_matches: ")) != NULL) {
        /* TOON scalar form — the search_code compact default. */
        sscanf(g, "total_grep_matches: %d", &grep_matches);
    }
    ASSERT_TRUE(grep_matches > 0);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    unlink(src_path);
    rmdir(proj_dir);
    rmdir(tmp);
    PASS();
}

#ifdef _WIN32
/* Issue #903 follow-up: scoped search_code on Windows writes a UTF-8 filelist
 * containing absolute source paths, then reads it back through PowerShell.
 * Windows PowerShell 5.1 treats UTF-8 without BOM as ANSI unless told
 * otherwise, so a non-ASCII project root can be mojibaked before
 * Select-String sees the LiteralPath. */
TEST(search_code_scoped_path_with_cjk_root_issue903) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/cbm_srch_cjk_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp)) {
        FAIL("cbm_mkdtemp failed");
    }

    char proj_dir[640];
    snprintf(proj_dir, sizeof(proj_dir), "%s/%s", tmp,
             "\xE4\xB8\xAD\xE6\x96\x87\xE9\xA1\xB9\xE7\x9B\xAE");
    if (!cbm_mkdir_p(proj_dir, 0755)) {
        cbm_rmdir(tmp);
        FAIL("cannot create CJK project dir");
    }

    char src_path[768];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = cbm_fopen(src_path, "wb");
    if (!fp) {
        cbm_rmdir(proj_dir);
        cbm_rmdir(tmp);
        FAIL("cannot write source file under CJK path");
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "cjk-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "HandleRequest",
                    .qualified_name = "cjk-search.main.HandleRequest",
                    .file_path = "main.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(st, &n), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":903,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"cjk-search\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    } else if ((g = strstr(inner, "total_grep_matches: ")) != NULL) {
        /* TOON scalar form — the search_code compact default. */
        sscanf(g, "total_grep_matches: %d", &grep_matches);
    }
    ASSERT_TRUE(grep_matches > 0);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cbm_unlink(src_path);
    cbm_rmdir(proj_dir);
    cbm_rmdir(tmp);
    PASS();
}
#endif

/* Shared fixture for the path_filter prefilter tests (PR #756 distilled):
 * a project with two indexed files that both contain the search pattern —
 * src/handler.go (inside the filter) and vendor/other.go (outside it). */
static cbm_mcp_server_t *setup_prefilter_server(char *tmp, size_t tmp_sz, char *src_path,
                                                size_t src_sz, char *vendor_path,
                                                size_t vendor_sz) {
    snprintf(tmp, tmp_sz, "/tmp/cbm_srch_pref_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        return NULL;
    }
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    cbm_mkdir(dir);

    snprintf(src_path, src_sz, "%s/src/handler.go", tmp);
    snprintf(vendor_path, vendor_sz, "%s/vendor/other.go", tmp);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);
    fp = fopen(vendor_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package vendored\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "prefilter-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, tmp);

    cbm_node_t n1 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.main.HandleRequest",
                     .file_path = "src/handler.go",
                     .start_line = 3,
                     .end_line = 5};
    cbm_node_t n2 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.vendored.HandleRequest",
                     .file_path = "vendor/other.go",
                     .start_line = 3,
                     .end_line = 5};
    if (cbm_store_upsert_node(st, &n1) <= 0 || cbm_store_upsert_node(st, &n2) <= 0) {
        cbm_mcp_server_free(srv);
        return NULL;
    }
    return srv;
}

static void cleanup_prefilter_dir(const char *tmp, const char *src_path, const char *vendor_path) {
    char dir[640];
    unlink(src_path);
    unlink(vendor_path);
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    rmdir(dir);
    rmdir(tmp);
}

/* PR #756 (distilled): scoped search_code prefilters the indexed filelist by
 * path_filter before grep runs. POSITIVE invariant guard: a path_filter that
 * matches the file containing the hit must still return that hit (guards
 * against over-filtering — the prefilter predicate must stay IDENTICAL to the
 * post-grep filter in collect_grep_matches), and files outside the filter
 * stay excluded. Green on pre-prefilter main too (the post-grep filter alone
 * produced the same results): the change is results-preserving perf-only. */
TEST(search_code_path_filter_prefilter_keeps_matches) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":95,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^src/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The in-filter hit is returned; the out-of-filter file is not. */
    ASSERT_NOT_NULL(strstr(inner, "src/handler.go"));
    ASSERT_TRUE(strstr(inner, "vendor/other.go") == NULL);

    /* Exactly the one in-filter grep match survives (same count before and
     * after the prefilter — predicate identity). */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    } else if ((g = strstr(inner, "total_grep_matches: ")) != NULL) {
        /* TOON scalar form — the search_code compact default. */
        sscanf(g, "total_grep_matches: %d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 1);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* PR #756 (distilled): path_filter matching ZERO indexed files. With the
 * prefilter the scoped filelist has 0 records, and handle_search_code now
 * skips the grep subprocess entirely (xargs on an empty filelist is
 * platform-dependent: GNU execs grep once with no operands, BSD skips) and
 * returns the empty result directly. Must be a clean zero-result response —
 * no error. Green on pre-prefilter main too (there the full filelist is
 * grepped and the post-grep filter drops every hit — an empty filelist is
 * unreachable on main): guards the edge the prefilter introduces. */
TEST(search_code_path_filter_matches_nothing) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":96,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^no_such_dir/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    } else if ((g = strstr(inner, "total_grep_matches: ")) != NULL) {
        /* TOON scalar form — the search_code compact default. */
        sscanf(g, "total_grep_matches: %d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 0);
    int results = -1;
    const char *r = strstr(inner, "\"total_results\":");
    if (r) {
        sscanf(r, "\"total_results\":%d", &results);
    } else if ((r = strstr(inner, "total_results: ")) != NULL) {
        sscanf(r, "total_results: %d", &results);
    }
    ASSERT_EQ(results, 0);
    ASSERT_TRUE(strstr(inner, "handler.go") == NULL);
    ASSERT_TRUE(strstr(inner, "other.go") == NULL);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* issue #283: search_code with regex=true and a syntactically invalid pattern
 * must return an explicit error, not an empty result indistinguishable from a
 * legitimate no-match. */
TEST(search_code_invalid_regex_errors_issue283) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Unclosed group under regex=true → must be flagged as an error. */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func(\",\"regex\":true,"
                                   "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "invalid regex"));
    free(resp);

    /* Same pattern as a literal (regex=false) must NOT error. */
    resp = cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
                                      "\"params\":{\"name\":\"search_code\","
                                      "\"arguments\":{\"pattern\":\"func(\",\"regex\":false,"
                                      "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid regex") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #282: a literal '|' under regex=false is a silent 0-match trap. It must
 * now be surfaced as a warning (and the result carries elapsed_ms). */
TEST(search_code_literal_pipe_warns_issue282) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest|Nope\","
                                   "\"regex\":false,\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "warning"));    /* surfaced, not silent */
    ASSERT_NOT_NULL(strstr(resp, "regex=true")); /* the hint names the fix */
    ASSERT_NOT_NULL(strstr(resp, "elapsed_ms")); /* timing is reported */
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #272: '&' in a path / file_pattern is neutralised by the command's
 * quoting and must no longer be rejected as "invalid characters". */
TEST(search_code_ampersand_accepted_issue272) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest\","
                                   "\"file_pattern\":\"*R&D*.go\",\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid characters") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"manage_adr\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression test for use-after-free in handle_manage_adr (get path).
 * MUST FAIL before fix: free(buf) is called before yy_doc_to_str serializes doc,
 * so result field is missing or contains garbage. MUST PASS after fix. */
TEST(tool_manage_adr_get_with_existing_adr) {
    /* Create a temp directory with .codebase-memory/adr.md */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS(); /* skip if mkdtemp fails */
    }

    char adr_dir[512];
    snprintf(adr_dir, sizeof(adr_dir), "%s/.codebase-memory", tmp_dir);
    cbm_mkdir(adr_dir);

    char adr_path[512];
    snprintf(adr_path, sizeof(adr_path), "%s/adr.md", adr_dir);
    FILE *fp = fopen(adr_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("## PURPOSE\nTest ADR content for regression test.\n\n"
          "## STACK\nC, SQLite.\n\n"
          "## ARCHITECTURE\nMCP server.\n",
          fp);
    fclose(fp);

    /* Create server and register the project */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "test-adr-uaf", tmp_dir);
    cbm_mcp_server_set_project(srv, "test-adr-uaf");

    /* Call manage_adr via full JSON-RPC path to exercise cbm_jsonrpc_format_response.
     * The bug: free(buf) before yy_doc_to_str causes garbage JSON; format_response
     * then fails to parse the result and omits the "result" field entirely. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\","
             "\"arguments\":{\"project\":\"test-adr-uaf\",\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    /* JSON-RPC response must include a "result" field (absent when use-after-free) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    /* ADR content must appear in response */
    ASSERT_NOT_NULL(strstr(resp, "PURPOSE"));
    /* Must not be an error */
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* Clean up */
    cbm_mcp_server_free(srv);
    remove(adr_path);
    rmdir(adr_dir);
    rmdir(tmp_dir);
    PASS();
}

/* issue #256: manage_adr (MCP) and the UI /api/adr endpoints must share ONE
 * backend. A manage_adr(update) write must be readable via cbm_store_adr_get
 * (the exact API the UI's /api/adr GET uses). */
TEST(tool_manage_adr_unified_backend_issue256) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-unify", "/tmp/adr-unify");
    cbm_mcp_server_set_project(srv, "adr-unify");

    /* Write via the MCP tool. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":120,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"update\",\"content\":\"## PURPOSE\\nUnified ADR backend.\\n\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    /* Read DIRECTLY via the store API the UI /api/adr uses — must see it. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-unify", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_NOT_NULL(strstr(adr.content, "Unified ADR backend."));
    cbm_store_adr_free(&adr);

    /* And manage_adr(get) round-trips the same content. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":121,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Unified ADR backend."));
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* mode='append' adds to the stored ADR instead of replacing it, so callers can
 * add one entry without re-sending the whole document (the re-send is both
 * expensive and a chance to silently drop existing text). */
TEST(tool_manage_adr_append_extends_without_replacing) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-append", "/tmp/adr-append");
    cbm_mcp_server_set_project(srv, "adr-append");

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"adr-append\",\"mode\":\"update\","
                                     "\"content\":\"## PURPOSE\\nFirst entry.\\n\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "manage_adr",
                               "{\"project\":\"adr-append\",\"mode\":\"append\","
                               "\"content\":\"## DECISIONS\\nSecond entry.\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "appended"));
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* The prefix must survive verbatim, and the new chunk must land after it. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-append", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    const char *first = strstr(adr.content, "First entry.");
    const char *second = strstr(adr.content, "Second entry.");
    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT(first < second);
    /* Exactly one blank line joins them — repeated appends must not pile up
     * whitespace, so the trailing newline of the stored copy is trimmed. */
    ASSERT_NOT_NULL(strstr(adr.content, "First entry.\n\n## DECISIONS"));
    cbm_store_adr_free(&adr);

    /* Both sections are now discoverable via mode='sections'. */
    resp = cbm_mcp_handle_tool(srv, "manage_adr",
                               "{\"project\":\"adr-append\",\"mode\":\"sections\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "## PURPOSE"));
    ASSERT_NOT_NULL(strstr(resp, "## DECISIONS"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Appending to a project that has no ADR yet behaves as a plain create — no
 * leading blank line, no error. */
TEST(tool_manage_adr_append_creates_when_absent) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-append-new", "/tmp/adr-append-new");
    cbm_mcp_server_set_project(srv, "adr-append-new");

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"adr-append-new\",\"mode\":\"append\","
                                     "\"content\":\"## PURPOSE\\nOnly entry.\\n\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "appended"));
    free(resp);

    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-append-new", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_STR_EQ(adr.content, "## PURPOSE\nOnly entry.\n");
    cbm_store_adr_free(&adr);

    cbm_mcp_server_free(srv);
    PASS();
}

/* append without content must fail loudly. Falling through to 'get' would hand
 * the caller a response that looks like a successful read while nothing was
 * written. */
TEST(tool_manage_adr_append_without_content_errors) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-append-empty", "/tmp/adr-append-empty");
    cbm_mcp_server_set_project(srv, "adr-append-empty");

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"adr-append-empty\",\"mode\":\"update\","
                                     "\"content\":\"## PURPOSE\\nUntouched.\\n\"}");
    ASSERT_NOT_NULL(resp);
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "manage_adr",
                               "{\"project\":\"adr-append-empty\",\"mode\":\"append\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing_content"));
    free(resp);

    /* And the stored ADR is untouched. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-append-empty", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_STR_EQ(adr.content, "## PURPOSE\nUntouched.\n");
    cbm_store_adr_free(&adr);

    cbm_mcp_server_free(srv);
    PASS();
}

/* The mode must be advertised, otherwise callers never learn it exists and
 * keep paying for whole-document rewrites. */
TEST(tool_manage_adr_append_is_advertised) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    ASSERT_NOT_NULL(resp);
    const char *adr_tool = strstr(resp, "manage_adr");
    ASSERT_NOT_NULL(adr_tool);
    ASSERT_NOT_NULL(strstr(adr_tool, "append"));
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_mutation_guard_balances_success) {
    const char *project = "guard-adr-success";
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, project, "/tmp/guard-adr-success"), CBM_STORE_OK);
    cbm_mcp_server_set_project(srv, project);

    mcp_mutation_guard_probe_t probe = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"guard-adr-success\",\"mode\":\"update\","
                                     "\"content\":\"## PURPOSE\\nGuarded ADR.\\n\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    ASSERT_EQ(probe.begin_count, 1);
    ASSERT_EQ(probe.end_count, 1);
    ASSERT_STR_EQ(probe.begin_projects[0], project);
    ASSERT_STR_EQ(probe.end_projects[0], project);
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_mutation_guard_releases_on_missing_store) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-mcp-adr-guard-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-adr-missing";
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_mutation_guard_probe_t probe = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"guard-adr-missing\",\"mode\":\"get\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    ASSERT_EQ(probe.begin_count, 1);
    ASSERT_EQ(probe.end_count, 1);
    ASSERT_STR_EQ(probe.begin_projects[0], project);
    ASSERT_STR_EQ(probe.end_projects[0], project);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    cbm_rmdir(cache);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    PASS();
}

/* A raw cbm_mcp_handle_tool() call is still one request lifetime. Cancellation
 * published from inside a non-pipeline handler must therefore be accepted,
 * observed before the write, and retired at completion so the next raw request
 * on the same server is not poisoned. */
TEST(tool_raw_dispatch_cancel_is_scoped_non_mutating_and_next_request_clean) {
    const char *project = "raw-cancel-adr";
    char root[256];
    snprintf(root, sizeof(root), "%s/cbm-mcp-raw-adr-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(root));
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, project, root), CBM_STORE_OK);
    cbm_mcp_server_set_project(srv, project);

    mcp_mutation_guard_probe_t probe = {
        .cancel_on_begin_call = 1,
        .cancel_server = srv,
    };
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char *cancelled_response =
        cbm_mcp_handle_tool(srv, "manage_adr",
                            "{\"project\":\"raw-cancel-adr\",\"mode\":\"update\","
                            "\"content\":\"## PURPOSE\\nMUST NOT COMMIT.\\n\"}");
    bool cancellation_reported = cancelled_response && strstr(cancelled_response, "cancelled") &&
                                 strstr(cancelled_response, "\"isError\":true");

    cbm_adr_t cancelled_adr = {0};
    int cancelled_lookup = cbm_store_adr_get(store, project, &cancelled_adr);
    if (cancelled_lookup == CBM_STORE_OK) {
        cbm_store_adr_free(&cancelled_adr);
    }

    char *next_response =
        cbm_mcp_handle_tool(srv, "manage_adr",
                            "{\"project\":\"raw-cancel-adr\",\"mode\":\"update\","
                            "\"content\":\"## PURPOSE\\nClean next request.\\n\"}");
    bool next_response_clean = next_response && strstr(next_response, "updated") &&
                               !strstr(next_response, "cancelled") &&
                               !strstr(next_response, "\"isError\":true");
    cbm_adr_t next_adr = {0};
    int next_lookup = cbm_store_adr_get(store, project, &next_adr);
    bool next_write_committed = next_lookup == CBM_STORE_OK && next_adr.content &&
                                strstr(next_adr.content, "Clean next request") &&
                                !strstr(next_adr.content, "MUST NOT COMMIT");
    if (next_lookup == CBM_STORE_OK) {
        cbm_store_adr_free(&next_adr);
    }

    free(cancelled_response);
    free(next_response);
    cbm_mcp_server_free(srv);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(probe.cancel_attempted);
    ASSERT_TRUE(probe.cancel_accepted);
    ASSERT_TRUE(cancellation_reported);
    ASSERT_EQ(cancelled_lookup, CBM_STORE_NOT_FOUND);
    ASSERT_TRUE(next_response_clean);
    ASSERT_TRUE(next_write_committed);
    ASSERT_EQ(probe.begin_count, 2);
    ASSERT_EQ(probe.end_count, 2);
    ASSERT_STR_EQ(probe.begin_projects[0], project);
    ASSERT_STR_EQ(probe.end_projects[0], project);
    ASSERT_STR_EQ(probe.begin_projects[1], project);
    ASSERT_STR_EQ(probe.end_projects[1], project);
    PASS();
}

/* The daemon publishes its transport request before entering MCP dispatch. A
 * disconnect in that narrow interval must remain latched through the nested
 * raw tool scope instead of being erased at dispatch entry. */
TEST(tool_outer_request_scope_preserves_predispatch_cancel) {
    const char *project = "outer-scope-cancel-adr";
    char root[256];
    (void)snprintf(root, sizeof(root), "%s/cbm-mcp-outer-cancel-XXXXXX", cbm_tmpdir());
    bool root_created = cbm_mkdtemp(root) != NULL;
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *store = cbm_mcp_server_store(srv);
    bool project_ready =
        root_created && store && cbm_store_upsert_project(store, project, root) == CBM_STORE_OK;
    cbm_mcp_server_set_project(srv, project);
    bool outer_scope = project_ready && cbm_mcp_server_request_scope_begin(srv);
    bool cancel_accepted = outer_scope && cbm_mcp_server_cancel_active(srv);
    char *cancelled_response =
        cancel_accepted
            ? cbm_mcp_handle_tool(srv, "manage_adr",
                                  "{\"project\":\"outer-scope-cancel-adr\","
                                  "\"mode\":\"update\",\"content\":\"MUST NOT COMMIT\"}")
            : NULL;
    bool cancellation_reported = cancelled_response && strstr(cancelled_response, "cancelled") &&
                                 strstr(cancelled_response, "\"isError\":true");
    cbm_mcp_server_request_scope_end(srv);

    char *next_response = srv ? cbm_mcp_handle_tool(srv, "ingest_traces", "{\"traces\":[]}") : NULL;
    bool next_response_clean = next_response && strstr(next_response, "accepted") &&
                               !strstr(next_response, "cancelled") &&
                               !strstr(next_response, "\"isError\":true");

    free(cancelled_response);
    free(next_response);
    cbm_mcp_server_free(srv);
    (void)cbm_rmdir(root);

    ASSERT_TRUE(root_created);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(project_ready);
    ASSERT_TRUE(outer_scope);
    ASSERT_TRUE(cancel_accepted);
    ASSERT_TRUE(cancellation_reported);
    ASSERT_TRUE(next_response_clean);
    PASS();
}

/* Publish cancellation from the local index mutation guard: the request scope
 * must already be active, and the cancellation must either stop before
 * pipeline admission or remain set through pipeline binding. No project DB may
 * be published, and the following request must start with a clean token. */
TEST(tool_index_repository_early_raw_cancel_survives_index_entry) {
    char cache[256];
    char repo[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-raw-index-cache-XXXXXX", cbm_tmpdir());
    snprintf(repo, sizeof(repo), "%s/cbm-mcp-raw-index-repo-XXXXXX", cbm_tmpdir());
    bool cache_created = cbm_mkdtemp(cache) != NULL;
    bool repo_created = cbm_mkdtemp(repo) != NULL;

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    if (cache_created) {
        cbm_setenv("CBM_CACHE_DIR", cache, 1);
    }

    char *project = repo_created ? cbm_project_name_from_path(repo) : NULL;
    cbm_mcp_server_t *srv =
        cache_created && repo_created && project ? cbm_mcp_server_new(NULL) : NULL;
    mcp_mutation_guard_probe_t probe = {
        .cancel_on_begin_call = 1,
        .cancel_server = srv,
    };
    if (srv) {
        cbm_mcp_server_set_background_tasks(srv, false);
        cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                                  mcp_mutation_guard_probe_end, &probe);
    }

    char args[CBM_SZ_1K];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", repo);
    char *cancelled_response = srv ? cbm_mcp_handle_tool(srv, "index_repository", args) : NULL;
    bool cancellation_reported = cancelled_response && strstr(cancelled_response, "cancelled") &&
                                 strstr(cancelled_response, "\"isError\":true");

    char db_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project ? project : "missing-project");
    bool no_project_published = !cbm_file_exists(db_path);

    char *next_response = srv ? cbm_mcp_handle_tool(srv, "ingest_traces", "{\"traces\":[]}") : NULL;
    bool next_response_clean = next_response && strstr(next_response, "accepted") &&
                               !strstr(next_response, "cancelled") &&
                               !strstr(next_response, "\"isError\":true");

    free(cancelled_response);
    free(next_response);
    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    if (cache_created) {
        (void)cbm_rmdir(cache);
    }
    if (repo_created) {
        (void)cbm_rmdir(repo);
    }
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    free(project);

    ASSERT_TRUE(cache_created);
    ASSERT_TRUE(repo_created);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(probe.cancel_attempted);
    ASSERT_TRUE(probe.cancel_accepted);
    ASSERT_TRUE(cancellation_reported);
    ASSERT_EQ(probe.begin_count, 1);
    ASSERT_EQ(probe.end_count, 1);
    ASSERT_TRUE(no_project_published);
    ASSERT_TRUE(next_response_clean);
    PASS();
}

static bool mcp_cross_repo_create_project_store(const char *cache, const char *project,
                                                const char *root_path) {
    char db_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    cbm_store_t *store = cbm_store_open_path(db_path);
    if (!store) {
        return false;
    }
    bool created = cbm_store_upsert_project(store, project, root_path) == CBM_STORE_OK;
    cbm_store_close(store);
    return created;
}

/* Seed exactly one HTTP route match without invoking the indexing pipeline.
 * This keeps the duplicate-target regression fast and makes a doubled result
 * count observable instead of relying on an empty (zero-edge) scan. */
static bool mcp_cross_repo_seed_http_match(const char *cache, const char *source_project,
                                           const char *target_project, const char *root_path) {
    char source_path[CBM_SZ_1K];
    char target_path[CBM_SZ_1K];
    snprintf(source_path, sizeof(source_path), "%s/%s.db", cache, source_project);
    snprintf(target_path, sizeof(target_path), "%s/%s.db", cache, target_project);

    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }

    bool ok = cbm_store_upsert_project(source, source_project, root_path) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, root_path) == CBM_STORE_OK;

    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "call_once",
                         .qualified_name = "cross.source.call_once",
                         .file_path = "client.c",
                         .start_line = 1,
                         .end_line = 2};
    cbm_node_t local_route = {.project = source_project,
                              .label = "Route",
                              .name = "GET /dedupe",
                              .qualified_name = "__route__GET__/dedupe",
                              .file_path = "client.c",
                              .start_line = 3,
                              .end_line = 3};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    int64_t local_route_id = ok ? cbm_store_upsert_node(source, &local_route) : 0;
    cbm_edge_t http_call = {.project = source_project,
                            .source_id = caller_id,
                            .target_id = local_route_id,
                            .type = "HTTP_CALLS",
                            .properties_json = "{\"url_path\":\"/dedupe\",\"method\":\"GET\"}"};
    ok = ok && caller_id > 0 && local_route_id > 0 && cbm_store_insert_edge(source, &http_call) > 0;

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = "GET /dedupe",
                               .qualified_name = "__route__GET__/dedupe",
                               .file_path = "server.c",
                               .start_line = 3,
                               .end_line = 3};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "handle_once",
                          .qualified_name = "cross.target.handle_once",
                          .file_path = "server.c",
                          .start_line = 1,
                          .end_line = 2};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;

    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

TEST(tool_cross_repo_mutation_guard_sorts_dedupes_and_unwinds) {
    char repo[256];
    snprintf(repo, sizeof(repo), "/tmp/cbm-mcp-cross-guard-XXXXXX");
    if (!cbm_mkdtemp(repo)) {
        PASS();
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, repo, NULL));

    mcp_mutation_guard_probe_t probe = {.deny_begin_call = 3};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"zzz-target\",\"000-target\",\"zzz-target\"]}",
             repo);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "blocked"));

    /* The source plus two unique targets are acquired in lexical order. The
     * third acquisition is denied, so only the first two are unwound. */
    ASSERT_EQ(probe.begin_count, 3);
    ASSERT_TRUE(strcmp(probe.begin_projects[0], probe.begin_projects[1]) < 0);
    ASSERT_TRUE(strcmp(probe.begin_projects[1], probe.begin_projects[2]) < 0);
    int low_target_count = 0;
    int high_target_count = 0;
    for (int i = 0; i < probe.begin_count; i++) {
        low_target_count += strcmp(probe.begin_projects[i], "000-target") == 0;
        high_target_count += strcmp(probe.begin_projects[i], "zzz-target") == 0;
    }
    ASSERT_EQ(low_target_count, 1);
    ASSERT_EQ(high_target_count, 1);
    ASSERT_EQ(probe.end_count, 2);
    ASSERT_STR_EQ(probe.end_projects[0], probe.begin_projects[1]);
    ASSERT_STR_EQ(probe.end_projects[1], probe.begin_projects[0]);
    free(resp);

    cbm_mcp_server_free(srv);
    cbm_rmdir(repo);
    PASS();
}

static unsigned char mcp_test_ascii_casefold(unsigned char ch) {
    return ch >= 'A' && ch <= 'Z' ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static bool mcp_test_project_keys_equivalent(const char *left, const char *right) {
    if (!left || !right) {
        return left == right;
    }
    while (*left && *right) {
        if (mcp_test_ascii_casefold((unsigned char)*left) !=
            mcp_test_ascii_casefold((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == *right;
}

/* Project-lock keys ASCII-fold A-Z, so case aliases must be one lease here too.
 * Otherwise Foo + foo self-deadlocks, and two requests whose raw strcmp order
 * differs can acquire the same OS locks in opposite (ABBA) order. Keep the
 * original spellings: folding is only the comparison key, not a lookup value. */
TEST(tool_cross_repo_mutation_guard_casefolds_aliases_and_order) {
    char repo[256];
    snprintf(repo, sizeof(repo), "/tmp/cbm-mcp-cross-case-guard-XXXXXX");
    if (!cbm_mkdtemp(repo)) {
        PASS();
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, repo, NULL));

    mcp_mutation_guard_probe_t first = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &first);
    char first_args[CBM_SZ_2K];
    snprintf(first_args, sizeof(first_args),
             "{\"repo_path\":\"%s\",\"name\":\"Zulu\","
             "\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"Foo\",\"foo\",\"Alpha\"]}",
             repo);
    char *first_resp = cbm_mcp_handle_tool(srv, "index_repository", first_args);
    ASSERT_NOT_NULL(first_resp);
    free(first_resp);

    mcp_mutation_guard_probe_t second = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &second);
    char second_args[CBM_SZ_2K];
    snprintf(second_args, sizeof(second_args),
             "{\"repo_path\":\"%s\",\"name\":\"zULU\","
             "\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"foo\",\"ALPHA\",\"FOO\"]}",
             repo);
    char *second_resp = cbm_mcp_handle_tool(srv, "index_repository", second_args);
    ASSERT_NOT_NULL(second_resp);
    free(second_resp);

    ASSERT_EQ(first.begin_count, 3);
    ASSERT_EQ(first.end_count, 3);
    ASSERT_EQ(second.begin_count, 3);
    ASSERT_EQ(second.end_count, 3);
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(
            mcp_test_project_keys_equivalent(first.begin_projects[i], second.begin_projects[i]));
        ASSERT_TRUE(
            mcp_test_project_keys_equivalent(first.end_projects[i], first.begin_projects[2 - i]));
        ASSERT_TRUE(
            mcp_test_project_keys_equivalent(second.end_projects[i], second.begin_projects[2 - i]));
    }
    ASSERT_STR_EQ(first.begin_projects[0], "Alpha");
    ASSERT_STR_EQ(first.begin_projects[1], "Foo");
    ASSERT_STR_EQ(first.begin_projects[2], "Zulu");
    ASSERT_STR_EQ(second.begin_projects[0], "ALPHA");
    ASSERT_STR_EQ(second.begin_projects[1], "FOO");
    ASSERT_STR_EQ(second.begin_projects[2], "zULU");

    cbm_mcp_server_free(srv);
    cbm_rmdir(repo);
    PASS();
}

/* A wildcard means "all projects" and therefore cannot be combined with a
 * named target. Accepting the mixed form both obscures caller intent and lets
 * the cross-repo pass create/use a literal "*.db" target on POSIX. Validation
 * must happen before any project mutation lease is acquired. */
TEST(tool_cross_repo_rejects_wildcard_mixed_with_named_targets) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-wildcard-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char *project = cbm_project_name_from_path(cache);
    ASSERT_NOT_NULL(project);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, cache, NULL));

    mcp_mutation_guard_probe_t probe = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"*\",\"named-target\"]}",
             cache);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool rejected = resp && strstr(resp, "\"isError\":true") != NULL;
    bool explained = resp && strstr(resp, "target_projects") && strstr(resp, "*") &&
                     (strstr(resp, "only") || strstr(resp, "combin"));
    int begin_count = probe.begin_count;
    int end_count = probe.end_count;

    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    cleanup_project_db(cache, "*");
    cleanup_project_db(cache, "named-target");
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(rejected);
    ASSERT_TRUE(explained);
    ASSERT_EQ(begin_count, 0);
    ASSERT_EQ(end_count, 0);
    PASS();
}

/* Cancellation can arrive while the final mutation lease is being acquired.
 * The cross-repo operation must advertise itself through cancel_active(),
 * observe the pending cancellation before doing cross-project writes, and
 * unwind every lease it acquired. */
TEST(tool_cross_repo_checks_cancellation_after_acquiring_leases) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-cancel-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char *project = cbm_project_name_from_path(cache);
    ASSERT_NOT_NULL(project);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, cache, NULL));

    mcp_mutation_guard_probe_t probe = {
        .cancel_on_begin_call = 3,
        .cancel_server = srv,
    };
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);

    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"000-cancel-target\",\"zzz-cancel-target\"]}",
             cache);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool response_cancelled = resp && strstr(resp, "cancelled") != NULL;
    bool cancel_attempted = probe.cancel_attempted;
    bool cancel_accepted = probe.cancel_accepted;
    int begin_count = probe.begin_count;
    int end_count = probe.end_count;
    bool reverse_unwind = begin_count == 3 && end_count == 3 &&
                          strcmp(probe.end_projects[0], probe.begin_projects[2]) == 0 &&
                          strcmp(probe.end_projects[1], probe.begin_projects[1]) == 0 &&
                          strcmp(probe.end_projects[2], probe.begin_projects[0]) == 0;

    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    cleanup_project_db(cache, "000-cancel-target");
    cleanup_project_db(cache, "zzz-cancel-target");
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(cancel_attempted);
    ASSERT_TRUE(cancel_accepted);
    ASSERT_TRUE(response_cancelled);
    ASSERT_EQ(begin_count, 3);
    ASSERT_EQ(end_count, 3);
    ASSERT_TRUE(reverse_unwind);
    PASS();
}

/* cbm_store_open_path() creates its path. Cross-repo validation must therefore
 * reject an absent source or named target before the matcher opens either one;
 * otherwise a typo silently becomes a valid-looking empty project database. */
TEST(tool_cross_repo_missing_inputs_fail_without_creating_ghost_databases) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-missing-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char *source_project = cbm_project_name_from_path(cache);
    ASSERT_NOT_NULL(source_project);
    const char *existing_target = "existing-cross-target";
    const char *missing_target = "missing-cross-target";
    ASSERT_TRUE(mcp_cross_repo_create_project_store(cache, existing_target, cache));

    char source_db_path[CBM_SZ_1K];
    char missing_target_db_path[CBM_SZ_1K];
    snprintf(source_db_path, sizeof(source_db_path), "%s/%s.db", cache, source_project);
    snprintf(missing_target_db_path, sizeof(missing_target_db_path), "%s/%s.db", cache,
             missing_target);
    ASSERT_FALSE(cbm_file_exists(source_db_path));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, cache, NULL));

    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"%s\"]}",
             cache, existing_target);
    char *source_resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool source_failed = source_resp && strstr(source_resp, "\"isError\":true");
    bool source_reported =
        source_resp && (strstr(source_resp, "not indexed") || strstr(source_resp, "not found") ||
                        strstr(source_resp, "missing"));
    bool source_ghost_created = cbm_file_exists(source_db_path);
    free(source_resp);

    cleanup_project_db(cache, source_project);
    ASSERT_TRUE(mcp_cross_repo_create_project_store(cache, source_project, cache));
    ASSERT_FALSE(cbm_file_exists(missing_target_db_path));

    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"%s\"]}",
             cache, missing_target);
    char *target_resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool target_failed = target_resp && strstr(target_resp, "\"isError\":true");
    bool target_reported =
        target_resp && (strstr(target_resp, "not indexed") || strstr(target_resp, "not found") ||
                        strstr(target_resp, "missing"));
    bool target_ghost_created = cbm_file_exists(missing_target_db_path);
    free(target_resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, source_project);
    cleanup_project_db(cache, existing_target);
    cleanup_project_db(cache, missing_target);
    free(source_project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(source_failed);
    ASSERT_TRUE(source_reported);
    ASSERT_FALSE(source_ghost_created);
    ASSERT_TRUE(target_failed);
    ASSERT_TRUE(target_reported);
    ASSERT_FALSE(target_ghost_created);
    PASS();
}

/* Named targets are a set, not a work list. A duplicate must be leased,
 * scanned, and counted once; the fixture provides one real edge so the result
 * counters cannot pass vacuously at zero. */
TEST(tool_cross_repo_dedupes_targets_before_scanning_and_counting) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-dedupe-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char *source_project = cbm_project_name_from_path(cache);
    ASSERT_NOT_NULL(source_project);
    const char *target_project = "cross-dedupe-target";
    ASSERT_TRUE(mcp_cross_repo_seed_http_match(cache, source_project, target_project, cache));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, cache, NULL));

    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"%s\",\"%s\"]}",
             cache, target_project, target_project);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool succeeded = resp && strstr(resp, "\"isError\":true") == NULL;
    bool scanned_once = response_contains_json_fragment(resp, "\"projects_scanned\":1");
    bool counted_once = response_contains_json_fragment(resp, "\"cross_http_calls\":1") &&
                        response_contains_json_fragment(resp, "\"total_cross_edges\":1");

    char source_db_path[CBM_SZ_1K];
    char target_db_path[CBM_SZ_1K];
    snprintf(source_db_path, sizeof(source_db_path), "%s/%s.db", cache, source_project);
    snprintf(target_db_path, sizeof(target_db_path), "%s/%s.db", cache, target_project);
    cbm_store_t *source = cbm_store_open_path_query(source_db_path);
    cbm_store_t *target = cbm_store_open_path_query(target_db_path);
    int source_cross_edges =
        source ? cbm_store_count_edges_by_type(source, source_project, "CROSS_HTTP_CALLS") : -1;
    int target_cross_edges =
        target ? cbm_store_count_edges_by_type(target, target_project, "CROSS_HTTP_CALLS") : -1;
    cbm_store_close(source);
    cbm_store_close(target);

    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, source_project);
    cleanup_project_db(cache, target_project);
    free(source_project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(succeeded);
    ASSERT_TRUE(scanned_once);
    ASSERT_TRUE(counted_once);
    ASSERT_EQ(source_cross_edges, 1);
    ASSERT_EQ(target_cross_edges, 1);
    PASS();
}

/* `name` is the documented index project-name override and must identify the
 * cross-repo source too. Deriving from repo_path here makes custom-named
 * projects impossible to rescan even though ordinary indexing created them. */
TEST(tool_cross_repo_honors_source_name_override) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-cross-name-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *source_project = "cross-custom-source";
    const char *target_project = "cross-custom-target";
    ASSERT_TRUE(mcp_cross_repo_seed_http_match(cache, source_project, target_project, cache));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    ASSERT_TRUE(cbm_mcp_server_set_session_context(srv, cache, NULL));
    char args[CBM_SZ_2K];
    snprintf(args, sizeof(args),
             "{\"repo_path\":\"%s\",\"name\":\"%s\","
             "\"mode\":\"cross-repo-intelligence\","
             "\"target_projects\":[\"%s\"]}",
             cache, source_project, target_project);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    bool succeeded = resp && !response_contains_json_fragment(resp, "\"isError\":true") &&
                     response_contains_json_fragment(resp, "\"cross_http_calls\":1");

    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, source_project);
    cleanup_project_db(cache, target_project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(succeeded);
    PASS();
}

/* Corrupt-store quarantine renames/unlinks the project DB and sidecars, so it
 * is a mutation even when resolve_store() was reached by a query tool. The
 * query path needs one balanced lease; manage_adr already owns that project
 * lease and must not acquire a nested second lease during the same cleanup. */
TEST(tool_corrupt_store_cleanup_guard_is_balanced_and_not_nested) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-guard-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-project";
    char db_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);

    ASSERT_TRUE(mcp_make_corrupt_project_store(cache, project));
    cbm_mcp_server_t *query_srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(query_srv);
    mcp_mutation_guard_probe_t query_probe = {
        .observed_db_path = db_path,
    };
    cbm_mcp_server_set_project_mutation_guard(query_srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &query_probe);

    char *resp =
        cbm_mcp_handle_tool(query_srv, "search_graph",
                            "{\"project\":\"guard-corrupt-project\",\"name_pattern\":\".*\"}");
    free(resp);
    cbm_mcp_server_free(query_srv);
    char query_backup_path[CBM_SZ_1K];
    int query_backup_count =
        mcp_find_corrupt_backups(cache, project, query_backup_path, sizeof(query_backup_path));
    bool query_quarantined =
        !cbm_file_exists(db_path) && query_backup_count == 1 && query_backup_path[0] != '\0';

    /* Replant the same deterministic corruption to exercise manage_adr's
     * already-held lease independently from the query server above. */
    mcp_cleanup_corrupt_backups(cache, project);
    ASSERT_TRUE(mcp_make_corrupt_project_store(cache, project));
    cbm_mcp_server_t *adr_srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(adr_srv);
    mcp_mutation_guard_probe_t adr_probe = {
        .observed_db_path = db_path,
    };
    cbm_mcp_server_set_project_mutation_guard(adr_srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &adr_probe);
    resp = cbm_mcp_handle_tool(adr_srv, "manage_adr",
                               "{\"project\":\"guard-corrupt-project\",\"mode\":\"get\"}");
    free(resp);
    cbm_mcp_server_free(adr_srv);
    char adr_backup_path[CBM_SZ_1K];
    int adr_backup_count =
        mcp_find_corrupt_backups(cache, project, adr_backup_path, sizeof(adr_backup_path));
    bool adr_quarantined =
        !cbm_file_exists(db_path) && adr_backup_count == 1 && adr_backup_path[0] != '\0';

    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(query_quarantined);
    ASSERT_EQ(query_probe.begin_count, 1);
    ASSERT_EQ(query_probe.end_count, 1);
    ASSERT_STR_EQ(query_probe.begin_projects[0], project);
    ASSERT_STR_EQ(query_probe.end_projects[0], project);
    ASSERT_TRUE(query_probe.db_exists_at_begin);
    ASSERT_FALSE(query_probe.db_exists_at_end);
    ASSERT_TRUE(adr_quarantined);
    ASSERT_EQ(adr_probe.begin_count, 1);
    ASSERT_EQ(adr_probe.end_count, 1);
    ASSERT_STR_EQ(adr_probe.begin_projects[0], project);
    ASSERT_STR_EQ(adr_probe.end_projects[0], project);
    ASSERT_TRUE(adr_probe.db_exists_at_begin);
    ASSERT_FALSE(adr_probe.db_exists_at_end);
    PASS();
}

/* Integrity is checked before the lease is requested, but quarantine itself
 * must fail closed when that lease is denied. In particular, a rejected query
 * may not remove either a recoverable DB generation or its committed WAL. */
TEST(tool_corrupt_store_cleanup_guard_denial_preserves_db_and_wal) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-denied-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-denied";
    char db_path[CBM_SZ_1K];
    char wal_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    cbm_store_t *writer = mcp_open_corrupt_project_store_with_wal(cache, project);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(cbm_file_exists(db_path));
    ASSERT_TRUE(cbm_file_exists(wal_path));

    long db_len = 0;
    long wal_len = 0;
    unsigned char *db_before = mcp_read_file_bytes(db_path, &db_len);
    unsigned char *wal_before = mcp_read_file_bytes(wal_path, &wal_len);
    ASSERT_NOT_NULL(db_before);
    ASSERT_NOT_NULL(wal_before);
    ASSERT_TRUE(db_len > 0);
    ASSERT_TRUE(wal_len > 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_mutation_guard_probe_t probe = {.deny_begin_call = 1};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph", "{\"project\":\"guard-corrupt-denied\",\"name_pattern\":\".*\"}");

    bool db_unchanged = mcp_file_matches_snapshot(db_path, db_before, db_len);
    bool wal_unchanged = mcp_file_matches_snapshot(wal_path, wal_before, wal_len);
    char unexpected_backup[CBM_SZ_1K];
    int backup_count =
        mcp_find_corrupt_backups(cache, project, unexpected_backup, sizeof(unexpected_backup));
    int artifact_count = mcp_count_corrupt_artifacts(cache, project);
    int begin_count = probe.begin_count;
    int end_count = probe.end_count;
    bool guarded_project = begin_count == 1 && strcmp(probe.begin_projects[0], project) == 0;

    free(resp);
    cbm_mcp_server_free(srv);
    free(db_before);
    free(wal_before);
    cbm_store_close(writer);
    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_EQ(begin_count, 1);
    ASSERT_EQ(end_count, 0);
    ASSERT_TRUE(guarded_project);
    ASSERT_TRUE(db_unchanged);
    ASSERT_TRUE(wal_unchanged);
    ASSERT_EQ(backup_count, 0);
    ASSERT_EQ(artifact_count, 0);
    PASS();
}

/* Another session may publish a good generation while this query waits for
 * the mutation lease. Cleanup must re-open and re-check the path after lease
 * acquisition; quarantining based on the stale pre-wait handle loses the new
 * generation and returns a false "not indexed" result. */
TEST(tool_corrupt_store_cleanup_rechecks_generation_after_guard_wait) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-recheck-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-recheck";
    const char *replacement_root = "/tmp/guard-corrupt-replacement";
    char db_path[CBM_SZ_1K];
    char replacement_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    snprintf(replacement_path, sizeof(replacement_path), "%s/%s.replacement.db", cache, project);
    ASSERT_TRUE(mcp_make_corrupt_project_store(cache, project));
    ASSERT_TRUE(mcp_make_valid_project_store_at(replacement_path, project, replacement_root));

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_replacing_mutation_guard_t replacement = {
        .replacement_path = replacement_path,
        .live_path = db_path,
    };
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_replacing_mutation_guard_begin,
                                              mcp_replacing_mutation_guard_end, &replacement);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph", "{\"project\":\"guard-corrupt-recheck\",\"name_pattern\":\".*\"}");
    bool response_used_replacement =
        resp && !response_contains_json_fragment(resp, "\"isError\":true");
    free(resp);
    cbm_mcp_server_free(srv);

    cbm_store_t *check = cbm_store_open_path_query(db_path);
    bool valid_generation = check && cbm_store_check_integrity(check);
    cbm_project_t stored_project = {0};
    bool replacement_root_visible =
        check && cbm_store_get_project(check, project, &stored_project) == CBM_STORE_OK &&
        stored_project.root_path && strcmp(stored_project.root_path, replacement_root) == 0;
    cbm_project_free_fields(&stored_project);
    cbm_store_close(check);
    char unexpected_backup[CBM_SZ_1K];
    int backup_count =
        mcp_find_corrupt_backups(cache, project, unexpected_backup, sizeof(unexpected_backup));
    bool live_exists = cbm_file_exists(db_path);
    bool replacement_consumed = !cbm_file_exists(replacement_path);
    int begin_count = replacement.guard.begin_count;
    int end_count = replacement.guard.end_count;
    bool guarded_project = begin_count == 1 && end_count == 1 &&
                           strcmp(replacement.guard.begin_projects[0], project) == 0 &&
                           strcmp(replacement.guard.end_projects[0], project) == 0;
    bool replacement_attempted = replacement.replacement_attempted;
    bool replacement_succeeded = replacement.replacement_succeeded;

    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    cbm_unlink(replacement_path);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(replacement_attempted);
    ASSERT_TRUE(replacement_succeeded);
    ASSERT_TRUE(guarded_project);
    ASSERT_TRUE(response_used_replacement);
    ASSERT_TRUE(live_exists);
    ASSERT_TRUE(replacement_consumed);
    ASSERT_TRUE(valid_generation);
    ASSERT_TRUE(replacement_root_visible);
    ASSERT_EQ(backup_count, 0);
    PASS();
}

/* A fixed `.corrupt` destination is itself user recovery data. A later
 * quarantine must retain it byte-for-byte and choose a distinct backup name
 * rather than unlinking the previous incident before rename. */
TEST(tool_corrupt_store_cleanup_preserves_existing_backup_and_uses_unique_name) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-unique-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-unique";
    char db_path[CBM_SZ_1K];
    char existing_backup_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    snprintf(existing_backup_path, sizeof(existing_backup_path), "%s.corrupt", db_path);
    ASSERT_TRUE(mcp_make_corrupt_project_store(cache, project));
    ASSERT_EQ(th_write_file(existing_backup_path, "previous-backup-must-survive\n"), 0);

    long existing_len = 0;
    unsigned char *existing_before = mcp_read_file_bytes(existing_backup_path, &existing_len);
    ASSERT_NOT_NULL(existing_before);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_mutation_guard_probe_t probe = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &probe);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph", "{\"project\":\"guard-corrupt-unique\",\"name_pattern\":\".*\"}");
    free(resp);
    cbm_mcp_server_free(srv);

    bool existing_unchanged =
        mcp_file_matches_snapshot(existing_backup_path, existing_before, existing_len);
    free(existing_before);
    char unique_backup_path[CBM_SZ_1K];
    int backup_count =
        mcp_find_corrupt_backups(cache, project, unique_backup_path, sizeof(unique_backup_path));
    cbm_store_t *quarantined =
        unique_backup_path[0] ? cbm_store_open_path_query(unique_backup_path) : NULL;
    bool unique_backup_is_corrupt = quarantined && !cbm_store_check_integrity(quarantined);
    cbm_store_close(quarantined);
    bool live_removed = !cbm_file_exists(db_path);
    int begin_count = probe.begin_count;
    int end_count = probe.end_count;
    bool guarded_project = begin_count == 1 && end_count == 1 &&
                           strcmp(probe.begin_projects[0], project) == 0 &&
                           strcmp(probe.end_projects[0], project) == 0;

    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(guarded_project);
    ASSERT_TRUE(existing_unchanged);
    ASSERT_EQ(backup_count, 2);
    ASSERT_TRUE(unique_backup_path[0] != '\0');
    ASSERT_TRUE(unique_backup_is_corrupt);
    ASSERT_TRUE(live_removed);
    PASS();
}

/* Deterministically fail immediately before atomic snapshot publication on
 * every platform. The incomplete pending copy must be removed while the live
 * DB and its committed WAL remain byte-for-byte untouched. */
TEST(tool_corrupt_store_cleanup_publish_failure_preserves_db_and_wal) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-publish-fail-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-publish-fail";
    char db_path[CBM_SZ_1K];
    char wal_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    cbm_store_t *writer = mcp_open_corrupt_project_store_with_wal(cache, project);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(cbm_file_exists(wal_path));

    long db_len = 0;
    long wal_len = 0;
    unsigned char *db_before = mcp_read_file_bytes(db_path, &db_len);
    unsigned char *wal_before = mcp_read_file_bytes(wal_path, &wal_len);
    ASSERT_NOT_NULL(db_before);
    ASSERT_NOT_NULL(wal_before);
    ASSERT_TRUE(db_len > 0);
    ASSERT_TRUE(wal_len > 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_mutation_guard_probe_t guard = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &guard);
    mcp_quarantine_hook_probe_t hook = {.deny_step = "before_snapshot_publish"};
    cbm_mcp_server_set_quarantine_test_hook(srv, mcp_quarantine_hook_probe, &hook);
    char *resp =
        cbm_mcp_handle_tool(srv, "search_graph",
                            "{\"project\":\"guard-corrupt-publish-fail\",\"name_pattern\":\".*\"}");

    bool db_unchanged = mcp_file_matches_snapshot(db_path, db_before, db_len);
    bool wal_unchanged = mcp_file_matches_snapshot(wal_path, wal_before, wal_len);
    char unexpected_backup[CBM_SZ_1K];
    int backup_count =
        mcp_find_corrupt_backups(cache, project, unexpected_backup, sizeof(unexpected_backup));
    int artifact_count = mcp_count_corrupt_artifacts(cache, project);
    int begin_count = guard.begin_count;
    int end_count = guard.end_count;
    bool guarded_project = begin_count == 1 && end_count == 1 &&
                           strcmp(guard.begin_projects[0], project) == 0 &&
                           strcmp(guard.end_projects[0], project) == 0;
    bool failed_at_publish =
        hook.call_count == 1 && strcmp(hook.steps[0], "before_snapshot_publish") == 0;

    free(resp);
    cbm_mcp_server_free(srv);
    free(db_before);
    free(wal_before);
    cbm_store_close(writer);
    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(failed_at_publish);
    ASSERT_TRUE(guarded_project);
    ASSERT_TRUE(db_unchanged);
    ASSERT_TRUE(wal_unchanged);
    ASSERT_EQ(backup_count, 0);
    ASSERT_EQ(artifact_count, 0);
    PASS();
}

/* Once the recovery snapshot is atomically visible, a crash/failure before
 * deleting the live generation may leave both copies. The live DB/WAL must be
 * unchanged, and the published backup must already contain committed WAL data
 * as one self-contained SQLite database. */
TEST(tool_corrupt_store_cleanup_publishes_complete_wal_snapshot_before_delete) {
    char cache[256];
    snprintf(cache, sizeof(cache), "%s/cbm-mcp-corrupt-after-publish-XXXXXX", cbm_tmpdir());
    ASSERT_NOT_NULL(cbm_mkdtemp(cache));

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "guard-corrupt-after-publish";
    char db_path[CBM_SZ_1K];
    char wal_path[CBM_SZ_1K];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    cbm_store_t *writer = mcp_open_corrupt_project_store_with_wal(cache, project);
    ASSERT_NOT_NULL(writer);
    ASSERT_TRUE(cbm_file_exists(wal_path));

    long db_len = 0;
    long wal_len = 0;
    unsigned char *db_before = mcp_read_file_bytes(db_path, &db_len);
    unsigned char *wal_before = mcp_read_file_bytes(wal_path, &wal_len);
    ASSERT_NOT_NULL(db_before);
    ASSERT_NOT_NULL(wal_before);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    mcp_mutation_guard_probe_t guard = {0};
    cbm_mcp_server_set_project_mutation_guard(srv, mcp_mutation_guard_probe_begin,
                                              mcp_mutation_guard_probe_end, &guard);
    mcp_quarantine_hook_probe_t hook = {.deny_step = "after_snapshot_publish"};
    cbm_mcp_server_set_quarantine_test_hook(srv, mcp_quarantine_hook_probe, &hook);
    char *resp = cbm_mcp_handle_tool(
        srv, "search_graph",
        "{\"project\":\"guard-corrupt-after-publish\",\"name_pattern\":\".*\"}");

    bool db_unchanged = mcp_file_matches_snapshot(db_path, db_before, db_len);
    bool wal_unchanged = mcp_file_matches_snapshot(wal_path, wal_before, wal_len);
    char backup_path[CBM_SZ_1K];
    int backup_count = mcp_find_corrupt_backups(cache, project, backup_path, sizeof(backup_path));
    int artifact_count = mcp_count_corrupt_artifacts(cache, project);
    cbm_store_t *snapshot = backup_path[0] ? cbm_store_open_path_query(backup_path) : NULL;
    cbm_project_t recovered = {0};
    bool recovered_wal_project =
        snapshot && cbm_store_get_project(snapshot, project, &recovered) == CBM_STORE_OK &&
        recovered.root_path && strcmp(recovered.root_path, "826") == 0;
    cbm_project_free_fields(&recovered);
    cbm_store_close(snapshot);
    char backup_wal[CBM_SZ_2K];
    char backup_shm[CBM_SZ_2K];
    snprintf(backup_wal, sizeof(backup_wal), "%s-wal", backup_path);
    snprintf(backup_shm, sizeof(backup_shm), "%s-shm", backup_path);
    bool snapshot_self_contained = !cbm_file_exists(backup_wal) && !cbm_file_exists(backup_shm);
    bool hook_order = hook.call_count == 2 &&
                      strcmp(hook.steps[0], "before_snapshot_publish") == 0 &&
                      strcmp(hook.steps[1], "after_snapshot_publish") == 0;
    bool guard_balanced = guard.begin_count == 1 && guard.end_count == 1 &&
                          strcmp(guard.begin_projects[0], project) == 0 &&
                          strcmp(guard.end_projects[0], project) == 0;

    free(resp);
    cbm_mcp_server_free(srv);
    free(db_before);
    free(wal_before);
    cbm_store_close(writer);
    mcp_cleanup_corrupt_backups(cache, project);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(hook_order);
    ASSERT_TRUE(guard_balanced);
    ASSERT_TRUE(db_unchanged);
    ASSERT_TRUE(wal_unchanged);
    ASSERT_EQ(backup_count, 1);
    ASSERT_EQ(artifact_count, 1);
    ASSERT_TRUE(recovered_wal_project);
    ASSERT_TRUE(snapshot_self_contained);
    PASS();
}

TEST(tool_index_repository_reports_store_backed_adr) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-adr-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Store-backed ADR metadata.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT(response_contains_json_fragment(resp, "\"adr_present\":true"));
    ASSERT_NULL(strstr(resp, "adr_hint"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Store-backed ADR metadata."));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_index_repository_dot_uses_absolute_project_key_and_preserves_adr) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-dot-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-dot-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return helper()\n\ndef helper():\n    return 1\n", fp);
    fclose(fp);

    char old_cwd[CBM_SZ_4K];
    ASSERT_NOT_NULL(cbm_getcwd(old_cwd, sizeof(old_cwd)));

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    ASSERT_EQ(cbm_chdir(tmp_dir), 0);
    char *resp =
        cbm_mcp_handle_tool(srv, "index_repository", "{\"repo_path\":\".\",\"mode\":\"fast\"}");
    ASSERT_EQ(cbm_chdir(old_cwd), 0);
    ASSERT_NOT_NULL(resp);
    if (!response_contains_json_fragment(resp, "\"status\":\"indexed\"")) {
        free(resp);
        cbm_mcp_server_free(srv);
        cleanup_project_db(cache, project);
        restore_cache_dir(saved_copy);
        free(saved_copy);
        free(project);
        remove(src_path);
        cbm_rmdir(cache);
        cbm_rmdir(tmp_dir);
        PASS();
    }
    ASSERT_NOT_NULL(strstr(resp, project));
    ASSERT(!response_contains_json_fragment(resp, "\"project\":\"root\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Dot-path ADR marker.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    ASSERT_EQ(cbm_chdir(tmp_dir), 0);
    resp = cbm_mcp_handle_tool(srv, "index_repository", "{\"repo_path\":\".\",\"mode\":\"fast\"}");
    ASSERT_EQ(cbm_chdir(old_cwd), 0);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT_NOT_NULL(strstr(resp, project));
    ASSERT(response_contains_json_fragment(resp, "\"adr_present\":true"));
    ASSERT(!response_contains_json_fragment(resp, "\"project\":\"root\""));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Dot-path ADR marker."));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_manage_adr_not_found_rich_error) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-missing-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"cbm-no-such-project-zzz\",\"mode\":\"get\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NOT_NULL(strstr(resp, "hint"));
    free(resp);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(tool_manage_adr_get_accepts_abs_path) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-abspath-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-abspath-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Abs-path normalization test.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", tmp_dir);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Abs-path normalization test."));
    ASSERT_NULL(strstr(resp, "or not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_manage_adr_get_accepts_symlink_path) {
#ifdef _WIN32
    PASS();
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-realpath-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-realpath-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    char link_path[320];
    snprintf(link_path, sizeof(link_path), "%s-link", tmp_dir);
    (void)unlink(link_path);
    if (symlink(tmp_dir, link_path) != 0) {
        cbm_rmdir(cache);
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", link_path);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT_NOT_NULL(strstr(resp, project));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Symlink-path normalization test.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", link_path);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Symlink-path normalization test."));
    ASSERT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    unlink(link_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
#endif
}

TEST(tool_detect_changes_not_found_rich_error) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-detect-missing-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_handle_tool(srv, "detect_changes", "{\"project\":\"cbm-no-such-project-zzz\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NOT_NULL(strstr(resp, "hint"));
    free(resp);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

/* detect_changes owns shell output through regular temporary files. An error
 * after opening that file must use fclose + unlink. The command hook then
 * rejects merge-base only when it reaches the contained subprocess helper, so
 * a raw popen regression bypasses the hook and fails this test. */
TEST(tool_detect_changes_contained_commands_clean_up_error_and_success) {
    char cache[512];
    (void)snprintf(cache, sizeof(cache), "%s/cbm-detect-contained-XXXXXX", cbm_tmpdir());
    bool cache_created = cbm_mkdtemp(cache) != NULL;
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    bool environment_ready = cache_created && cbm_setenv("CBM_CACHE_DIR", cache, 1) == 0;

    char root[CBM_SZ_4K] = {0};
    bool root_ready = cbm_getcwd(root, sizeof(root)) != NULL;
    const char *project = "detect-contained-project";
    cbm_mcp_server_t *srv = environment_ready && root_ready ? cbm_mcp_server_new(NULL) : NULL;
    bool server_ready = srv != NULL;
    cbm_store_t *store = srv ? cbm_mcp_server_store(srv) : NULL;
    bool project_ready = store && cbm_store_upsert_project(store, project, root) == CBM_STORE_OK;
    mcp_command_hook_probe_t command_probe = {.reject_merge_base = true};
    if (project_ready) {
        cbm_mcp_server_set_project(srv, project);
        cbm_mcp_server_set_command_test_hook(srv, mcp_command_hook_probe, &command_probe);
    }

    char *invalid_response =
        project_ready ? cbm_mcp_handle_tool(srv, "detect_changes",
                                            "{\"project\":\"detect-contained-project\","
                                            "\"base_branch\":\"HEAD\",\"scope\":\"files\","
                                            "\"direction\":\"sideways\"}")
                      : NULL;
    bool invalid_rejected = invalid_response && strstr(invalid_response, "invalid direction");
    char logs[640];
    (void)snprintf(logs, sizeof(logs), "%s/logs", cache);
    int artifacts_after_error =
        invalid_response ? mcp_count_directory_entries_with_prefix(logs, ".mcp-command-") : -1;

    char *rejected_response =
        project_ready ? cbm_mcp_handle_tool(srv, "detect_changes",
                                            "{\"project\":\"detect-contained-project\","
                                            "\"base_branch\":\"HEAD\",\"scope\":\"files\"}")
                      : NULL;
    bool containment_rejected =
        rejected_response && strstr(rejected_response, "contained command could not complete");
    int artifacts_after_rejection =
        rejected_response ? mcp_count_directory_entries_with_prefix(logs, ".mcp-command-") : -1;

    command_probe.reject_merge_base = false;
    char *success_response =
        project_ready ? cbm_mcp_handle_tool(srv, "detect_changes",
                                            "{\"project\":\"detect-contained-project\","
                                            "\"base_branch\":\"HEAD\",\"scope\":\"files\"}")
                      : NULL;
    bool merge_base_reported = success_response && strstr(success_response, "merge_base");
    int artifacts_after_success =
        success_response ? mcp_count_directory_entries_with_prefix(logs, ".mcp-command-") : -1;

    free(invalid_response);
    free(rejected_response);
    free(success_response);
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    bool cleaned = !cache_created || th_rmtree(cache) == 0;

    ASSERT_TRUE(cache_created);
    ASSERT_TRUE(environment_ready);
    ASSERT_TRUE(root_ready);
    ASSERT_TRUE(server_ready);
    ASSERT_TRUE(project_ready);
    ASSERT_TRUE(invalid_rejected);
    ASSERT_EQ(artifacts_after_error, 0);
    ASSERT_TRUE(containment_rejected);
    ASSERT_EQ(artifacts_after_rejection, 0);
    ASSERT_TRUE(merge_base_reported);
    ASSERT_EQ(artifacts_after_success, 0);
    ASSERT_EQ(command_probe.diff_calls, 3);
    ASSERT_EQ(command_probe.merge_base_calls, 2);
    ASSERT_TRUE(cleaned);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"ingest_traces\","
             "\"arguments\":{\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    ASSERT_NOT_NULL(strstr(resp, "traces_received"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":38,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"ingest_traces\","
                                   "\"arguments\":{\"traces\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Write sample source file */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "package main\n"
                "\n"
                "func HandleRequest() error {\n"
                "\treturn nil\n"
                "}\n"
                "\n"
                "func ProcessOrder(id int) {\n"
                "\t// process\n"
                "}\n"
                "\n"
                "func Run() {\n"
                "\t// server\n"
                "}\n");
    fclose(fp);

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    return srv;
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content) {
        /* Handle JSON-RPC wrapper: {"jsonrpc":...,"result":{"content":[...]}} */
        yyjson_val *rpc_result = yyjson_obj_get(root, "result");
        if (rpc_result) {
            content = yyjson_obj_get(rpc_result, "content");
        }
    }
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

static bool is_valid_json_response(const char *json) {
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

static bool snippet_source_has_replacement(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *source = yyjson_obj_get(root, "source");
    const char *source_str = yyjson_get_str(source);
    bool found = source_str && strstr(source_str, "\xEF\xBF\xBD");
    yyjson_doc_free(doc);
    return found;
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* No property-blob spill: the source IS the payload (signature and
     * docstring are literally in it); metrics live behind search_graph
     * fields=[...]. */
    ASSERT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NULL(strstr(resp, "\"return_type\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    /* GUARD (inverted since the compact-output change): the snippet response
     * carries the verbatim source plus location/degree/coverage metadata and
     * NOTHING from the node's property blob — no signature/return_type/
     * is_exported duplication, and never the fp/sp/bt similarity internals
     * (41% of the legacy response). */
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    ASSERT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NULL(strstr(resp, "\"is_exported\""));
    ASSERT_NULL(strstr(resp, "\"fp\""));
    ASSERT_NULL(strstr(resp, "\"bt\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_SourceInvalidUtf8 ────────────────────────────── */

TEST(snippet_source_invalid_utf8) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/project/main.go", tmp);
    FILE *fp = fopen(src_path, "wb");
    ASSERT_NOT_NULL(fp);
    const unsigned char source[] = {
        'p',  'a',  'c', 'k', 'a', 'g',  'e',  ' ',  'm',  'a',  'i',  'n', '\n', '\n',
        'f',  'u',  'n', 'c', ' ', 'H',  'a',  'n',  'd',  'l',  'e',  'R', 'e',  'q',
        'u',  'e',  's', 't', '(', ')',  ' ',  'e',  'r',  'r',  'o',  'r', ' ',  '{',
        '\n', '\t', '/', '/', ' ', 0xC0, 0xD4, 0xB7, 0xC2, '\n', '\t', 'r', 'e',  't',
        'u',  'r',  'n', ' ', 'n', 'i',  'l',  '\n', '}',  '\n'};
    ASSERT_EQ(fwrite(source, 1, sizeof(source), fp), sizeof(source));
    ASSERT_EQ(fclose(fp), 0);

    char *raw =
        cbm_mcp_handle_tool(srv, "get_code_snippet",
                            "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                            "\"project\":\"test-project\"}");
    ASSERT_TRUE(is_valid_json_response(raw));
    char *resp = extract_text_content(raw);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(is_valid_json_response(resp));
    ASSERT_NULL(strstr(resp, "\xC0\xD4"));
    ASSERT_NOT_NULL(strstr(resp, "HandleRequest"));
    ASSERT_NOT_NULL(strstr(resp, "return nil"));
    ASSERT_TRUE(snippet_source_has_replacement(resp));

    free(resp);
    free(raw);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_empty_string) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_jsonrpc_field) {
    /* jsonrpc field absent — parser defaults to "2.0" if method present */
    const char *line = "{\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_TRUE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_method) {
    /* method is required — should fail */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_string_id) {
    /* JSON-RPC §4: string and numeric ids are distinct. A string id is
     * preserved verbatim (issue #253), never coerced to a number. */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "99");
    ASSERT_STR_EQ(req.method, "tools/list");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_no_params) {
    /* Request with no params field — params_raw should be NULL */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(req.params_raw);
    ASSERT_EQ(req.id, 5);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_extra_whitespace) {
    /* Leading/trailing whitespace and internal spacing in JSON */
    const char *line = "  { \"jsonrpc\" : \"2.0\" , \"id\" : 7 , \"method\" : \"ping\" }  ";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(req.id, 7);
    ASSERT_STR_EQ(req.method, "ping");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_array_not_object) {
    /* JSON array at root — not a valid JSON-RPC request */
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("[1,2,3]", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_string_arg_empty_json) {
    /* Empty JSON string — yyjson_read fails → NULL */
    char *val = cbm_mcp_get_string_arg("", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_empty_object) {
    /* Valid JSON with no keys → NULL for any key */
    char *val = cbm_mcp_get_string_arg("{}", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_nested_value) {
    /* Value is an object, not a string → should return NULL */
    const char *args = "{\"config\":{\"nested\":true},\"name\":\"hello\"}";
    char *val = cbm_mcp_get_string_arg(args, "config");
    ASSERT_NULL(val); /* not a string type */
    val = cbm_mcp_get_string_arg(args, "name");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "hello");
    free(val);
    PASS();
}

TEST(mcp_get_string_arg_int_value) {
    /* Value is an integer, not a string → NULL */
    char *val = cbm_mcp_get_string_arg("{\"count\":42}", "count");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg_empty_json) {
    int val = cbm_mcp_get_int_arg("", "key", 99);
    ASSERT_EQ(val, 99);
    PASS();
}

TEST(mcp_get_int_arg_string_value) {
    /* Value is a string, not int → should return default */
    int val = cbm_mcp_get_int_arg("{\"limit\":\"ten\"}", "limit", 5);
    ASSERT_EQ(val, 5);
    PASS();
}

TEST(mcp_get_int_arg_bool_value) {
    /* Value is a bool, not int → default */
    int val = cbm_mcp_get_int_arg("{\"flag\":true}", "flag", -1);
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(mcp_get_bool_arg_empty_json) {
    bool val = cbm_mcp_get_bool_arg("", "key");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_bool_arg_int_value) {
    /* Value is int 1, not bool → should return false */
    bool val = cbm_mcp_get_bool_arg("{\"flag\":1}", "flag");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_tool_name_empty_json) {
    char *name = cbm_mcp_get_tool_name("");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_tool_name_missing_name) {
    char *name = cbm_mcp_get_tool_name("{\"arguments\":{}}");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_arguments_empty_json) {
    char *args = cbm_mcp_get_arguments("");
    ASSERT_NULL(args);
    PASS();
}

TEST(mcp_get_arguments_no_arguments_key) {
    /* No "arguments" key → returns "{}" */
    char *args = cbm_mcp_get_arguments("{\"name\":\"tool\"}");
    ASSERT_NOT_NULL(args);
    ASSERT_STR_EQ(args, "{}");
    free(args);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  FILE URI PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_http_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("http://example.com/path", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_ftp_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("ftp://server/file.txt", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_buffer_too_small) {
    char path[5]; /* only 5 bytes — path gets truncated */
    ASSERT_TRUE(cbm_parse_file_uri("file:///usr/local/bin", path, sizeof(path)));
    /* snprintf truncates to 4 chars + NUL */
    ASSERT_EQ(strlen(path), 4);
    ASSERT_STR_EQ(path, "/usr");
    PASS();
}

TEST(parse_file_uri_spaces_in_path) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/my%20project", path, sizeof(path)));
    /* Raw percent-encoding is preserved (not decoded) */
    ASSERT_STR_EQ(path, "/home/user/my%20project");
    PASS();
}

TEST(parse_file_uri_null_out_path) {
    /* NULL out_path — should not crash */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", NULL, 256));
    PASS();
}

TEST(parse_file_uri_zero_size) {
    char path[256] = "garbage";
    /* out_size=0 → should fail safely */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", path, 0));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_invalid_json) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(srv, "this is not json at all");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32700")); /* Parse error */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_empty_object) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Valid JSON but no method field → parse error */
    char *resp = cbm_mcp_server_handle(srv, "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_call_missing_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* tools/call with no tool name in params */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
                                   "\"params\":{\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about unknown/missing tool */
    ASSERT_NOT_NULL(strstr(resp, "\"id\":50"));
    ASSERT_TRUE(strstr(resp, "error") || strstr(resp, "isError") || strstr(resp, "unknown"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  POLL/GETLINE FILE* BUFFERING FIX
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>

/* Signal handler used by alarm() to abort the test if it hangs */
static void alarm_handler(int sig) {
    (void)sig;
    /* Writing to stderr is async-signal-safe */
    const char msg[] = "FAIL: mcp_server_run_rapid_messages timed out (>5s)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

TEST(mcp_server_run_rapid_messages) {
    /* Simulate a client sending initialize + notifications/initialized +
     * tools/list all at once (no delays), which exercises the FILE*
     * buffering fix: the first getline() over-reads kernel data into the
     * libc buffer; without the fix, subsequent poll() calls block for 60s.
     *
     * We use alarm(5) to abort the test process if the server hangs. */
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Write all 3 messages to the write end in one shot */
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
    ssize_t written = write(fds[1], msgs, strlen(msgs));
    ASSERT_TRUE(written > 0);
    close(fds[1]); /* EOF signals end of input to the server */

    FILE *in_fp = fdopen(fds[0], "r");
    ASSERT_NOT_NULL(in_fp);

    FILE *out_fp = tmpfile();
    ASSERT_NOT_NULL(out_fp);

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    return srv;
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content) {
        /* Handle JSON-RPC wrapper: {"jsonrpc":...,"result":{"content":[...]}} */
        yyjson_val *rpc_result = yyjson_obj_get(root, "result");
        if (rpc_result) {
            content = yyjson_obj_get(rpc_result, "content");
        }
    }
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

static bool is_valid_json_response(const char *json) {
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

static bool snippet_source_has_replacement(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *source = yyjson_obj_get(root, "source");
    const char *source_str = yyjson_get_str(source);
    bool found = source_str && strstr(source_str, "\xEF\xBF\xBD");
    yyjson_doc_free(doc);
    return found;
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* Enriched properties */
    ASSERT_NOT_NULL(strstr(resp, "\"signature\":\"func HandleRequest() error\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\":\"error\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    /* ── A: list_projects reports INTERNAL names; filters the ghost ── */
    char *list =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(strstr(list, "alpha704")); /* control */
    ASSERT_NOT_NULL(strstr(list, "beta704"));  /* internal name of drifted db (RED before) */
    ASSERT_NULL(strstr(list, "gamma704"));     /* filename must NOT be advertised (RED before) */
    ASSERT_NULL(strstr(list, "ghost704"));     /* 0-byte ghost filtered (RED before) */
    free(list);

    char *page = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"list_projects\","
             "\"arguments\":{\"offset\":0,\"limit\":1}}}");
    ASSERT_NOT_NULL(page);
    ASSERT_NOT_NULL(strstr(page, "\\\"total\\\":3"));
    ASSERT_NOT_NULL(strstr(page, "\\\"limit\\\":1"));
    ASSERT_NOT_NULL(strstr(page, "\\\"returned\\\":1"));
    ASSERT_NOT_NULL(strstr(page, "\\\"has_more\\\":true"));
    ASSERT_NOT_NULL(strstr(page, "alpha704"));
    ASSERT_NULL(strstr(page, "beta704"));
    ASSERT_NULL(strstr(page, "\\\"nodes\\\"")); /* details are opt-in */
    free(page);

    char *next_page = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"list_projects\","
             "\"arguments\":{\"offset\":1,\"limit\":1,\"metadata_only\":true}}}");
    ASSERT_NOT_NULL(next_page);
    ASSERT_NOT_NULL(strstr(next_page, "beta704"));
    ASSERT_NULL(strstr(next_page, "alpha704"));
    ASSERT_NULL(strstr(next_page, "\\\"nodes\\\"")); /* compatibility alias stays lean */
    free(next_page);

    char *details = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"list_projects\","
             "\"arguments\":{\"offset\":0,\"limit\":1,\"include_details\":true}}}");
    ASSERT_NOT_NULL(details);
    ASSERT_NOT_NULL(strstr(details, "\\\"nodes\\\""));
    free(details);

    /* ── B: the drifted project resolves by its INTERNAL name ──────── */
    char *q_beta = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"beta704\",\"name_pattern\":\"betaFunc704\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_beta);
    ASSERT_NOT_NULL(strstr(q_beta, "betaFunc704")); /* resolved + returned node (RED before) */
    ASSERT_NULL(strstr(q_beta, "not found"));       /* not the not-found error */
    free(q_beta);

    /* ── C: control project still resolves on the fast path ────────── */
    char *q_alpha = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"alpha704\",\"name_pattern\":\"alphaFunc704\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_alpha);
    ASSERT_NOT_NULL(strstr(q_alpha, "alphaFunc704"));
    free(q_alpha);

    /* ── D: the 0-byte ghost is NOT resolvable ─────────────────────── */
    char *q_ghost = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"ghost704\",\"name_pattern\":\".*\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_ghost);
    ASSERT_NOT_NULL(strstr(q_ghost, "not found"));
    free(q_ghost);

    /* ── E: addressing the drifted db by its FILENAME stays not-found ── */
    char *q_gamma = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"gamma704\",\"name_pattern\":\".*\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_gamma);
    ASSERT_NOT_NULL(strstr(q_gamma, "not found"));
    free(q_gamma);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NOT_NULL(strstr(resp, "\"is_exported\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_SourceInvalidUtf8 ────────────────────────────── */

TEST(snippet_source_invalid_utf8) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/project/main.go", tmp);
    FILE *fp = fopen(src_path, "wb");
    ASSERT_NOT_NULL(fp);
    const unsigned char source[] = {
        'p',  'a',  'c', 'k', 'a', 'g',  'e',  ' ',  'm',  'a',  'i',  'n', '\n', '\n',
        'f',  'u',  'n', 'c', ' ', 'H',  'a',  'n',  'd',  'l',  'e',  'R', 'e',  'q',
        'u',  'e',  's', 't', '(', ')',  ' ',  'e',  'r',  'r',  'o',  'r', ' ',  '{',
        '\n', '\t', '/', '/', ' ', 0xC0, 0xD4, 0xB7, 0xC2, '\n', '\t', 'r', 'e',  't',
        'u',  'r',  'n', ' ', 'n', 'i',  'l',  '\n', '}',  '\n'};
    ASSERT_EQ(fwrite(source, 1, sizeof(source), fp), sizeof(source));
    ASSERT_EQ(fclose(fp), 0);

    char *raw =
        cbm_mcp_handle_tool(srv, "get_code_snippet",
                            "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                            "\"project\":\"test-project\"}");
    ASSERT_TRUE(is_valid_json_response(raw));
    char *resp = extract_text_content(raw);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(is_valid_json_response(resp));
    ASSERT_NULL(strstr(resp, "\xC0\xD4"));
    ASSERT_NOT_NULL(strstr(resp, "HandleRequest"));
    ASSERT_NOT_NULL(strstr(resp, "return nil"));
    ASSERT_TRUE(snippet_source_has_replacement(resp));

    free(resp);
    free(raw);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_empty_string) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_jsonrpc_field) {
    /* jsonrpc field absent — parser defaults to "2.0" if method present */
    const char *line = "{\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_TRUE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_method) {
    /* method is required — should fail */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_string_id) {
    /* JSON-RPC §4: string and numeric ids are distinct. A string id is
     * preserved verbatim (issue #253), never coerced to a number. */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "99");
    ASSERT_STR_EQ(req.method, "tools/list");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_no_params) {
    /* Request with no params field — params_raw should be NULL */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(req.params_raw);
    ASSERT_EQ(req.id, 5);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_extra_whitespace) {
    /* Leading/trailing whitespace and internal spacing in JSON */
    const char *line = "  { \"jsonrpc\" : \"2.0\" , \"id\" : 7 , \"method\" : \"ping\" }  ";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(req.id, 7);
    ASSERT_STR_EQ(req.method, "ping");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_array_not_object) {
    /* JSON array at root — not a valid JSON-RPC request */
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("[1,2,3]", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_string_arg_empty_json) {
    /* Empty JSON string — yyjson_read fails → NULL */
    char *val = cbm_mcp_get_string_arg("", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_empty_object) {
    /* Valid JSON with no keys → NULL for any key */
    char *val = cbm_mcp_get_string_arg("{}", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_nested_value) {
    /* Value is an object, not a string → should return NULL */
    const char *args = "{\"config\":{\"nested\":true},\"name\":\"hello\"}";
    char *val = cbm_mcp_get_string_arg(args, "config");
    ASSERT_NULL(val); /* not a string type */
    val = cbm_mcp_get_string_arg(args, "name");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "hello");
    free(val);
    PASS();
}

TEST(mcp_get_string_arg_int_value) {
    /* Value is an integer, not a string → NULL */
    char *val = cbm_mcp_get_string_arg("{\"count\":42}", "count");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg_empty_json) {
    int val = cbm_mcp_get_int_arg("", "key", 99);
    ASSERT_EQ(val, 99);
    PASS();
}

TEST(mcp_get_int_arg_string_value) {
    /* Value is a string, not int → should return default */
    int val = cbm_mcp_get_int_arg("{\"limit\":\"ten\"}", "limit", 5);
    ASSERT_EQ(val, 5);
    PASS();
}

TEST(mcp_get_int_arg_bool_value) {
    /* Value is a bool, not int → default */
    int val = cbm_mcp_get_int_arg("{\"flag\":true}", "flag", -1);
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(mcp_get_bool_arg_empty_json) {
    bool val = cbm_mcp_get_bool_arg("", "key");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_bool_arg_int_value) {
    /* Value is int 1, not bool → should return false */
    bool val = cbm_mcp_get_bool_arg("{\"flag\":1}", "flag");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_tool_name_empty_json) {
    char *name = cbm_mcp_get_tool_name("");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_tool_name_missing_name) {
    char *name = cbm_mcp_get_tool_name("{\"arguments\":{}}");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_arguments_empty_json) {
    char *args = cbm_mcp_get_arguments("");
    ASSERT_NULL(args);
    PASS();
}

TEST(mcp_get_arguments_no_arguments_key) {
    /* No "arguments" key → returns "{}" */
    char *args = cbm_mcp_get_arguments("{\"name\":\"tool\"}");
    ASSERT_NOT_NULL(args);
    ASSERT_STR_EQ(args, "{}");
    free(args);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  FILE URI PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_http_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("http://example.com/path", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_ftp_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("ftp://server/file.txt", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_buffer_too_small) {
    char path[5]; /* only 5 bytes — path gets truncated */
    ASSERT_TRUE(cbm_parse_file_uri("file:///usr/local/bin", path, sizeof(path)));
    /* snprintf truncates to 4 chars + NUL */
    ASSERT_EQ(strlen(path), 4);
    ASSERT_STR_EQ(path, "/usr");
    PASS();
}

TEST(parse_file_uri_spaces_in_path) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/my%20project", path, sizeof(path)));
    /* Raw percent-encoding is preserved (not decoded) */
    ASSERT_STR_EQ(path, "/home/user/my%20project");
    PASS();
}

TEST(parse_file_uri_null_out_path) {
    /* NULL out_path — should not crash */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", NULL, 256));
    PASS();
}

TEST(parse_file_uri_zero_size) {
    char path[256] = "garbage";
    /* out_size=0 → should fail safely */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", path, 0));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_invalid_json) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(srv, "this is not json at all");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32700")); /* Parse error */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_empty_object) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Valid JSON but no method field → parse error */
    char *resp = cbm_mcp_server_handle(srv, "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_call_missing_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* tools/call with no tool name in params */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
                                   "\"params\":{\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about unknown/missing tool */
    ASSERT_NOT_NULL(strstr(resp, "\"id\":50"));
    ASSERT_TRUE(strstr(resp, "error") || strstr(resp, "isError") || strstr(resp, "unknown"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  POLL/GETLINE FILE* BUFFERING FIX
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>

/* Signal handler used by alarm() to abort the test if it hangs */
static void alarm_handler(int sig) {
    (void)sig;
    /* Writing to stderr is async-signal-safe */
    const char msg[] = "FAIL: mcp_server_run_rapid_messages timed out (>5s)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

TEST(mcp_server_run_rapid_messages) {
    /* Simulate a client sending initialize + notifications/initialized +
     * tools/list all at once (no delays), which exercises the FILE*
     * buffering fix: the first getline() over-reads kernel data into the
     * libc buffer; without the fix, subsequent poll() calls block for 60s.
     *
     * We use alarm(5) to abort the test process if the server hangs. */
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Write all 3 messages to the write end in one shot */
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
    ssize_t written = write(fds[1], msgs, strlen(msgs));
    ASSERT_TRUE(written > 0);
    close(fds[1]); /* EOF signals end of input to the server */

    FILE *in_fp = fdopen(fds[0], "r");
    ASSERT_NOT_NULL(in_fp);

    FILE *out_fp = tmpfile();
    ASSERT_NOT_NULL(out_fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Install alarm to fail the test if cbm_mcp_server_run blocks */
    signal(SIGALRM, alarm_handler);
    alarm(5);

    int rc = cbm_mcp_server_run(srv, in_fp, out_fp);

    alarm(0); /* cancel alarm */
    signal(SIGALRM, SIG_DFL);

    ASSERT_EQ(rc, 0);

    /* Verify both responses are present:
     *   id:1 — initialize response
     *   id:2 — tools/list response (notifications/initialized produces none)
     * and that the tools list payload is included. */
    rewind(out_fp);
    char buf[4096] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, out_fp);
    ASSERT_TRUE(nread > 0);
    ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(buf, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(buf, "tools"));

    cbm_mcp_server_free(srv);
    fclose(out_fp);
    /* in_fp already EOF; fclose cleans up */
    fclose(in_fp);
    PASS();
}
#endif /* !_WIN32 */

/* Issue #235: passing an unrecognised project name to a tool crashed the
 * binary with a buffer overflow while building the "available_projects"
 * error list — collect_db_project_names overflowed projects[CBM_SZ_4K] via
 * an unsigned underflow on (out_sz - offset) once the listed names exceeded
 * the buffer. Fill a temp cache dir with enough long-named .db files to
 * exceed 4 KB, then hit the bad-project path. Under ASan a regression aborts
 * here; the fixed bounds-check keeps it clean and returns a normal error. */
#define ISSUE235_DBNAME(buf, dir, i)                                                         \
    snprintf((buf), sizeof(buf),                                                             \
             "%s/proj_%02d_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.db",                      \
             (dir), (i))
TEST(tool_bad_project_name_no_overflow_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 40 * ~130-char names overflows the 4 KB available-projects buffer. */
    enum { ISSUE235_N = 40 };
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        FILE *fp = fopen(name, "w");
        if (fp) {
            fputc('x', fp);
            fclose(fp);
        }
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"search_graph\",\"arguments\":{\"label\":\"Function\","
             "\"project\":\"definitely-not-a-real-project-xyz\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        cbm_unlink(name);
    }
    cbm_rmdir(cache);
    PASS();
}
#undef ISSUE235_DBNAME

/* M2-a: explore emits a "## Neighbors" section (1-hop CALLS callees + same-file
 * siblings) by default. Populate the server's store with caller→callee plus a
 * same-file sibling, then assert the section + neighbor names appear. */
TEST(tool_explore_neighbors_expand_default) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "explore-neighbors";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/explore-neighbors");

    cbm_node_t n_caller = {.project = proj,
                           .label = "Function",
                           .name = "caller",
                           .qualified_name = "explore-neighbors.main.caller",
                           .file_path = "main.go",
                           .start_line = 10,
                           .end_line = 15};
    cbm_node_t n_callee = {.project = proj,
                           .label = "Function",
                           .name = "calleeFn",
                           .qualified_name = "explore-neighbors.main.calleeFn",
                           .file_path = "main.go",
                           .start_line = 20,
                           .end_line = 25};
    cbm_node_t n_sibling = {.project = proj,
                            .label = "Function",
                            .name = "siblingFn",
                            .qualified_name = "explore-neighbors.main.siblingFn",
                            .file_path = "main.go",
                            .start_line = 30,
                            .end_line = 35};
    int64_t id_caller = cbm_store_upsert_node(st, &n_caller);
    int64_t id_callee = cbm_store_upsert_node(st, &n_callee);
    ASSERT_GT(id_caller, 0);
    ASSERT_GT(id_callee, 0);
    ASSERT_GT(cbm_store_upsert_node(st, &n_sibling), 0);
    cbm_edge_t e = {
        .project = proj, .source_id = id_caller, .target_id = id_callee, .type = "CALLS"};
    cbm_store_insert_edge(st, &e);

    /* expand defaults to true (omitted) */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"explore\","
             "\"arguments\":{\"query\":\"caller\",\"project\":\"explore-neighbors\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NOT_NULL(strstr(inner, "## Neighbors"));
    ASSERT_NOT_NULL(strstr(inner, "calleeFn"));  /* 1-hop outbound CALLS */
    ASSERT_NOT_NULL(strstr(inner, "siblingFn")); /* same-file sibling */

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* M2-a: explore with expand:false suppresses the Neighbors section while keeping
 * the Blast-radius and Source sections. */
TEST(tool_explore_neighbors_expand_false) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "explore-no-neighbors";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/explore-no-neighbors");

    cbm_node_t n_caller = {.project = proj,
                           .label = "Function",
                           .name = "caller",
                           .qualified_name = "explore-no-neighbors.main.caller",
                           .file_path = "main.go",
                           .start_line = 10,
                           .end_line = 15};
    cbm_node_t n_callee = {.project = proj,
                           .label = "Function",
                           .name = "calleeFn",
                           .qualified_name = "explore-no-neighbors.main.calleeFn",
                           .file_path = "main.go",
                           .start_line = 20,
                           .end_line = 25};
    int64_t id_caller = cbm_store_upsert_node(st, &n_caller);
    int64_t id_callee = cbm_store_upsert_node(st, &n_callee);
    ASSERT_GT(id_caller, 0);
    ASSERT_GT(id_callee, 0);
    cbm_edge_t e = {
        .project = proj, .source_id = id_caller, .target_id = id_callee, .type = "CALLS"};
    cbm_store_insert_edge(st, &e);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"explore\",\"arguments\":{\"query\":\"caller\","
             "\"project\":\"explore-no-neighbors\",\"expand\":false}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NULL(strstr(inner, "## Neighbors")); /* suppressed */
    ASSERT_NOT_NULL(strstr(inner, "## Blast radius"));
    ASSERT_NOT_NULL(strstr(inner, "## Source"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);

/* Issue #576: ingest_dbt_manifest parses a compiled dbt manifest.json and
 * upserts model/seed/snapshot as graph nodes, source as Source nodes, and each
 * node's depends_on.nodes as DEPENDS_ON edges, keyed by dbt unique_id. The
 * handler opens (and creates) the project's on-disk store at
 * <CBM_CACHE_DIR>/<project>.db, so an isolated cache dir plus a small fixture
 * manifest exercises the whole path. The fixture carries 3 models + 1 seed +
 * 1 snapshot + 2 sources and one `test` node that must be skipped, so the
 * reported counts pin both the resource-type filter and the lineage edges. */
static const char *DBT_MANIFEST_FIXTURE =
    "{\"nodes\":{"
    "\"model.shop.stg_orders\":{\"resource_type\":\"model\",\"name\":\"stg_orders\","
    "\"depends_on\":{\"nodes\":[\"source.shop.raw.orders\"]}},"
    "\"model.shop.stg_customers\":{\"resource_type\":\"model\",\"name\":\"stg_customers\","
    "\"depends_on\":{\"nodes\":[\"source.shop.raw.customers\"]}},"
    "\"model.shop.fct_orders\":{\"resource_type\":\"model\",\"name\":\"fct_orders\","
    "\"depends_on\":{\"nodes\":[\"model.shop.stg_orders\",\"model.shop.stg_customers\"]}},"
    "\"seed.shop.country_codes\":{\"resource_type\":\"seed\",\"name\":\"country_codes\","
    "\"depends_on\":{\"nodes\":[]}},"
    "\"snapshot.shop.orders_snap\":{\"resource_type\":\"snapshot\",\"name\":\"orders_snap\","
    "\"depends_on\":{\"nodes\":[\"model.shop.fct_orders\"]}},"
    "\"test.shop.not_null_fct_orders\":{\"resource_type\":\"test\",\"name\":\"not_null\","
    "\"depends_on\":{\"nodes\":[\"model.shop.fct_orders\"]}}"
    "},\"sources\":{"
    "\"source.shop.raw.orders\":{\"resource_type\":\"source\",\"name\":\"orders\"},"
    "\"source.shop.raw.customers\":{\"resource_type\":\"source\",\"name\":\"customers\"}"
    "}}";

TEST(tool_ingest_dbt_manifest_issue576) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-dbt-manifest-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    const char *project = "test-dbt-manifest";

    /* Write the fixture manifest into the isolated cache dir. */
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", cache);
    FILE *fp = fopen(manifest_path, "w");
    int wrote = 0;
    if (fp) {
        fputs(DBT_MANIFEST_FIXTURE, fp);
        fclose(fp);
        wrote = 1;
    }

    /* The tool augments an existing project store (index_repository normally
     * creates it). Pre-create the store at the path the tool reopens and
     * register the project row, otherwise the FK-checked node upserts are
     * rejected and nothing is ingested. */
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/%s.db", cache, project);
    cbm_store_t *seed = cbm_store_open_path(dbpath);
    int seeded = 0;
    if (seed) {
        cbm_store_upsert_project(seed, project, cache);
        cbm_store_close(seed);
        seeded = 1;
    }

    /* Call the tool through the MCP server and capture the reported counts. */
    int ok_status = 0, ok_models = 0, ok_sources = 0, ok_edges = 0;
    if (wrote && seeded) {
        char req[2048];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
                 "\"ingest_dbt_manifest\",\"arguments\":{\"project\":\"%s\","
                 "\"manifest_path\":\"%s\"}}}",
                 project, manifest_path);
        cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
        if (srv) {
            char *resp = cbm_mcp_server_handle(srv, req);
            if (resp) {
                char *inner = extract_text_content(resp);
                if (inner) {
                    ok_status = strstr(inner, "\"status\":\"ingested\"") != NULL;
                    ok_models = strstr(inner, "\"models\":5") != NULL;
                    ok_sources = strstr(inner, "\"sources\":2") != NULL;
                    ok_edges = strstr(inner, "\"edges\":5") != NULL;
                    free(inner);
                }
                free(resp);
            }
            cbm_mcp_server_free(srv);
        }
    }

    /* Restore env and clean up BEFORE asserting, so a failed assertion never
     * leaks the overridden CBM_CACHE_DIR into the rest of the suite. */
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    char dbwal[576];
    char dbshm[576];
    snprintf(dbwal, sizeof(dbwal), "%s-wal", dbpath);
    snprintf(dbshm, sizeof(dbshm), "%s-shm", dbpath);
    cbm_unlink(dbwal);
    cbm_unlink(dbshm);
    cbm_unlink(dbpath);
    cbm_unlink(manifest_path);
    cbm_rmdir(cache);

    ASSERT(wrote);
    ASSERT(seeded);
    /* 3 models + 1 seed + 1 snapshot = 5 lineage-bearing nodes; the `test`
     * node is skipped (so models is 5, not 6). */
    ASSERT(ok_status);
    ASSERT(ok_models);
    ASSERT(ok_sources);
    /* stg_orders/stg_customers -> source, fct_orders -> 2 stg, snap -> fct = 5
     * DEPENDS_ON edges; the skipped test node contributes none. */
    ASSERT(ok_edges);

    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(mcp) {
    /* JSON-RPC parsing */
    RUN_TEST(jsonrpc_parse_request);
    RUN_TEST(jsonrpc_parse_notification);
    RUN_TEST(jsonrpc_parse_invalid);
    RUN_TEST(jsonrpc_parse_tools_call);
    RUN_TEST(jsonrpc_parse_string_id_issue253);
    RUN_TEST(jsonrpc_format_response_string_id_issue253);

    /* JSON-RPC parsing — edge cases */
    RUN_TEST(jsonrpc_parse_empty_string);
    RUN_TEST(jsonrpc_parse_missing_jsonrpc_field);
    RUN_TEST(jsonrpc_parse_missing_method);
    RUN_TEST(jsonrpc_parse_string_id);
    RUN_TEST(jsonrpc_parse_no_params);
    RUN_TEST(jsonrpc_parse_extra_whitespace);
    RUN_TEST(jsonrpc_parse_array_not_object);

    /* JSON-RPC formatting */
    RUN_TEST(jsonrpc_format_response);
    RUN_TEST(jsonrpc_format_error);

    /* MCP protocol helpers */
    RUN_TEST(mcp_initialize_response);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_tools_array_schemas_have_items);
    RUN_TEST(mcp_text_result);
    RUN_TEST(mcp_text_result_error);

    /* Argument extraction */
    RUN_TEST(mcp_get_tool_name);
    RUN_TEST(mcp_get_arguments);
    RUN_TEST(mcp_get_string_arg);
    RUN_TEST(mcp_get_int_arg);
    RUN_TEST(mcp_get_bool_arg);

    /* Argument extraction — edge cases */
    RUN_TEST(mcp_get_string_arg_empty_json);
    RUN_TEST(mcp_get_string_arg_empty_object);
    RUN_TEST(mcp_get_string_arg_nested_value);
    RUN_TEST(mcp_get_string_arg_int_value);
    RUN_TEST(mcp_get_int_arg_empty_json);
    RUN_TEST(mcp_get_int_arg_string_value);
    RUN_TEST(mcp_get_int_arg_bool_value);
    RUN_TEST(mcp_get_bool_arg_empty_json);
    RUN_TEST(mcp_get_bool_arg_int_value);
    RUN_TEST(mcp_get_tool_name_empty_json);
    RUN_TEST(mcp_get_tool_name_missing_name);
    RUN_TEST(mcp_get_arguments_empty_json);
    RUN_TEST(mcp_get_arguments_no_arguments_key);

    /* Server protocol handling */
    RUN_TEST(server_handle_initialize);
    RUN_TEST(server_handle_initialized_notification);
    RUN_TEST(server_handle_tools_list);
    RUN_TEST(server_handle_unknown_method);

    /* Server handle — edge cases */
    RUN_TEST(server_handle_invalid_json);
    RUN_TEST(server_handle_empty_object);
    RUN_TEST(server_handle_tools_call_missing_name);

    /* Tool handlers */
    RUN_TEST(tool_list_projects_empty);
    RUN_TEST(tool_get_graph_schema_empty);
    RUN_TEST(tool_unknown_tool);
    RUN_TEST(tool_explore_requires_query);
    RUN_TEST(tool_explore_unindexed_no_crash);
    RUN_TEST(tool_explore_neighbors_expand_default);
    RUN_TEST(tool_explore_neighbors_expand_false);
    RUN_TEST(tool_search_graph_basic);
    RUN_TEST(tool_search_graph_includes_node_properties);
    RUN_TEST(tool_search_graph_toon_never_leaks_internal_fields);
    RUN_TEST(tool_output_byte_budgets);
    RUN_TEST(tool_search_graph_query_honors_file_pattern_issue552);
    RUN_TEST(tool_search_graph_semantic_only_uses_vector_results_issue915);
    RUN_TEST(mcp_resource_discovery_methods_return_empty_lists);
    RUN_TEST(tool_query_graph_basic);
    RUN_TEST(tool_ingest_dbt_manifest_issue576);
    RUN_TEST(tool_index_status_no_project);
    RUN_TEST(tool_index_status_includes_git_metadata);

    /* Tool handlers with validation */
    RUN_TEST(tool_trace_call_path_not_found);
    RUN_TEST(tool_trace_missing_function_name);
    RUN_TEST(tool_trace_call_path_ambiguous);
    RUN_TEST(tool_trace_call_path_prefers_definition);
    RUN_TEST(tool_delete_project_not_found);
    RUN_TEST(tool_get_architecture_empty);
    RUN_TEST(tool_get_architecture_emits_populated_sections);
    RUN_TEST(tool_query_graph_missing_query);

    /* Pipeline-dependent tool handlers */
    RUN_TEST(tool_index_repository_missing_path);
    RUN_TEST(tool_get_code_snippet_missing_qn);
    RUN_TEST(tool_get_code_snippet_not_found);
    RUN_TEST(tool_search_code_missing_pattern);
    RUN_TEST(tool_search_code_no_project);
    RUN_TEST(search_code_multi_word);
    RUN_TEST(search_code_invalid_regex_errors_issue283);
    RUN_TEST(search_code_literal_pipe_warns_issue282);
    RUN_TEST(search_code_ampersand_accepted_issue272);
    RUN_TEST(tool_detect_changes_no_project);
    RUN_TEST(tool_manage_adr_no_project);
    RUN_TEST(tool_manage_adr_get_with_existing_adr);
    RUN_TEST(tool_manage_adr_unified_backend_issue256);
    RUN_TEST(tool_manage_adr_append_extends_without_replacing);
    RUN_TEST(tool_manage_adr_append_creates_when_absent);
    RUN_TEST(tool_manage_adr_append_without_content_errors);
    RUN_TEST(tool_manage_adr_append_is_advertised);
    RUN_TEST(tool_index_repository_reports_store_backed_adr);
    RUN_TEST(tool_index_repository_dot_uses_absolute_project_key_and_preserves_adr);
    RUN_TEST(index_repository_relative_path_uses_explicit_session_root);
    RUN_TEST(index_repository_supervisor_uses_canonical_session_path);
    RUN_TEST(index_repository_cli_name_override_issue823);
    RUN_TEST(index_supervisor_unsafe_clean_is_never_fallback_or_recovery);
    RUN_TEST(index_supervisor_gate_requires_marked_host_issue845);
    RUN_TEST(index_supervisor_start_failure_is_fail_closed_in_real_host);
    RUN_TEST(index_bg_paths_route_through_supervisor_issue832);
    RUN_TEST(sequential_service_edge_props_are_valid_json_issue898);
    RUN_TEST(index_second_inprocess_run_survives_issue773);
    RUN_TEST(index_recovery_parallel_quarantines_crasher);
    RUN_TEST(tool_manage_adr_not_found_rich_error);
    RUN_TEST(tool_manage_adr_get_accepts_abs_path);
    RUN_TEST(tool_manage_adr_get_accepts_symlink_path);
    RUN_TEST(tool_detect_changes_not_found_rich_error);
    RUN_TEST(tool_detect_changes_contained_commands_clean_up_error_and_success);
    RUN_TEST(tool_ingest_traces_basic);
    RUN_TEST(tool_ingest_traces_empty);

    /* Idle store eviction */
    RUN_TEST(store_idle_eviction);
    RUN_TEST(store_idle_no_eviction_within_timeout);
    RUN_TEST(store_idle_evict_protects_initial_store);
    RUN_TEST(store_idle_evict_access_resets_timer);

    /* URI helpers */
    RUN_TEST(parse_file_uri_unix);
    RUN_TEST(parse_file_uri_windows);
    RUN_TEST(parse_file_uri_invalid);

    /* URI helpers — edge cases */
    RUN_TEST(parse_file_uri_http_scheme);
    RUN_TEST(parse_file_uri_ftp_scheme);
    RUN_TEST(parse_file_uri_buffer_too_small);
    RUN_TEST(parse_file_uri_spaces_in_path);
    RUN_TEST(parse_file_uri_null_out_path);
    RUN_TEST(parse_file_uri_zero_size);

    /* Poll/getline FILE* buffering fix */
#ifndef _WIN32
    RUN_TEST(mcp_server_run_rapid_messages);
#endif

    /* Snippet resolution (port of snippet_test.go) */
    RUN_TEST(snippet_exact_qn);
    RUN_TEST(snippet_qn_suffix);
    RUN_TEST(snippet_unique_short_name);
    RUN_TEST(snippet_name_tier);
    RUN_TEST(snippet_ambiguous_short_name);
    RUN_TEST(snippet_not_found);
    RUN_TEST(snippet_fuzzy_suggestions);
    RUN_TEST(snippet_enriched_properties);
    RUN_TEST(snippet_fuzzy_last_segment);
    RUN_TEST(snippet_auto_resolve_default);
    RUN_TEST(snippet_auto_resolve_enabled);
    RUN_TEST(snippet_include_neighbors_default);
    RUN_TEST(snippet_include_neighbors_enabled);
    RUN_TEST(snippet_source_invalid_utf8);
    RUN_TEST(tool_bad_project_name_no_overflow_issue235);
}
