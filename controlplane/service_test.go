package controlplane

import (
	"context"
	"testing"

	"github.com/google/uuid"
)

func TestDiscoverPoolFromMountEvent(t *testing.T) {
	sqlDB := testDB(t)
	poolUUID := uuid.MustParse("11111111-2222-4333-8444-555555555555")
	domainID := "domain-smoothfs"

	exec := func(q string, args ...any) {
		if _, err := sqlDB.Exec(q, args...); err != nil {
			t.Fatalf("seed exec %q: %v", q, err)
		}
	}
	exec(`INSERT INTO placement_domains (id, backend_kind) VALUES (?, ?)`, domainID, "smoothfs")
	exec(`INSERT INTO tier_targets
	        (id, name, placement_domain, backend_kind, rank, target_fill_pct, full_threshold_pct, backing_ref)
	        VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		"tier-fast", "fast", domainID, "smoothfs", 0, 50, 95, "/mnt/fast")
	exec(`INSERT INTO tier_targets
	        (id, name, placement_domain, backend_kind, rank, target_fill_pct, full_threshold_pct, backing_ref)
	        VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		"tier-slow", "slow", domainID, "smoothfs", 1, 60, 98, "/mnt/slow")
	exec(`INSERT INTO managed_namespaces
	        (id, name, placement_domain, backend_kind, backend_ref)
	        VALUES (?, ?, ?, ?, ?)`,
		"ns-smoothfs", "pool-a", domainID, "smoothfs", poolUUID.String())

	ev := &Event{
		PoolUUID: poolUUID,
		PoolName: "pool-a",
		Tiers: []MountedTier{
			{Rank: 0, Path: "/mnt/fast"},
			{Rank: 1, Path: "/mnt/slow"},
		},
	}
	pool, err := DiscoverPoolFromDB(context.Background(), sqlDB, ev)
	if err != nil {
		t.Fatalf("DiscoverPoolFromDB: %v", err)
	}
	if pool.NamespaceID != "ns-smoothfs" {
		t.Fatalf("namespace_id = %q, want ns-smoothfs", pool.NamespaceID)
	}
	if len(pool.Tiers) != 2 {
		t.Fatalf("tiers = %d, want 2", len(pool.Tiers))
	}
	if pool.Tiers[0].TargetID != "tier-fast" || pool.Tiers[0].LowerDir != "/mnt/fast" {
		t.Fatalf("tier[0] = %+v", pool.Tiers[0])
	}
	if pool.Tiers[1].TargetID != "tier-slow" || pool.Tiers[1].LowerDir != "/mnt/slow" {
		t.Fatalf("tier[1] = %+v", pool.Tiers[1])
	}
}

func TestDiscoverPoolFallsBackToBackingRefWhenMountEventIsPartial(t *testing.T) {
	sqlDB := testDB(t)
	poolUUID := uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")
	domainID := "domain-smoothfs"

	exec := func(q string, args ...any) {
		if _, err := sqlDB.Exec(q, args...); err != nil {
			t.Fatalf("seed exec %q: %v", q, err)
		}
	}
	exec(`INSERT INTO placement_domains (id, backend_kind) VALUES (?, ?)`, domainID, "smoothfs")
	exec(`INSERT INTO tier_targets
	        (id, name, placement_domain, backend_kind, rank, target_fill_pct, full_threshold_pct, backing_ref)
	        VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		"tier-fast", "fast", domainID, "smoothfs", 0, 50, 95, "/mnt/fast")
	exec(`INSERT INTO tier_targets
	        (id, name, placement_domain, backend_kind, rank, target_fill_pct, full_threshold_pct, backing_ref)
	        VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		"tier-slow", "slow", domainID, "smoothfs", 1, 60, 98, "/mnt/slow")
	exec(`INSERT INTO managed_namespaces
	        (id, name, placement_domain, backend_kind, backend_ref)
	        VALUES (?, ?, ?, ?, ?)`,
		"ns-smoothfs", "pool-a", domainID, "smoothfs", poolUUID.String())

	ev := &Event{
		PoolUUID: poolUUID,
		PoolName: "pool-a",
		Tiers: []MountedTier{
			{Rank: 1, Path: "/runtime/slow"},
		},
	}
	pool, err := DiscoverPoolFromDB(context.Background(), sqlDB, ev)
	if err != nil {
		t.Fatalf("DiscoverPoolFromDB: %v", err)
	}
	if len(pool.Tiers) != 2 {
		t.Fatalf("tiers = %d, want 2", len(pool.Tiers))
	}
	if pool.Tiers[0].LowerDir != "/mnt/fast" {
		t.Fatalf("tier[0].LowerDir = %q, want /mnt/fast", pool.Tiers[0].LowerDir)
	}
	if pool.Tiers[1].LowerDir != "/mnt/slow" {
		t.Fatalf("tier[1].LowerDir = %q, want /mnt/slow", pool.Tiers[1].LowerDir)
	}
}
