package controlplane

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/google/uuid"
	"github.com/mdlayher/genetlink"
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

func TestServiceUsesLUNResumerInWorkerFactory(t *testing.T) {
	svc := &Service{lunResumer: &fakeLUNTargetResumer{}}
	w := svc.newWorker()
	if w == nil {
		t.Fatal("newWorker() = nil")
	}
	if w.resumeLUNTarget == nil {
		t.Fatal("worker resumeLUNTarget is nil when LUN resumer is configured")
	}
}

func TestServiceCanSetLUNResumerAfterConstruction(t *testing.T) {
	svc := &Service{}
	svc.SetLUNResumer(&fakeLUNTargetResumer{})
	w := svc.newWorker()
	if w == nil {
		t.Fatal("newWorker() = nil")
	}
	if w.resumeLUNTarget == nil {
		t.Fatal("worker resumeLUNTarget is nil after SetLUNResumer")
	}
}

func TestServiceRegisterPoolConcurrentAccess(t *testing.T) {
	svc := &Service{
		planner: NewPlanner(nil, make(chan MovementPlan, 1), PlannerConfig{}),
		pools:   make(map[string]*Pool),
	}

	const (
		writers   = 8
		readers   = 4
		perWriter = 128
	)
	errCh := make(chan string, writers*perWriter)
	var wg sync.WaitGroup

	for i := 0; i < writers; i++ {
		i := i
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < perWriter; j++ {
				id := uuid.New()
				pool := &Pool{
					UUID:        id,
					Name:        fmt.Sprintf("pool-%d-%d", i, j),
					NamespaceID: "ns-smoothfs-test",
					Tiers: []TierInfo{
						{Rank: 0, TargetID: "tier-fast", LowerDir: "/mnt/fast"},
						{Rank: 1, TargetID: "tier-slow", LowerDir: "/mnt/slow"},
					},
				}
				svc.RegisterPool(pool)
				if got := svc.PoolByUUID(id.String()); got != pool {
					errCh <- fmt.Sprintf("PoolByUUID(%s) = %p, want %p", id, got, pool)
				}
			}
		}()
	}
	for i := 0; i < readers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < writers*perWriter; j++ {
				_ = svc.PoolByUUID(uuid.NewString())
			}
		}()
	}

	wg.Wait()
	close(errCh)
	for msg := range errCh {
		t.Error(msg)
	}

	svc.mu.Lock()
	got := len(svc.pools)
	svc.mu.Unlock()
	if got != writers*perWriter {
		t.Fatalf("registered pools = %d, want %d", got, writers*perWriter)
	}
}

func TestServiceRunCancelsBlockedReceive(t *testing.T) {
	sqlDB := testDB(t)
	client := newBlockingServiceClient()
	plans := make(chan MovementPlan, 1)
	svc := &Service{
		db:          sqlDB,
		client:      client,
		heat:        NewHeatAggregator(sqlDB, 86400),
		planner:     NewPlanner(sqlDB, plans, PlannerConfig{IntervalSec: 3600}),
		planChan:    plans,
		workerCount: 1,
		pools:       make(map[string]*Pool),
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)

	go func() {
		done <- svc.Run(ctx)
	}()
	select {
	case <-client.receiveStarted:
	case err := <-done:
		t.Fatalf("Service.Run returned before Receive blocked: %v", err)
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for Receive to block")
	}

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Service.Run returned error: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Service.Run did not return after cancellation")
	}
	if !client.isClosed() {
		t.Fatal("service client was not closed during cancellation")
	}
}

type blockingServiceClient struct {
	receiveStarted chan struct{}
	closed         chan struct{}
	startOnce      sync.Once
	closeOnce      sync.Once
	mu             sync.Mutex
	closedFlag     bool
}

func newBlockingServiceClient() *blockingServiceClient {
	return &blockingServiceClient{
		receiveStarted: make(chan struct{}),
		closed:         make(chan struct{}),
	}
}

func (b *blockingServiceClient) Receive() ([]genetlink.Message, error) {
	b.startOnce.Do(func() { close(b.receiveStarted) })
	<-b.closed
	return nil, context.Canceled
}

func (b *blockingServiceClient) Close() error {
	b.closeOnce.Do(func() {
		b.mu.Lock()
		b.closedFlag = true
		b.mu.Unlock()
		close(b.closed)
	})
	return nil
}

func (b *blockingServiceClient) isClosed() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closedFlag
}

func (b *blockingServiceClient) Inspect(uuid.UUID, [OIDLen]byte) (*InspectResult, error) {
	return nil, errors.New("unexpected Inspect call")
}

func (b *blockingServiceClient) MovePlan(uuid.UUID, [OIDLen]byte, uint8, uint64) error {
	return errors.New("unexpected MovePlan call")
}

func (b *blockingServiceClient) MoveCutover(uuid.UUID, [OIDLen]byte, uint64) error {
	return errors.New("unexpected MoveCutover call")
}
