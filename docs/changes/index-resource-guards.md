# Index Resource Guard Design

## Problem

An explicit `index_repository` request currently trusts the requested directory
and lets discovery, parsing, and persistence grow until the operation finishes
or the host runs out of resources. This is unsafe for aggregation directories,
accidental filesystem roots, and repositories with unexpectedly large generated
trees.

## Behavior contract

The indexer resolves one immutable resource policy at admission time. The same
policy is applied to discovery, extraction, and the database commit. A limit
failure is reported as `resource_limit_exceeded`; it is never reported as a
successful or merely empty index.

This fork is **in-process**: there is no daemon admission step, no supervised
worker subprocess, and no separate staging database. The pipeline runs inside
the server process and writes the SQLite database in place. Anything the
upstream design says about a worker process tree, a supervisor, or staging
files therefore does not apply here.

### BDD scenarios

1. **Dangerous root**
   - Given an explicit request whose canonical path is the filesystem root,
     current user's home directory, the cache directory, or a configured
     denied root
   - When `index_repository` validates the request
   - Then it refuses the request before discovery starts
   - And the response identifies the `repository_root` policy
   - And the repository, home, and cache paths are canonicalized (symlinks,
     `..`, `.`, trailing slashes) before comparison; an unresolvable repository
     path fails the run closed

2. **Bounded discovery**
   - Given a tree that exceeds the configured file, directory, entry, depth,
     aggregate indexable-source-byte, single-file, or scan-timeout limit
   - When discovery reaches that boundary
   - Then the walk stops immediately and identifies the exact boundary
   - And no partial file list is passed to parsing
   - And the previously published database remains usable (discovery runs
     before any database is touched)

3. **Bounded compute**
   - Given default configuration on a large host
   - When an index job starts
   - Then at most `index_cpu_cores` (default 4) parallel extraction workers are
     used
   - And a global pipeline lock ensures only one physical indexing job runs at
     a time

4. **Bounded persistence**
   - Given an index whose committed database (including `-wal`/`-shm`/
     `-journal` sidecars) exceeds `index_max_db_mb`, or whose target
     filesystem lacks the `index_min_free_disk_mb` reserve
   - When the run commits
   - Then a free-disk violation is refused pre-flight and the previously
     published database remains usable
   - And a database-size violation is detected after the in-place commit (the
     old index was already replaced — there is no staging layer to roll back):
     the oversized database and its sidecars are deleted, the run returns
     `resource_limit_exceeded`, and **no index remains** until the next run
   - And the same post-commit size check applies to the incremental path, which
     also commits in place

5. **Configuration validation**
   - Given an unknown resource key, a non-integer value, zero, or a value
     outside the supported range
   - When `config set` is invoked
   - Then configuration is rejected without changing the previous value
   - And a corrupt stored value makes the pipeline fall back to defaults with a
     logged warning — indexing is never bricked by bad config

6. **Public-repository hygiene**
   - Given the final change
   - When tracked content is scanned
   - Then it contains only generic examples such as `/path/to/repo`
   - And it contains no developer usernames, workstation paths, private
     repository names, tokens, or index contents

7. **Cross-repository database linking**
   - Given `mode="cross-repo-intelligence"` and existing source and target
     databases
   - When `index_repository` handles the request
   - Then it preserves session containment and target validation
   - And it does not apply source-root or discovery limits because it does not
     walk the source directory

## Resource policy

All byte-size settings use MiB at the user-facing configuration boundary and
are converted with overflow checks.

| Key | Default | Status |
|---|---:|---|
| `index_max_files` | 100,000 | Enforced: accepted source files |
| `index_max_directories` | 20,000 | Enforced: traversed directories, including root |
| `index_max_entries` | 500,000 | Enforced: filesystem entries examined |
| `index_max_depth` | 64 | Enforced: root is depth 0 |
| `index_max_source_mb` | 4,096 | Enforced: aggregate accepted source bytes |
| `index_max_file_mb` | 64 | Enforced: oversized files are skipped and reported |
| `index_scan_timeout_seconds` | 30 | Enforced: discovery deadline |
| `index_cpu_cores` | 4 | Enforced: maximum parallel extraction workers |
| `index_max_db_mb` | 16,384 | Enforced: post-commit database ceiling (delete on violation) |
| `index_min_free_disk_mb` | 4,096 | Enforced: pre-flight free-disk reserve |
| `index_denied_roots` | empty | Enforced: additional canonical roots, semicolon-separated |
| `index_memory_limit_mb` | 8,192 | Accepted, not enforced: no supervised worker process |
| `index_max_duration_seconds` | 3,600 | Accepted, not enforced: no wall-clock supervisor |
| `index_low_priority` | `true` | Accepted, not enforced: no in-process priority control |
| `index_max_staging_mb` | 20,480 | Accepted, not enforced: no staging database |
| `index_max_task_temp_mb` | 24,576 | Accepted, not enforced: no worker temporary-output accounting |
| `index_cache_max_mb` | 32,768 | Accepted, not enforced: no cache admission layer |
| `index_concurrent_jobs` | 1 | Accepted, not enforced: one global pipeline lock instead |

Built-in dangerous roots cannot be disabled. Operators may lower or raise
numeric values within documented validation ranges. `0` never means unlimited;
it is rejected.

## Data model and state transitions

`cbm_index_limits_t` is the resolved, byte-based policy. Discovery additionally
returns counters and a stable violation enum. The index request progresses:

`canonicalize -> root guard -> free-disk pre-flight -> bounded discovery ->
extraction -> commit -> post-commit size check`

Any resource violation transitions to `resource_limit_exceeded`:

- Root, free-disk, and discovery violations occur before the old index is
  touched: the previously published database remains usable.
- The post-commit size check runs after the in-place commit, when the old index
  is already gone. An oversized database is deleted so no out-of-policy index
  survives; the next run reindexes from scratch.

## Shared entry paths

All three indexing entry points load the same persisted policy through
`cbm_config_load_index_limits` and apply it to the pipeline before the run:

- `handle_index_repository` (MCP tool) and the one-shot CLI both enter the
  MCP server handler.
- The auto-index thread applies the policy for session-start indexing.
- The file watcher applies the policy for change-triggered reindexes.

`cross-repo-intelligence` remains a database-linking path with its existing
target-count and mutation guards; it does not enter source discovery.

## Tests and fixtures

- `tests/test_index_limits.c` covers defaults, per-key validation, unit
  conversion, root matching, and fail-closed parsing.
- `tests/test_discover.c` covers bounded-walk violations (files, directories,
  entries, depth, source bytes, deadline) and early loop termination.
- `tests/test_pipeline.c` adds two resource-limit pipeline tests:
  `discovery_resource_limit_preserves_committed_db` (a discovery violation
  leaves the previous DB untouched) and
  `storage_resource_limits_preserve_committed_db` (a pre-flight free-disk
  violation preserves the previous DB; a post-commit database-size violation
  deletes the oversized DB and returns `resource_limit_exceeded`).
- `tests/test_cli.c` covers `config set` rejection of invalid values.

Note: `suite_pipeline` is currently **not** wired into `tests/test_main.c`
because it carries six pre-existing failures unrelated to this change
(`pipeline_structure_nodes`, the three `*_fetch_*_classified_as_http_calls`
tests, and two k8s tests). The two resource-limit tests above are part of that
suite and will run once the pre-existing failures are fixed.

## Installation and migration

No database migration is required. Existing indexes remain readable. The new
limits affect the next indexing operation. Existing environment variables
remain compatible.

## Non-goals

- Exact percentage CPU throttling. Worker-count caps (`index_cpu_cores`) and
  single-job admission (global pipeline lock) provide portable bounded CPU use.
- Automatic deletion of unrelated cache entries. The indexer never deletes
  another project's data.
- Following symlinks, junctions, or paths outside the canonical requested root.
- Per-request public overrides that let an untrusted MCP caller weaken operator
  policy.
- Worker-process memory supervision, wall-clock duration supervision, and
  scheduling-priority control — the daemon/worker architecture those belong to
  does not exist in this fork.
