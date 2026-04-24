package controlplane

import (
	"database/sql"
	"path/filepath"
	"testing"

	_ "github.com/mattn/go-sqlite3"
)

func testDB(t *testing.T) *sql.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "smoothfs-controlplane.db")
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		t.Fatalf("open db: %v", err)
	}
	t.Cleanup(func() { _ = db.Close() })

	stmts := []string{
		`PRAGMA foreign_keys = ON;`,
		`CREATE TABLE placement_domains (
			id TEXT PRIMARY KEY,
			backend_kind TEXT NOT NULL
		);`,
		`CREATE TABLE tier_targets (
			id TEXT PRIMARY KEY,
			name TEXT NOT NULL,
			placement_domain TEXT NOT NULL,
			backend_kind TEXT NOT NULL,
			rank INTEGER NOT NULL,
			target_fill_pct INTEGER NOT NULL DEFAULT 0,
			full_threshold_pct INTEGER NOT NULL DEFAULT 100,
			backing_ref TEXT NOT NULL DEFAULT '',
			FOREIGN KEY(placement_domain) REFERENCES placement_domains(id)
		);`,
		`CREATE TABLE managed_namespaces (
			id TEXT PRIMARY KEY,
			name TEXT NOT NULL,
			placement_domain TEXT NOT NULL,
			backend_kind TEXT NOT NULL,
			backend_ref TEXT NOT NULL DEFAULT '',
			FOREIGN KEY(placement_domain) REFERENCES placement_domains(id)
		);`,
		`CREATE TABLE smoothfs_objects (
			object_id TEXT PRIMARY KEY,
			namespace_id TEXT NOT NULL,
			current_tier_id TEXT NOT NULL,
			intended_tier_id TEXT,
			movement_state TEXT NOT NULL DEFAULT 'placed',
			pin_state TEXT NOT NULL DEFAULT 'none',
			transaction_seq INTEGER NOT NULL DEFAULT 0,
			last_committed_cutover_gen INTEGER NOT NULL DEFAULT 0,
			failure_reason TEXT NOT NULL DEFAULT '',
			ewma_value REAL NOT NULL DEFAULT 0.0,
			last_heat_sample_at TEXT,
			last_movement_at TEXT,
			created_at TEXT NOT NULL DEFAULT (datetime('now')),
			updated_at TEXT NOT NULL DEFAULT (datetime('now')),
			rel_path TEXT NOT NULL DEFAULT ''
		);`,
		`CREATE TABLE smoothfs_movement_log (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			object_id TEXT NOT NULL,
			transaction_seq INTEGER NOT NULL,
			from_state TEXT,
			to_state TEXT NOT NULL,
			source_tier TEXT NOT NULL,
			dest_tier TEXT NOT NULL,
			payload_json TEXT NOT NULL DEFAULT '{}',
			written_at TEXT NOT NULL DEFAULT (datetime('now'))
		);`,
		`CREATE TABLE control_plane_config (
			key TEXT PRIMARY KEY,
			value TEXT NOT NULL
		);`,
		`INSERT INTO control_plane_config (key, value) VALUES
			('smoothfs_heat_halflife_seconds', '86400'),
			('smoothfs_planner_interval_seconds', '900'),
			('smoothfs_min_residency_seconds', '3600'),
			('smoothfs_movement_cooldown_seconds', '21600'),
			('smoothfs_hysteresis_pct', '20'),
			('smoothfs_promote_percentile', '80'),
			('smoothfs_demote_percentile', '20');`,
	}
	for _, stmt := range stmts {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatalf("init schema: %v\nsql=%s", err, stmt)
		}
	}
	return db
}

func seedPool(t *testing.T, sqlDB *sql.DB) (nsID, tier0ID, tier1ID string) {
	t.Helper()
	nsID = "ns-smoothfs-test"
	tier0ID = "tier-fast"
	tier1ID = "tier-slow"
	domainID := "domain-test"

	exec := func(q string, args ...any) {
		if _, err := sqlDB.Exec(q, args...); err != nil {
			t.Fatalf("seed exec %q: %v", q, err)
		}
	}
	exec(`INSERT INTO placement_domains (id, backend_kind) VALUES (?, ?)`, domainID, "smoothfs")
	exec(`INSERT INTO tier_targets (id, name, placement_domain, backend_kind, rank)
	        VALUES (?, ?, ?, ?, ?)`, tier0ID, "fast", domainID, "smoothfs", 0)
	exec(`INSERT INTO tier_targets (id, name, placement_domain, backend_kind, rank)
	        VALUES (?, ?, ?, ?, ?)`, tier1ID, "slow", domainID, "smoothfs", 1)
	exec(`INSERT INTO managed_namespaces (id, name, placement_domain, backend_kind)
	        VALUES (?, ?, ?, ?)`, nsID, "test", domainID, "smoothfs")
	return
}
