package controlplane

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

func TestReconcilePoolSubtreesRemovesEmptyDuplicateDirs(t *testing.T) {
	fast := t.TempDir()
	slow := t.TempDir()
	mustMkdirAll(t, filepath.Join(fast, "a", "b"))
	mustMkdirAll(t, filepath.Join(slow, "a", "b"))

	pool := &Pool{
		UUID:        uuid.MustParse("11111111-2222-4333-8444-555555555555"),
		Name:        "pool-a",
		NamespaceID: "ns-a",
		Tiers: []TierInfo{
			{Rank: 0, TargetID: "fast", LowerDir: fast},
			{Rank: 1, TargetID: "slow", LowerDir: slow},
		},
	}

	if err := ReconcilePoolSubtrees(pool); err != nil {
		t.Fatalf("ReconcilePoolSubtrees: %v", err)
	}

	if _, err := os.Stat(filepath.Join(slow, "a", "b")); !os.IsNotExist(err) {
		t.Fatalf("expected slow duplicate leaf dir removed, stat err=%v", err)
	}
	if _, err := os.Stat(filepath.Join(slow, "a")); !os.IsNotExist(err) {
		t.Fatalf("expected slow duplicate parent dir removed after leaf, stat err=%v", err)
	}
}

func TestReconcilePoolSubtreesKeepsNonEmptyDirs(t *testing.T) {
	fast := t.TempDir()
	slow := t.TempDir()
	mustMkdirAll(t, filepath.Join(fast, "a"))
	mustMkdirAll(t, filepath.Join(slow, "a"))
	if err := os.WriteFile(filepath.Join(slow, "a", "payload.bin"), []byte("x"), 0o644); err != nil {
		t.Fatalf("write payload: %v", err)
	}

	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: "ns-a",
		Tiers: []TierInfo{
			{Rank: 0, TargetID: "fast", LowerDir: fast},
			{Rank: 1, TargetID: "slow", LowerDir: slow},
		},
	}

	if err := ReconcilePoolSubtrees(pool); err != nil {
		t.Fatalf("ReconcilePoolSubtrees: %v", err)
	}

	if _, err := os.Stat(filepath.Join(slow, "a")); err != nil {
		t.Fatalf("expected non-empty slow dir kept: %v", err)
	}
}

func TestReconcilePoolSubtreesKeepsUniqueDirs(t *testing.T) {
	fast := t.TempDir()
	slow := t.TempDir()
	mustMkdirAll(t, filepath.Join(slow, "spill-only"))

	pool := &Pool{
		UUID:        uuid.MustParse("99999999-2222-4333-8444-555555555555"),
		Name:        "pool-a",
		NamespaceID: "ns-a",
		Tiers: []TierInfo{
			{Rank: 0, TargetID: "fast", LowerDir: fast},
			{Rank: 1, TargetID: "slow", LowerDir: slow},
		},
	}

	if err := ReconcilePoolSubtrees(pool); err != nil {
		t.Fatalf("ReconcilePoolSubtrees: %v", err)
	}

	if _, err := os.Stat(filepath.Join(slow, "spill-only")); err != nil {
		t.Fatalf("expected unique slow dir kept: %v", err)
	}
}

func mustMkdirAll(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(path, 0o755); err != nil {
		t.Fatalf("mkdir %s: %v", path, err)
	}
}
