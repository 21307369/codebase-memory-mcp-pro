/*
 * discover.c — Recursive directory walk with filtering.
 *
 * Walks a repository directory tree, applying:
 *   1. Hardcoded directory skip patterns (60+ dirs like .git, node_modules)
 *   2. Hardcoded suffix filters (.pyc, .png, .wasm, etc.)
 *   3. Fast-mode additional filters (docs, examples, lock files, etc.)
 *   4. Gitignore-style pattern matching
 *   5. Language detection for accepted files
 */
#include "discover/discover.h"
#include "cbm.h" // CBMLanguage, CBM_LANG_COUNT, CBM_LANG_JSON

#include "foundation/constants.h"
#include "foundation/compat_fs.h"
#include "foundation/platform.h"
#ifdef _WIN32
#include "foundation/win_utf8.h"
#endif
#include <stdint.h> // int64_t
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // strdup
#include <sys/stat.h>

/* ── Hardcoded always-skip directories ──────────────────────────── */

static const char *ALWAYS_SKIP_DIRS[] = {
    /* VCS */
    ".git", ".hg", ".svn", ".worktrees",
    /* IDE */
    ".idea", ".vs", ".vscode", ".eclipse", ".claude",
    /* Python */
    ".cache", ".eggs", ".env", ".mypy_cache", ".nox", ".pytest_cache", ".ruff_cache", ".tox",
    ".venv", "__pycache__", "env", "htmlcov", "site-packages", "venv",
    /* JS/TS */
    ".npm", ".nyc_output", ".pnpm-store", ".yarn", "bower_components", "coverage", "node_modules",
    ".next", ".nuxt", ".svelte-kit", ".angular", ".turbo", ".parcel-cache", ".docusaurus", ".expo",
    /* Build artifacts */
    "dist", "obj", "Pods", "target", "temp", "tmp", ".terraform", ".serverless", "bazel-bin",
    "bazel-out", "bazel-testlogs",
    /* Language caches */
    ".cargo", ".stack-work", ".dart_tool", "zig-cache", "zig-out", ".metals", ".bloop", ".bsp",
    ".ccls-cache", ".clangd", "elm-stuff", "_opam", ".cpcache", ".shadow-cljs",
    /* Deploy */
    ".vercel", ".netlify",
    /* Misc */
    ".qdrant_code_embeddings", ".tmp", "vendor", "vendored", NULL};

static const char *FAST_SKIP_DIRS[] = {
    "generated", "gen",           "auto-generated", "fixtures",     "testdata",    "test_data",
    "__tests__", "__mocks__",     "__snapshots__",  "__fixtures__", "__test__",    "docs",
    "doc",       "documentation", "examples",       "example",      "samples",     "sample",
    "assets",    "static",        "public",         "media",        "third_party", "thirdparty",
    "3rdparty",  "external",      "migrations",     "seeds",        "e2e",         "integration",
    "locale",    "locales",       "i18n",           "l10n",         "scripts",     "tools",
    "hack",      "bin",           "build",          "out",          NULL};

/* ── Ignored suffixes ───────────────────────────────── */

static const char *ALWAYS_IGNORED_SUFFIXES[] = {
    ".tmp",    "~",        ".pyc",  ".pyo",   ".o",   ".a",   ".so",  ".dll",
    ".class",  ".png",     ".jpg",  ".jpeg",  ".gif", ".ico", ".bmp", ".tiff",
    ".webp",   ".svg",     ".wasm", ".node",  ".exe", ".bin", ".dat", ".db",
    ".sqlite", ".sqlite3", ".woff", ".woff2", ".ttf", ".eot", ".otf", NULL};

static const char *FAST_IGNORED_SUFFIXES[] = {
    ".zip", ".tar",  ".gz",       ".bz2",  ".xz",  ".rar",    ".7z",      ".jar",
    ".war", ".ear",  ".mp3",      ".mp4",  ".avi", ".mov",    ".wav",     ".flac",
    ".ogg", ".mkv",  ".webm",     ".pdf",  ".doc", ".docx",   ".xls",     ".xlsx",
    ".ppt", ".pptx", ".odt",      ".ods",  ".map", ".min.js", ".min.css", ".pem",
    ".crt", ".key",  ".cer",      ".p12",  ".pb",  ".avro",   ".parquet", ".beam",
    ".elc", ".rlib", ".coverage", ".prof", ".out", ".patch",  ".diff",    NULL};

/* ── Fast-mode skip filenames ─────────────────────── */

static const char *FAST_SKIP_FILENAMES[] = {
    "LICENSE",        "LICENSE.txt",     "LICENSE.md",   "LICENSE-MIT",   "LICENSE-APACHE",
    "LICENCE",        "LICENCE.txt",     "LICENCE.md",   "CHANGELOG",     "CHANGELOG.md",
    "CHANGES.md",     "HISTORY",         "HISTORY.md",   "AUTHORS",       "AUTHORS.md",
    "CONTRIBUTORS",   "CONTRIBUTORS.md", "CODEOWNERS",   "go.sum",        "yarn.lock",
    "pnpm-lock.yaml", "Pipfile.lock",    "poetry.lock",  "Gemfile.lock",  "Cargo.lock",
    "mix.lock",       "flake.lock",      "pubspec.lock", "composer.lock", "package-lock.json",
    "configure",      "Makefile.in",     "config.guess", "config.sub",    NULL};

/* ── Fast-mode substring patterns ───────────────────── */

static const char *FAST_PATTERNS[] = {".d.ts",      ".bundle.", ".chunk.", ".generated.",
                                      ".pb.go",     "_pb2.py",  ".pb2.py", "_grpc.pb.go",
                                      "_string.go", "mock_",    "_mock.",  "_test_helpers.",
                                      ".stories.",  ".spec.",   ".test.",  NULL};

/* ── Ignored JSON filenames ──────────────────────── */

static const char *IGNORED_JSON_FILES[] = {
    "package.json",       "package-lock.json", "tsconfig.json",
    "jsconfig.json",      "composer.json",     "composer.lock",
    "yarn.lock",          "openapi.json",      "swagger.json",
    "jest.config.json",   ".eslintrc.json",    ".prettierrc.json",
    ".babelrc.json",      "tslint.json",       "angular.json",
    "firebase.json",      "renovate.json",     "lerna.json",
    "turbo.json",         ".stylelintrc.json", "pnpm-lock.json",
    "deno.json",          "biome.json",        "devcontainer.json",
    ".devcontainer.json", "launch.json",       "settings.json",
    "extensions.json",    "tasks.json",        NULL};

/* ── Helper: check if string is in NULL-terminated array ─────────── */

static bool str_in_list(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Helper: check if string ends with suffix ────────────── */

static bool ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t sufflen = strlen(suffix);
    if (sufflen > slen) {
        return false;
    }
    return strcmp(s + slen - sufflen, suffix) == 0;
}

/* ── Helper: check if string contains substring ───────────── */

static bool str_contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

/* ── Public filter functions ─────────────────────── */

bool cbm_should_skip_dir(const char *dirname, cbm_index_mode_t mode) {
    if (!dirname) {
        return false;
    }

    if (str_in_list(dirname, ALWAYS_SKIP_DIRS)) {
        return true;
    }

    /* Fast discovery applies to both MODERATE and FAST — only FULL keeps everything. */
    if (mode != CBM_MODE_FULL) {
        if (str_in_list(dirname, FAST_SKIP_DIRS)) {
            return true;
        }
    }

    return false;
}

bool cbm_has_ignored_suffix(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    for (int i = 0; ALWAYS_IGNORED_SUFFIXES[i]; i++) {
        if (ends_with(filename, ALWAYS_IGNORED_SUFFIXES[i])) {
            return true;
        }
    }

    if (mode != CBM_MODE_FULL) {
        for (int i = 0; FAST_IGNORED_SUFFIXES[i]; i++) {
            if (ends_with(filename, FAST_IGNORED_SUFFIXES[i])) {
                return true;
            }
        }
    }

    return false;
}

bool cbm_should_skip_filename(const char *filename, cbm_index_mode_t mode) {
    if (!filename) {
        return false;
    }

    if (mode != CBM_MODE_FULL) {
        if (str_in_list(filename, FAST_SKIP_FILENAMES)) {
            return true;
        }
    }

    return false;
}

bool cbm_matches_fast_pattern(const char *filename, cbm_index_mode_t mode) {
    if (!filename || mode == CBM_MODE_FULL) {
        return false;
    }

    for (int i = 0; FAST_PATTERNS[i]; i++) {
        if (str_contains(filename, FAST_PATTERNS[i])) {
            return true;
        }
    }

    return false;
}

/* ── Dynamic file list ────────────────────────── */

typedef struct {
    cbm_file_info_t *files;
    int count;
    int capacity;
    /* Directories skipped during the walk (rel paths), so callers can surface
     * which subtrees were dropped (#411). strdup'd; freed by the caller via
     * cbm_discover_free_excluded or internally when not requested. */
    char **excluded;
    int excluded_count;
    int excluded_cap;
    /* Resource policy (optional). When set, violations abort the walk with
     * CBM_DISCOVER_LIMIT_EXCEEDED instead of degrading silently. */
    const cbm_discover_limits_t *limits;
    cbm_discover_report_t *report;
    uint64_t deadline_ms;
    uint64_t directories;
    uint64_t entries;
    uint64_t source_bytes;
    bool failed;
    bool limit_exceeded;
} file_list_t;

static void file_list_violate(file_list_t *fl, cbm_discover_limit_t violation, uint64_t observed,
                              uint64_t limit) {
    if (!fl || fl->limit_exceeded || fl->failed) {
        return;
    }
    fl->limit_exceeded = true;
    if (fl->report) {
        fl->report->violation = violation;
        fl->report->observed = observed;
        fl->report->limit = limit;
    }
}

static bool file_list_should_stop(file_list_t *fl) {
    if (!fl) {
        return true;
    }
    if (!fl->failed && fl->deadline_ms != 0 && cbm_now_ms() >= fl->deadline_ms) {
        if (fl->limits) {
            file_list_violate(fl, CBM_DISCOVER_LIMIT_DEADLINE, cbm_now_ms(), fl->deadline_ms);
        } else {
            fl->failed = true;
        }
    }
    return fl->failed || fl->limit_exceeded;
}

static void file_list_add_excluded(file_list_t *fl, const char *rel_path) {
    if (!rel_path || rel_path[0] == '\0') {
        return;
    }
    if (fl->excluded_count >= fl->excluded_cap) {
        int new_cap = fl->excluded_cap ? fl->excluded_cap * PAIR_LEN : CBM_SZ_64;
        char **grown = realloc(fl->excluded, new_cap * sizeof(char *));
        if (!grown) {
            return;
        }
        fl->excluded = grown;
        fl->excluded_cap = new_cap;
    }
    char *copy = strdup(rel_path);
    if (!copy) {
        return;
    }
    fl->excluded[fl->excluded_count++] = copy;
}

static void fl_add(file_list_t *fl, const char *abs_path, const char *rel_path, CBMLanguage lang,
                   int64_t size) {
    if (fl->limits && fl->limits->max_files > 0 &&
        (uint64_t)fl->count >= fl->limits->max_files) {
        file_list_violate(fl, CBM_DISCOVER_LIMIT_FILES, (uint64_t)fl->count + 1U,
                          fl->limits->max_files);
        return;
    }
    uint64_t source_size = size > 0 ? (uint64_t)size : 0;
    if (fl->limits && fl->limits->max_source_bytes > 0 &&
        (source_size > fl->limits->max_source_bytes ||
         fl->source_bytes > fl->limits->max_source_bytes - source_size)) {
        uint64_t observed = source_size > UINT64_MAX - fl->source_bytes
                                ? UINT64_MAX
                                : fl->source_bytes + source_size;
        file_list_violate(fl, CBM_DISCOVER_LIMIT_SOURCE_BYTES, observed,
                          fl->limits->max_source_bytes);
        return;
    }
    fl->source_bytes += source_size;
    if (fl->count >= fl->capacity) {
        int new_cap = fl->capacity ? fl->capacity * PAIR_LEN : CBM_SZ_256;
        cbm_file_info_t *new_files = realloc(fl->files, new_cap * sizeof(cbm_file_info_t));
        if (!new_files) {
            return;
        }
        fl->files = new_files;
        fl->capacity = new_cap;
    }

    cbm_file_info_t *fi = &fl->files[fl->count++];
    fi->path = strdup(abs_path);
    fi->rel_path = strdup(rel_path);
    fi->language = lang;
    fi->size = size;
}

/* ── Recursive walk ─────────────────────────────── */

/* Compute path relative to a nested .gitignore's directory.
 * "webapp/src/foo.js" with prefix "webapp" → "src/foo.js". */
static const char *local_rel_path(const char *rel_path, const char *local_prefix) {
    if (!local_prefix || local_prefix[0] == '\0') {
        return rel_path;
    }
    size_t prefix_len = strlen(local_prefix);
    if (strncmp(rel_path, local_prefix, prefix_len) == 0 && rel_path[prefix_len] == '/') {
        return rel_path + prefix_len + SKIP_ONE;
    }
    return rel_path;
}

/* Check if a directory entry should be skipped (hardcoded dirs + gitignore). */
static bool should_skip_directory(const char *entry_name, const char *rel_path,
                                  const cbm_discover_opts_t *opts, const cbm_gitignore_t *gitignore,
                                  const cbm_gitignore_t *cbmignore, const cbm_gitignore_t *local_gi,
                                  const char *local_gi_prefix) {
    if (cbm_should_skip_dir(entry_name, opts ? opts->mode : CBM_MODE_FULL)) {
        return true;
    }
    if (gitignore && cbm_gitignore_matches(gitignore, rel_path, true)) {
        return true;
    }
    if (local_gi) {
        const char *lrel = local_rel_path(rel_path, local_gi_prefix);
        if (cbm_gitignore_matches(local_gi, lrel, true)) {
            return true;
        }
    }
    if (cbmignore && cbm_gitignore_matches(cbmignore, rel_path, true)) {
        return true;
    }
    return false;
}

/* Check if a regular file should be skipped (filters + gitignore + size). */
static bool should_skip_file(const char *entry_name, const char *rel_path,
                             const cbm_discover_opts_t *opts, const cbm_gitignore_t *gitignore,
                             const cbm_gitignore_t *cbmignore, const cbm_gitignore_t *local_gi,
                             const char *local_gi_prefix, off_t file_size) {
    cbm_index_mode_t mode = opts ? opts->mode : CBM_MODE_FULL;
    if (cbm_has_ignored_suffix(entry_name, mode)) {
        return true;
    }
    if (cbm_should_skip_filename(entry_name, mode)) {
        return true;
    }
    if (cbm_matches_fast_pattern(entry_name, mode)) {
        return true;
    }
    if (gitignore && cbm_gitignore_matches(gitignore, rel_path, false)) {
        return true;
    }
    if (local_gi) {
        const char *lrel = local_rel_path(rel_path, local_gi_prefix);
        if (cbm_gitignore_matches(local_gi, lrel, false)) {
            return true;
        }
    }
    if (cbmignore && cbm_gitignore_matches(cbmignore, rel_path, false)) {
        return true;
    }
    if (opts && opts->max_file_size > 0 && file_size > opts->max_file_size) {
        return true;
    }
    return false;
}

/* Detect language for a file, handling .m disambiguation and JSON filtering. */
static CBMLanguage detect_file_language(const char *entry_name, const char *abs_path) {
    CBMLanguage lang = cbm_language_for_filename(entry_name);
    if (lang == CBM_LANG_COUNT) {
        /* Filename/extension detection failed: fall back to a conservative
         * shebang probe so extensionless scripts get indexed (#1199). Filename
         * detection stays authoritative — this runs only when it returns
         * unknown. */
        return cbm_language_from_shebang(abs_path);
    }
    /* Special: .m files need content-based disambiguation */
    const char *dot = strrchr(entry_name, '.');
    if (dot && strcmp(dot, ".m") == 0) {
        lang = cbm_disambiguate_m(abs_path);
    }
    /* Check ignored JSON files */
    if (lang == CBM_LANG_JSON && str_in_list(entry_name, IGNORED_JSON_FILES)) {
        return CBM_LANG_COUNT;
    }
    return lang;
}

/* UTF-8-safe stat: wide API on Windows, regular stat on POSIX. */
static int wide_stat(const char *path, struct stat *st) {
#ifdef _WIN32
    wchar_t *wpath = cbm_utf8_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    struct _stat64 wst;
    int ret = _wstat64(wpath, &wst);
    free(wpath);
    if (ret != 0) {
        return CBM_NOT_FOUND;
    }
    st->st_mode = wst.st_mode;
    st->st_size = wst.st_size;
    st->st_mtime = wst.st_mtime;
    return 0;
#else
    return stat(path, st);
#endif
}

/* Stat a path, skipping symlinks. Returns 0 on success, -1 to skip. */
static int safe_stat(const char *abs_path, struct stat *st) {
#ifdef _WIN32
    return wide_stat(abs_path, st);
#else
    if (lstat(abs_path, st) != 0) {
        return CBM_NOT_FOUND;
    }
    if (S_ISLNK(st->st_mode)) {
        return CBM_NOT_FOUND;
    }
    return 0;
#endif
}

/* Process a single regular file entry during directory walk. */
static void walk_dir_process_file(const char *abs_path, const char *rel_path, const char *name,
                                  const cbm_discover_opts_t *opts, const cbm_gitignore_t *gitignore,
                                  const cbm_gitignore_t *cbmignore, const cbm_gitignore_t *local_gi,
                                  const char *local_gi_prefix, off_t size, file_list_t *out) {
    if (should_skip_file(name, rel_path, opts, gitignore, cbmignore, local_gi, local_gi_prefix,
                         size)) {
        return;
    }
    CBMLanguage lang = detect_file_language(name, abs_path);
    if (lang == CBM_LANG_COUNT) {
        return;
    }
    fl_add(out, abs_path, rel_path, lang, size);
}

typedef struct {
    char dir[CBM_SZ_4K];
    char prefix[CBM_SZ_4K];
    uint64_t depth;                  /* root is depth zero */
    cbm_gitignore_t *local_gi;       /* nested .gitignore for this subtree */
    char local_gi_prefix[CBM_SZ_4K]; /* rel_prefix when local_gi was loaded */
} walk_frame_t;
#define WALK_STACK_CAP 512
/* Build abs/rel paths and process one directory entry. */
/* Try to load a nested .gitignore from this directory. Returns owned pointer or NULL. */
static cbm_gitignore_t *try_load_nested_gitignore(const walk_frame_t *frame) {
    if (frame->local_gi || frame->prefix[0] == '\0') {
        return NULL;
    }
    char gi_path[CBM_SZ_4K];
    snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", frame->dir);
    struct stat gi_st;
    if (wide_stat(gi_path, &gi_st) == 0 && S_ISREG(gi_st.st_mode)) {
        return cbm_gitignore_load(gi_path);
    }
    return NULL;
}

/* Push a subdirectory onto the walk stack, inheriting local gitignore context. */
static void walk_push_subdir(walk_frame_t *stack, int *top, const char *abs_path,
                             const char *rel_path, const walk_frame_t *parent) {
    if (*top >= WALK_STACK_CAP) {
        return;
    }
    snprintf(stack[*top].dir, CBM_SZ_4K, "%s", abs_path);
    snprintf(stack[*top].prefix, CBM_SZ_4K, "%s", rel_path);
    stack[*top].depth = parent->depth + 1U;
    stack[*top].local_gi = parent->local_gi;
    snprintf(stack[*top].local_gi_prefix, CBM_SZ_4K, "%s", parent->local_gi_prefix);
    (*top)++;
}

static void walk_dir_process_entry(cbm_dirent_t *entry, const walk_frame_t *frame,
                                   const cbm_discover_opts_t *opts,
                                   const cbm_gitignore_t *gitignore,
                                   const cbm_gitignore_t *cbmignore, walk_frame_t *stack, int *top,
                                   file_list_t *out) {
    if (file_list_should_stop(out)) {
        return;
    }
    out->entries++;
    if (out->limits && out->limits->max_entries > 0 && out->entries > out->limits->max_entries) {
        file_list_violate(out, CBM_DISCOVER_LIMIT_ENTRIES, out->entries, out->limits->max_entries);
        return;
    }

    char abs_path[CBM_SZ_4K];
    char rel_path[CBM_SZ_4K];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", frame->dir, entry->name);
    if (frame->prefix[0] != '\0') {
        snprintf(rel_path, sizeof(rel_path), "%s/%s", frame->prefix, entry->name);
    } else {
        snprintf(rel_path, sizeof(rel_path), "%s", entry->name);
    }

    struct stat st;
    if (safe_stat(abs_path, &st) != 0) {
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!should_skip_directory(entry->name, rel_path, opts, gitignore, cbmignore,
                                   frame->local_gi, frame->local_gi_prefix)) {
            uint64_t child_depth = frame->depth + 1U;
            if (out->limits && out->limits->max_depth > 0 && child_depth > out->limits->max_depth) {
                file_list_violate(out, CBM_DISCOVER_LIMIT_DEPTH, child_depth,
                                  out->limits->max_depth);
                return;
            }
            uint64_t next_directory = out->directories + 1U;
            if (out->limits && out->limits->max_directories > 0 &&
                next_directory > out->limits->max_directories) {
                file_list_violate(out, CBM_DISCOVER_LIMIT_DIRECTORIES, next_directory,
                                  out->limits->max_directories);
                return;
            }
            out->directories = next_directory;
            walk_push_subdir(stack, top, abs_path, rel_path, frame);
        } else {
            /* Record the excluded subtree root so callers can report it (#411). */
            file_list_add_excluded(out, rel_path);
        }
    } else if (S_ISREG(st.st_mode)) {
        walk_dir_process_file(abs_path, rel_path, entry->name, opts, gitignore, cbmignore,
                              frame->local_gi, frame->local_gi_prefix, st.st_size, out);
    }
}

enum { GI_OWNED_CAP = 64 };

static void walk_dir(const char *dir_path, const char *rel_prefix, const cbm_discover_opts_t *opts,
                     const cbm_gitignore_t *gitignore, const cbm_gitignore_t *cbmignore,
                     file_list_t *out) {
    walk_frame_t *stack = calloc(WALK_STACK_CAP, sizeof(walk_frame_t));
    if (!stack) {
        return;
    }
    /* Collect all owned gitignores — freed at the end because child frames
     * on the stack hold borrowed pointers to them. */
    cbm_gitignore_t *owned_gis[GI_OWNED_CAP];
    int owned_count = 0;

    int top = 0;
    snprintf(stack[top].dir, CBM_SZ_4K, "%s", dir_path);
    snprintf(stack[top].prefix, CBM_SZ_4K, "%s", rel_prefix);
    top++;
    out->directories = 1; /* the root counts as one directory */

    while (top > 0) {
        if (file_list_should_stop(out)) {
            break;
        }
        walk_frame_t frame = stack[--top];

        cbm_gitignore_t *loaded = try_load_nested_gitignore(&frame);
        if (loaded) {
            frame.local_gi = loaded;
            snprintf(frame.local_gi_prefix, sizeof(frame.local_gi_prefix), "%s", frame.prefix);
            if (owned_count < GI_OWNED_CAP) {
                owned_gis[owned_count++] = loaded;
            }
        }

        cbm_dir_t *d = cbm_opendir(frame.dir);
        if (!d) {
            continue;
        }

        cbm_dirent_t *entry;
        while (!file_list_should_stop(out) && (entry = cbm_readdir(d)) != NULL) {
            walk_dir_process_entry(entry, &frame, opts, gitignore, cbmignore, stack, &top, out);
        }
        cbm_closedir(d);
    }
    for (int i = 0; i < owned_count; i++) {
        cbm_gitignore_free(owned_gis[i]);
    }
    free(stack);
}

/* ── Public API ───────────────────────────────── */

int cbm_discover(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                 int *count) {
    return cbm_discover_ex(repo_path, opts, out, count, NULL, NULL);
}

int cbm_discover_ex(const char *repo_path, const cbm_discover_opts_t *opts, cbm_file_info_t **out,
                    int *count, char ***excluded_out, int *excluded_count_out) {
    if (excluded_out) {
        *excluded_out = NULL;
    }
    if (excluded_count_out) {
        *excluded_count_out = 0;
    }
    if (!repo_path || !out || !count) {
        return CBM_NOT_FOUND;
    }

    *out = NULL;
    *count = 0;

    /* Verify directory exists */
    struct stat st;
    if (wide_stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return CBM_NOT_FOUND;
    }

    /* Load gitignore if .git directory exists */
    cbm_gitignore_t *gitignore = NULL;
    char gi_path[CBM_SZ_4K];
    snprintf(gi_path, sizeof(gi_path), "%s/.git", repo_path);
    struct stat gi_stat;
    if (wide_stat(gi_path, &gi_stat) == 0 && S_ISDIR(gi_stat.st_mode)) {
        snprintf(gi_path, sizeof(gi_path), "%s/.gitignore", repo_path);
        gitignore = cbm_gitignore_load(gi_path);
    }

    /* Load cbmignore if specified or exists at repo root */
    cbm_gitignore_t *cbmignore = NULL;
    if (opts && opts->ignore_file) {
        cbmignore = cbm_gitignore_load(opts->ignore_file);
    } else {
        snprintf(gi_path, sizeof(gi_path), "%s/.cbmignore", repo_path);
        cbmignore = cbm_gitignore_load(gi_path);
    }

    /* Walk */
    file_list_t fl = {
        .limits = opts ? opts->limits : NULL,
        .report = opts ? opts->report : NULL,
        .deadline_ms = opts && opts->limits ? opts->limits->deadline_ms : 0,
    };
    if (fl.report) {
        memset(fl.report, 0, sizeof(*fl.report));
    }
    walk_dir(repo_path, "", opts, gitignore, cbmignore, &fl);

    /* Cleanup */
    cbm_gitignore_free(gitignore);
    cbm_gitignore_free(cbmignore);

    if (fl.limit_exceeded) {
        /* Discard partial results: a resource policy was violated. */
        cbm_discover_free(fl.files, fl.count);
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
        *out = NULL;
        *count = 0;
        return CBM_DISCOVER_LIMIT_EXCEEDED;
    }

    *out = fl.files;
    *count = fl.count;

    /* Hand the excluded-dir list to the caller, or free it if not requested. */
    if (excluded_out) {
        *excluded_out = fl.excluded;
        if (excluded_count_out) {
            *excluded_count_out = fl.excluded_count;
        }
    } else {
        cbm_discover_free_excluded(fl.excluded, fl.excluded_count);
    }
    return 0;
}

void cbm_discover_free(cbm_file_info_t *files, int count) {
    if (!files) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(files[i].path);
        free(files[i].rel_path);
    }
    free(files);
}

void cbm_discover_free_excluded(char **excluded, int count) {
    if (!excluded) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(excluded[i]);
    }
    free(excluded);
}

/* ── Bounded file counting (#713) ─────────────────────────────────
 * Count the indexable files under repo_path without building a result list,
 * stopping as soon as the count exceeds `limit`. Serves as the auto-index
 * OOM guard for non-git roots, where `git ls-files` cannot provide a count.
 * Honors the same hardcoded skip dirs, suffix filters, and language gate as
 * full discovery (mode FULL) but deliberately NOT gitignore: over-counting
 * only makes the guard more conservative, and this path must stay cheap. */
int cbm_discover_count_files(const char *repo_path, int limit) {
    if (!repo_path || !repo_path[0] || limit < 0) {
        return 0;
    }

    struct stat st;
    if (wide_stat(repo_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }

    /* Dynamic stack of absolute directory paths (heap-owned). */
    int cap = CBM_SZ_64;
    char **stack = malloc((size_t)cap * sizeof(char *));
    if (!stack) {
        return 0;
    }
    stack[0] = strdup(repo_path);
    if (!stack[0]) {
        free(stack);
        return 0;
    }
    int top = 1;

    int count = 0;
    while (top > 0 && count <= limit) {
        char *dir = stack[--top];
        cbm_dir_t *d = cbm_opendir(dir);
        if (!d) {
            free(dir);
            continue;
        }

        cbm_dirent_t *entry;
        while (count <= limit && (entry = cbm_readdir(d)) != NULL) {
            if (entry->is_dir) {
                if (cbm_should_skip_dir(entry->name, CBM_MODE_FULL)) {
                    continue;
                }
                char sub[CBM_SZ_4K];
                snprintf(sub, sizeof(sub), "%s/%s", dir, entry->name);
                /* safe_stat skips symlinks / junctions so the walk cannot
                 * cycle or escape the root — same policy as discovery. */
                struct stat sst;
                if (safe_stat(sub, &sst) != 0 || !S_ISDIR(sst.st_mode)) {
                    continue;
                }
                if (top >= cap) {
                    int new_cap = cap * PAIR_LEN;
                    char **grown = realloc(stack, (size_t)new_cap * sizeof(char *));
                    if (!grown) {
                        continue; /* OOM: skip subtree — count stays a lower bound */
                    }
                    stack = grown;
                    cap = new_cap;
                }
                char *copy = strdup(sub);
                if (copy) {
                    stack[top++] = copy;
                }
            } else {
                if (cbm_has_ignored_suffix(entry->name, CBM_MODE_FULL)) {
                    continue;
                }
                if (cbm_language_for_filename(entry->name) == CBM_LANG_COUNT) {
                    continue;
                }
                count++;
            }
        }
        cbm_closedir(d);
        free(dir);
    }

    for (int i = 0; i < top; i++) {
        free(stack[i]);
    }
    free(stack);
    return count;
}

void cbm_discover_free_ignored(cbm_ignored_file_t *ignored, int count) {
    if (!ignored) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(ignored[i].rel_path);
        free(ignored[i].reason);
    }
    free(ignored);
}

const char *cbm_discover_limit_name(cbm_discover_limit_t limit) {
    switch (limit) {
    case CBM_DISCOVER_LIMIT_FILES:
        return "files";
    case CBM_DISCOVER_LIMIT_DIRECTORIES:
        return "directories";
    case CBM_DISCOVER_LIMIT_ENTRIES:
        return "entries";
    case CBM_DISCOVER_LIMIT_DEPTH:
        return "depth";
    case CBM_DISCOVER_LIMIT_SOURCE_BYTES:
        return "source_bytes";
    case CBM_DISCOVER_LIMIT_DEADLINE:
        return "deadline";
    case CBM_DISCOVER_LIMIT_NONE:
    default:
        return "none";
    }
}
