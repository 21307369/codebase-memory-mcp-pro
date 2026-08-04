# Index Resource Limits

Indexing is intentionally bounded. Before parsing begins, the pipeline
canonicalizes the repository root, validates it against the built-in dangerous
roots, and performs a bounded discovery walk. All limits below are enforced
inside the single indexing process — this fork has no daemon, supervisor, or
worker subprocess (see `docs/changes/index-resource-guards.md`).

If a hard boundary is reached, `index_repository` returns
`resource_limit_exceeded` with the boundary name, observed value, configured
limit, and a remediation hint naming the config key. A limit failure is never
reported as a successful or merely empty index.

## Enforcement status

### Enforced

| Configuration key | Default | Enforced behavior |
|---|---:|---|
| `index_max_files` | `100000` | Discovery stops when the accepted source-file count exceeds this |
| `index_max_directories` | `20000` | Discovery stops when traversed directories (root counts as one) exceed this |
| `index_max_entries` | `500000` | Discovery stops when examined filesystem entries exceed this |
| `index_max_depth` | `64` | Discovery stops below this directory nesting depth (root is depth 0) |
| `index_max_source_mb` | `4096` | Discovery stops when the aggregate accepted source size exceeds this |
| `index_max_file_mb` | `64` | Larger source files are skipped (and reported as ignored) |
| `index_scan_timeout_seconds` | `30` | Discovery walk deadline; the walk stops and the run fails |
| `index_cpu_cores` | `4` | Cap on parallel extraction worker threads |
| `index_max_db_mb` | `16384` | Committed SQLite DB (plus `-wal`/`-shm`/`-journal` sidecars) must fit |
| `index_min_free_disk_mb` | `4096` | Pre-flight free-disk reserve on the DB's filesystem |
| `index_denied_roots` | empty | Additional exact canonical roots that are refused |

### Accepted but not enforced

The following keys are validated and stored by `config set`, but have **no
effect** in this fork. They come from the upstream daemon/worker architecture
(worker process memory caps, wall-clock supervision, scheduling priority,
staging/task/cache quotas), whose subsystems were deleted in this fork. They
are listed so configurations remain portable and invalid values are still
rejected.

| Configuration key | Default | Why it is not enforced |
|---|---:|---|
| `index_memory_limit_mb` | `8192` | No supervised worker process to cap; indexing shares the server's memory |
| `index_max_duration_seconds` | `3600` | No wall-clock supervisor |
| `index_low_priority` | `true` | No scheduling-priority control in-process |
| `index_max_staging_mb` | `20480` | No staging database — the fork writes the DB in place |
| `index_max_task_temp_mb` | `24576` | No worker temporary-output accounting |
| `index_cache_max_mb` | `32768` | No cache admission layer |
| `index_concurrent_jobs` | `1` | No multi-job scheduler; a single global pipeline lock serializes runs |

## Defaults

Set a value with:

```sh
codebase-memory-mcp config set index_max_files 75000
codebase-memory-mcp config set index_max_db_mb 8192
```

Separate additional denied roots with semicolons on every platform:

```sh
codebase-memory-mcp config set index_denied_roots '/path/to/aggregate;/path/to/archive'
```

Numeric values must be positive and within the ranges shown by
`codebase-memory-mcp config --help`. `index_denied_roots` may be empty. Invalid
or unknown keys are rejected. Automatic indexing keeps its separate, stricter
`auto_index_limit` default of `50000`.

A corrupt stored value (for example, hand-edited config) makes the pipeline
fall back to defaults with a logged warning — indexing is never bricked by a
bad config value.

## Root safety

The filesystem root, current user's home directory, and the Codebase Memory
cache directory are always refused as repository roots. This built-in check
cannot be disabled. The repository path, home directory, and cache directory
are canonicalized (symlinks, `..`, `.`, trailing slashes resolved) before
comparison; a repository path that cannot be resolved fails the run closed.
`index_denied_roots` adds exact canonical roots; it does not deny all
descendants, so individual repositories below an aggregation directory can
still be indexed.

`CBM_ALLOWED_ROOT` remains available for deployments that also need an outer
containment boundary.

`cross-repo-intelligence` is not a source-indexing mode: it reads and updates
already published project databases and does not walk `repo_path`. It therefore
keeps the session containment and cross-repository target-count checks, but does
not apply source-root or discovery limits.

## Failure and recovery

- **Discovery violations** (`index_max_files`, `index_max_directories`,
  `index_max_entries`, `index_max_depth`, `index_max_source_mb`,
  `index_scan_timeout_seconds`) stop the walk **before any database is
  touched**: the previously published index remains usable.
- **Free-disk violations** (`index_min_free_disk_mb`) are a pre-flight check,
  also before any database is touched: the previously published index remains
  usable.
- **Database-size violations** (`index_max_db_mb`) are checked **after** the
  commit, because the fork writes the database in place — there is no staging
  layer to roll back, and the previous index was already replaced by the run
  (a full reindex deletes the old DB first; an incremental run overwrites it in
  place). The oversized database and its sidecars are **deleted** and the run
  returns `resource_limit_exceeded`: a violation leaves **no index**, and the
  next run reindexes from scratch. This applies to both the full-reindex and
  the incremental path.
- Root refusals happen before anything is read or written.

The indexer never deletes unrelated project indexes automatically.
