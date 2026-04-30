# smoothfs control-plane schema contract

This document is the schema contract consumed by the in-repository
`controlplane` package and by SmoothNAS/tierd migrations outside this repository.
The test fixture in `controlplane/testdb_test.go` is intentionally small; this
page records the minimum production surface that must remain compatible with the
code.

Production migrations may add tables, indexes, columns, triggers, and stricter
constraints. They must not remove or rename the columns listed here without a
matching control-plane code change.

## Scope

The schema stores:

- placement-domain and tier topology used to register mounted smoothfs pools
- per-object placement and movement state
- heat aggregation inputs used by the planner
- movement transition history used for recovery and operator diagnostics
- policy keys used by planner cadence, heat decay, and anti-thrash gates

SQLite foreign-key enforcement should be enabled for production connections:

```sql
PRAGMA foreign_keys = ON;
```

## Required Tables

### `placement_domains`

Placement domains group tier targets and namespaces.

```sql
CREATE TABLE IF NOT EXISTS placement_domains (
    id           TEXT PRIMARY KEY,
    backend_kind TEXT NOT NULL
);
```

Required semantics:

- `id` is referenced by `tier_targets.placement_domain` and
  `managed_namespaces.placement_domain`.
- `backend_kind` must be `smoothfs` for domains served by this control-plane
  package.

### `tier_targets`

Tier targets describe the ordered lower tiers available to a placement domain.

```sql
CREATE TABLE IF NOT EXISTS tier_targets (
    id                 TEXT PRIMARY KEY,
    name               TEXT NOT NULL,
    placement_domain   TEXT NOT NULL,
    backend_kind        TEXT NOT NULL,
    rank               INTEGER NOT NULL,
    target_fill_pct    INTEGER NOT NULL DEFAULT 0,
    full_threshold_pct INTEGER NOT NULL DEFAULT 100,
    backing_ref         TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (placement_domain) REFERENCES placement_domains(id)
);
```

Required semantics:

- `rank` is the kernel-visible smoothfs tier rank. Lower rank means hotter or
  more preferred storage.
- `target_fill_pct` is currently advisory; it is loaded into `TierInfo.TargetPct`
  for policy/UI consumers.
- `full_threshold_pct` blocks movement into a destination tier when current
  filesystem fill is at or above the threshold.
- `backing_ref` is the lower directory path when known from configuration. If it
  is empty, mount-ready events must provide a lower path for the same rank.
- For a smoothfs placement domain, ranks should be unique and contiguous from
  `0` through `n-1`.

Recommended production indexes:

```sql
CREATE INDEX IF NOT EXISTS tier_targets_domain_backend_rank
    ON tier_targets(placement_domain, backend_kind, rank);
```

### `managed_namespaces`

Namespaces bind an appliance namespace to a placement domain and a smoothfs
pool identity.

```sql
CREATE TABLE IF NOT EXISTS managed_namespaces (
    id               TEXT PRIMARY KEY,
    name             TEXT NOT NULL,
    placement_domain TEXT NOT NULL,
    backend_kind     TEXT NOT NULL,
    backend_ref      TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (placement_domain) REFERENCES placement_domains(id)
);
```

Required semantics:

- `backend_kind` must be `smoothfs` for namespaces served by this package.
- `backend_ref` should be the smoothfs pool UUID string. The service falls back
  to `name` for legacy rows, so production migrations should keep `name` stable
  until all pools have UUID-backed `backend_ref` values.

Recommended production indexes:

```sql
CREATE INDEX IF NOT EXISTS managed_namespaces_backend_ref
    ON managed_namespaces(backend_kind, backend_ref);

CREATE INDEX IF NOT EXISTS managed_namespaces_backend_name
    ON managed_namespaces(backend_kind, name);
```

### `smoothfs_objects`

Objects are the authoritative control-plane mirror of kernel object placement.
`object_id` is the lowercase hex encoding of the 16-byte smoothfs object id.

```sql
CREATE TABLE IF NOT EXISTS smoothfs_objects (
    object_id                  TEXT PRIMARY KEY,
    namespace_id               TEXT NOT NULL,
    current_tier_id            TEXT NOT NULL,
    intended_tier_id           TEXT,
    movement_state             TEXT NOT NULL DEFAULT 'placed',
    pin_state                  TEXT NOT NULL DEFAULT 'none',
    transaction_seq            INTEGER NOT NULL DEFAULT 0,
    last_committed_cutover_gen INTEGER NOT NULL DEFAULT 0,
    failure_reason             TEXT NOT NULL DEFAULT '',
    ewma_value                 REAL NOT NULL DEFAULT 0.0,
    last_heat_sample_at        TEXT,
    last_movement_at           TEXT,
    created_at                 TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at                 TEXT NOT NULL DEFAULT (datetime('now')),
    rel_path                   TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (namespace_id) REFERENCES managed_namespaces(id),
    FOREIGN KEY (current_tier_id) REFERENCES tier_targets(id),
    FOREIGN KEY (intended_tier_id) REFERENCES tier_targets(id)
);
```

Required `movement_state` values:

```text
placed
plan_accepted
destination_reserved
copy_in_progress
copy_complete
copy_verified
cutover_in_progress
switched
cleanup_in_progress
cleanup_complete
failed
stale
```

Required `pin_state` values:

```text
none
pin_hot
pin_cold
pin_hardlink
pin_lease
pin_lun
```

Required semantics:

- `object_id` must decode to exactly 16 bytes. Production migrations should
  enforce `length(object_id) = 32` for smoothfs objects.
- `namespace_id` must match the pool namespace registered with the planner.
- `current_tier_id` is the tier currently authoritative for reads.
- `intended_tier_id` is non-null while a movement is planned or in flight.
- `transaction_seq` stores the movement sequence assigned by the pool.
- `last_committed_cutover_gen` increments when the worker finalizes a cutover or
  recovery forwards a post-cutover movement.
- `ewma_value` and `last_heat_sample_at` are updated by `HeatAggregator`.
- `last_movement_at` is used with `created_at` to enforce min-residency and
  cooldown gates.
- `rel_path` is the normalized path of the object relative to each lower tier.
  It is required for nested cutover, recovery pin repair, and active-LUN
  movement.

Recommended production checks and indexes:

```sql
CREATE INDEX IF NOT EXISTS smoothfs_objects_namespace
    ON smoothfs_objects(namespace_id);

CREATE INDEX IF NOT EXISTS smoothfs_objects_movement
    ON smoothfs_objects(namespace_id, movement_state)
    WHERE movement_state NOT IN ('placed', 'cleanup_complete', 'failed', 'stale');

CREATE INDEX IF NOT EXISTS smoothfs_objects_namespace_rel_path
    ON smoothfs_objects(namespace_id, rel_path);
```

### `smoothfs_movement_log`

The movement log is append-only history for recovery review, UI diagnostics, and
operator escalation.

```sql
CREATE TABLE IF NOT EXISTS smoothfs_movement_log (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    object_id       TEXT NOT NULL,
    transaction_seq INTEGER NOT NULL,
    from_state      TEXT,
    to_state        TEXT NOT NULL,
    source_tier     TEXT NOT NULL,
    dest_tier       TEXT NOT NULL,
    payload_json    TEXT NOT NULL DEFAULT '{}',
    written_at      TEXT NOT NULL DEFAULT (datetime('now'))
);
```

Required semantics:

- The worker writes every state transition it attempts to make visible.
- `payload_json` is `{}` for normal transitions and may include a `reason`
  string for failure or cleanup diagnostics.
- Consumers must order by `id` or `written_at` rather than assuming one row per
  object.

Recommended production indexes:

```sql
CREATE INDEX IF NOT EXISTS smoothfs_movement_log_object
    ON smoothfs_movement_log(object_id, id);

CREATE INDEX IF NOT EXISTS smoothfs_movement_log_written
    ON smoothfs_movement_log(written_at);
```

### `control_plane_config`

Policy values are string encoded so migrations and appliance settings can share
one table.

```sql
CREATE TABLE IF NOT EXISTS control_plane_config (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

Required keys and default values:

```sql
INSERT OR IGNORE INTO control_plane_config (key, value) VALUES
    ('smoothfs_heat_halflife_seconds', '86400'),
    ('smoothfs_planner_interval_seconds', '900'),
    ('smoothfs_min_residency_seconds', '3600'),
    ('smoothfs_movement_cooldown_seconds', '21600'),
    ('smoothfs_hysteresis_pct', '20'),
    ('smoothfs_promote_percentile', '80'),
    ('smoothfs_demote_percentile', '20');
```

The Go loader tolerates missing keys by applying the same defaults, but
production migrations should seed them so UI/API layers can read and update a
complete policy row set.

## Movement State Recovery Contract

`Recover` scans rows where `movement_state` is not one of:

```text
placed
cleanup_complete
stale
```

Recovery behavior:

- `plan_accepted`, `destination_reserved`, `copy_in_progress`,
  `copy_complete`, and `copy_verified` roll back to `placed` on
  `current_tier_id`.
- `cutover_in_progress`, `switched`, and `cleanup_in_progress` forward to
  `placed` on `intended_tier_id` when the destination tier is present.
- `failed` rows are normally left failed. A failed LUN row with `pin_lun` and a
  destination tier preserves the destination tier before attempting pin repair.
- LUN pin repair requires `rel_path` plus a resolvable `tier_targets.backing_ref`
  for the final tier.

## Planner Read Contract

The planner reads `smoothfs_objects` rows with:

```sql
WHERE namespace_id = ?
  AND movement_state = 'placed'
```

It requires:

- `object_id` decodable from 32 lowercase hex characters to 16 bytes
- `current_tier_id` present in the registered pool's tier list
- `rel_path` populated for movement plans that cannot rely on kernel inspect to
  backfill it
- `ewma_value` populated by the heat aggregator or seeded to `0.0`
- `pin_state` set to `none` for normal background movement

Pinned objects are skipped by normal planning. Active LUN movement uses the
explicit quiesce path and requires a DB row with `pin_state = 'pin_lun'` while
kernel inspect reports `pin_state = 'none'`.

## Drift Checks

Integration tests for the external migration owner should fail if any of these
queries cannot run:

```sql
SELECT id, rank, target_fill_pct, full_threshold_pct, backing_ref
  FROM tier_targets
 WHERE placement_domain = ?
   AND backend_kind = 'smoothfs'
 ORDER BY rank;

SELECT id, placement_domain
  FROM managed_namespaces
 WHERE backend_kind = 'smoothfs'
   AND (backend_ref = ? OR name = ?)
 LIMIT 1;

SELECT object_id, current_tier_id, rel_path, ewma_value,
       COALESCE(last_movement_at, created_at), pin_state
  FROM smoothfs_objects
 WHERE namespace_id = ?
   AND movement_state = 'placed';

SELECT ewma_value, last_heat_sample_at
  FROM smoothfs_objects
 WHERE object_id = ?;

SELECT object_id, current_tier_id, intended_tier_id, movement_state,
       transaction_seq, pin_state, namespace_id, rel_path
  FROM smoothfs_objects
 WHERE movement_state NOT IN ('placed','cleanup_complete','stale');
```

Production migrations should also verify that the required config keys exist and
that every `smoothfs_objects.object_id` is a 32-character hex string.
