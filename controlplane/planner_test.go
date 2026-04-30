package controlplane

import (
	"context"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
)

func TestPlannerDemotesColdObjectWhenSourceTierIsOverfull(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("11111111-2222-4333-8444-555555555555"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir(), TargetPct: 50, FullPct: 95},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir(), TargetPct: 80, FullPct: 98},
		},
	}

	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, ewma_value, last_movement_at, pin_state, rel_path)
		VALUES
			('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', ?, ?, 'placed', 0.1, ?, 'none', 'cold.bin')`,
		nsID, tier0ID, time.Now().UTC().Format(time.RFC3339Nano))
	if err != nil {
		t.Fatalf("insert object: %v", err)
	}

	origTierFillPct := tierFillPct
	t.Cleanup(func() { tierFillPct = origTierFillPct })
	tierFillPct = func(path string) (int, error) {
		if path == pool.Tiers[0].LowerDir {
			return 97, nil
		}
		return 10, nil
	}

	plans := make(chan MovementPlan, 4)
	planner := NewPlanner(sqlDB, plans, PlannerConfig{
		MinResidencySec:   3600,
		CooldownSec:       21600,
		PromotePercentile: 80,
		DemotePercentile:  20,
		IntervalSec:       900,
	})
	planner.RegisterPool(pool)

	if err := planner.tick(context.Background()); err != nil {
		t.Fatalf("tick: %v", err)
	}

	select {
	case plan := <-plans:
		if plan.SourceTierID != tier0ID {
			t.Fatalf("source tier = %q, want %q", plan.SourceTierID, tier0ID)
		}
		if plan.DestTierID != tier1ID {
			t.Fatalf("dest tier = %q, want %q", plan.DestTierID, tier1ID)
		}
	default:
		t.Fatal("expected a demotion plan while source tier is overfull")
	}
}

func TestPlannerDefersPromotionIntoFullDestination(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir(), TargetPct: 50, FullPct: 95},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir(), TargetPct: 80, FullPct: 90},
		},
	}

	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, ewma_value, last_movement_at, pin_state, rel_path)
		VALUES
			('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb', ?, ?, 'placed', 1000.0, ?, 'none', 'hot.bin')`,
		nsID, tier1ID, time.Now().Add(-48*time.Hour).UTC().Format(time.RFC3339Nano))
	if err != nil {
		t.Fatalf("insert object: %v", err)
	}

	origTierFillPct := tierFillPct
	t.Cleanup(func() { tierFillPct = origTierFillPct })
	tierFillPct = func(path string) (int, error) {
		if path == pool.Tiers[0].LowerDir {
			return 95, nil
		}
		return 20, nil
	}

	plans := make(chan MovementPlan, 4)
	planner := NewPlanner(sqlDB, plans, PlannerConfig{
		MinResidencySec:   3600,
		CooldownSec:       21600,
		PromotePercentile: 80,
		DemotePercentile:  20,
		IntervalSec:       900,
	})
	planner.RegisterPool(pool)

	if err := planner.tick(context.Background()); err != nil {
		t.Fatalf("tick: %v", err)
	}

	select {
	case plan := <-plans:
		t.Fatalf("unexpected plan into full destination tier: %+v", plan)
	default:
	}
}

func TestPlannerRegisterPoolConcurrentWithTick(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	tiers := []TierInfo{
		{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir(), TargetPct: 50, FullPct: 95},
		{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir(), TargetPct: 80, FullPct: 98},
	}
	plans := make(chan MovementPlan, 16)
	planner := NewPlanner(sqlDB, plans, PlannerConfig{
		PromotePercentile: 80,
		DemotePercentile:  20,
		IntervalSec:       900,
	})

	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 100; j++ {
				planner.RegisterPool(&Pool{
					UUID:        uuid.New(),
					Name:        "pool-race",
					NamespaceID: nsID,
					Tiers:       tiers,
				})
			}
		}()
	}

	for i := 0; i < 100; i++ {
		if err := planner.tick(context.Background()); err != nil {
			t.Fatalf("tick: %v", err)
		}
	}
	wg.Wait()
}
