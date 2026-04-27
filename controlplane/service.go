package controlplane

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"sort"
	"sync"
)

// Service is the top-level wire-up that runs the netlink listener, the
// heat aggregator, the planner, and the worker pool for the lifetime
// of tierd.
type Service struct {
	db          *sql.DB
	client      *Client
	heat        *HeatAggregator
	planner     *Planner
	planChan    chan MovementPlan
	workerCount int
	lunResumer  LUNTargetResumer

	mu    sync.Mutex
	pools map[string]*Pool
}

func NewService(ctx context.Context, db *sql.DB, workerCount int) (*Service, error) {
	return NewServiceWithLUNResumer(ctx, db, workerCount, nil)
}

func NewServiceWithLUNResumer(ctx context.Context, db *sql.DB, workerCount int, resumer LUNTargetResumer) (*Service, error) {
	if workerCount <= 0 {
		workerCount = 4
	}
	cfg, err := LoadPlannerConfig(ctx, db)
	if err != nil {
		return nil, err
	}
	half := 86400
	_ = db.QueryRowContext(ctx,
		`SELECT CAST(value AS INTEGER) FROM control_plane_config
		  WHERE key = 'smoothfs_heat_halflife_seconds'`).Scan(&half)

	client, err := Open()
	if err != nil {
		return nil, err
	}

	planChan := make(chan MovementPlan, 64)
	return &Service{
		db:          db,
		client:      client,
		heat:        NewHeatAggregator(db, half),
		planner:     NewPlanner(db, planChan, cfg),
		planChan:    planChan,
		workerCount: workerCount,
		lunResumer:  resumer,
		pools:       make(map[string]*Pool),
	}, nil
}

func (s *Service) SetLUNResumer(resumer LUNTargetResumer) {
	if s == nil {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lunResumer = resumer
}

func (s *Service) newWorker() *Worker {
	if s == nil {
		return nil
	}
	if s.lunResumer == nil {
		return NewWorker(s.db, s.client)
	}
	return NewWorkerWithLUNResumer(s.db, s.client, s.lunResumer)
}

func (s *Service) Close() error {
	if s == nil {
		return nil
	}
	return s.client.Close()
}

func (s *Service) RegisterPool(p *Pool) {
	s.mu.Lock()
	s.pools[p.UUID.String()] = p
	s.mu.Unlock()
	s.planner.RegisterPool(p)
}

// ClientConn returns the netlink client held by the service.
func (s *Service) ClientConn() *Client {
	if s == nil {
		return nil
	}
	return s.client
}

// PoolByUUID returns a registered pool by UUID string.
func (s *Service) PoolByUUID(id string) *Pool {
	if s == nil {
		return nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.pools[id]
}

// DiscoverPool exposes the mount-event discovery logic for tests and
// compatibility wrappers.
func (s *Service) DiscoverPool(ctx context.Context, ev *Event) (*Pool, error) {
	return s.discoverPool(ctx, ev)
}

// DiscoverPoolFromDB exposes the mount-event discovery logic without
// requiring a fully constructed Service. Used by compatibility wrappers
// and tests.
func DiscoverPoolFromDB(ctx context.Context, db *sql.DB, ev *Event) (*Pool, error) {
	s := &Service{db: db}
	return s.discoverPool(ctx, ev)
}

func (s *Service) Run(ctx context.Context) error {
	if err := Recover(ctx, s.db); err != nil {
		log.Printf("smoothfs: recovery failed: %v", err)
	}

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		s.runEvents(ctx)
	}()
	wg.Add(1)
	go func() {
		defer wg.Done()
		s.planner.Run(ctx)
	}()
	wg.Add(1)
	go func() {
		defer wg.Done()
		s.runSubtreeReconcile(ctx)
	}()
	for i := 0; i < s.workerCount; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			s.newWorker().Run(ctx, s.planChan)
		}()
	}

	<-ctx.Done()
	close(s.planChan)
	wg.Wait()
	return nil
}

func (s *Service) runEvents(ctx context.Context) {
	for {
		if ctx.Err() != nil {
			return
		}
		msgs, err := s.client.Receive()
		if err != nil {
			if errors.Is(err, context.Canceled) || ctx.Err() != nil {
				return
			}
			log.Printf("smoothfs: netlink receive: %v", err)
			continue
		}
		for _, m := range msgs {
			ev, err := DecodeEvent(m)
			if err != nil {
				log.Printf("smoothfs: decode event: %v", err)
				continue
			}
			if ev == nil {
				continue
			}
			switch ev.Type {
			case EventHeatSample:
				if err := s.heat.Apply(ctx, ev.HeatSamples); err != nil {
					log.Printf("smoothfs: heat apply: %v", err)
				}
			case EventMountReady:
				if err := s.registerMountEvent(ctx, ev); err != nil {
					log.Printf("smoothfs: mount-ready discovery failed for %s: %v",
						ev.PoolUUID, err)
				}
			case EventMoveState:
				if ev.Move != nil {
					log.Printf("smoothfs: move state %s for object %x seq=%d",
						ev.Move.NewState, ev.Move.OID, ev.Move.TransactionSeq)
				}
			case EventTierFault:
				if ev.Tier != nil {
					log.Printf("smoothfs: tier fault: pool %s tier rank %d",
						ev.PoolUUID, ev.Tier.TierRank)
				}
			case EventSpill:
				s.mu.Lock()
				if pool := s.pools[ev.PoolUUID.String()]; pool != nil {
					pool.AnySpillSinceMount = true
				}
				s.mu.Unlock()
			}
		}
	}
}

func (s *Service) registerMountEvent(ctx context.Context, ev *Event) error {
	if ev == nil {
		return nil
	}
	s.mu.Lock()
	if _, ok := s.pools[ev.PoolUUID.String()]; ok {
		s.mu.Unlock()
		log.Printf("smoothfs: pool %s already registered", ev.PoolUUID)
		return nil
	}
	s.mu.Unlock()

	pool, err := s.discoverPool(ctx, ev)
	if err != nil {
		return err
	}
	s.RegisterPool(pool)
	log.Printf("smoothfs: registered pool %s namespace=%s tiers=%d",
		ev.PoolUUID, pool.NamespaceID, len(pool.Tiers))
	return nil
}

func (s *Service) discoverPool(ctx context.Context, ev *Event) (*Pool, error) {
	ns, err := s.lookupNamespace(ctx, ev)
	if err != nil {
		return nil, err
	}

	rows, err := s.db.QueryContext(ctx, `
		SELECT id, rank, target_fill_pct, full_threshold_pct, backing_ref
		  FROM tier_targets
		 WHERE placement_domain = ?
		   AND backend_kind = 'smoothfs'
		 ORDER BY rank`, ns.PlacementDomain)
	if err != nil {
		return nil, fmt.Errorf("query tier_targets: %w", err)
	}
	defer rows.Close()

	mountedByRank := make(map[int]MountedTier, len(ev.Tiers))
	for _, t := range ev.Tiers {
		mountedByRank[int(t.Rank)] = t
	}

	var tiers []TierInfo
	for rows.Next() {
		var (
			id, backingRef     string
			rank               int
			targetPct, fullPct int
		)
		if err := rows.Scan(&id, &rank, &targetPct, &fullPct, &backingRef); err != nil {
			return nil, fmt.Errorf("scan tier_target: %w", err)
		}
		lowerDir := backingRef
		if lowerDir == "" {
			mounted, ok := mountedByRank[rank]
			if !ok {
				return nil, fmt.Errorf("mount-ready missing rank %d for pool %s", rank, ev.PoolUUID)
			}
			lowerDir = mounted.Path
		}
		tiers = append(tiers, TierInfo{
			Rank:      uint8(rank),
			TargetID:  id,
			LowerDir:  lowerDir,
			TargetPct: targetPct,
			FullPct:   fullPct,
		})
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate tier_targets: %w", err)
	}
	sort.Slice(tiers, func(i, j int) bool { return tiers[i].Rank < tiers[j].Rank })
	if len(tiers) == 0 {
		return nil, fmt.Errorf("no tier_targets found for placement_domain %s", ns.PlacementDomain)
	}

	return &Pool{
		UUID:               ev.PoolUUID,
		Name:               ev.PoolName,
		NamespaceID:        ns.ID,
		Tiers:              tiers,
		AnySpillSinceMount: ev.AnySpillSinceMount,
	}, nil
}

type namespaceRow struct {
	ID, PlacementDomain string
}

func (s *Service) lookupNamespace(ctx context.Context, ev *Event) (*namespaceRow, error) {
	candidates := []string{ev.PoolUUID.String()}
	if ev.PoolName != "" {
		candidates = append(candidates, ev.PoolName)
	}
	for _, candidate := range candidates {
		var ns namespaceRow
		err := s.db.QueryRowContext(ctx, `
			SELECT id, placement_domain
			  FROM managed_namespaces
			 WHERE backend_kind = 'smoothfs'
			   AND (backend_ref = ? OR name = ?)
			 LIMIT 1`, candidate, candidate).Scan(&ns.ID, &ns.PlacementDomain)
		if err == nil {
			return &ns, nil
		}
		if err != sql.ErrNoRows {
			return nil, fmt.Errorf("lookup namespace %q: %w", candidate, err)
		}
	}
	return nil, fmt.Errorf("no smoothfs namespace found for pool %s (%s)", ev.PoolUUID, ev.PoolName)
}
