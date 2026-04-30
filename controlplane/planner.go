package controlplane

import (
	"context"
	"database/sql"
	"encoding/hex"
	"fmt"
	"sort"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/google/uuid"
)

// Pool is a runtime registration of a smoothfs mount that the planner
// considers for movement.
type Pool struct {
	UUID               uuid.UUID
	Name               string
	NamespaceID        string
	Tiers              []TierInfo
	AnySpillSinceMount bool
	transaction        uint64
}

// TierInfo describes one lower-tier in a Pool.
type TierInfo struct {
	Rank      uint8
	TargetID  string
	LowerDir  string
	FillPct   int
	TargetPct int
	FullPct   int
}

func (p *Pool) NextSeq() uint64 {
	return atomic.AddUint64(&p.transaction, 1)
}

// Planner reads heat from smoothfs_objects and emits MovementPlan items.
type Planner struct {
	db    *sql.DB
	plans chan<- MovementPlan
	mu    sync.RWMutex
	pools map[uuid.UUID]*Pool
	cfg   PlannerConfig
}

type PlannerConfig struct {
	MinResidencySec   int
	CooldownSec       int
	HysteresisPct     int
	PromotePercentile int
	DemotePercentile  int
	IntervalSec       int
}

type candidate struct {
	oid         string
	currentTier string
	relPath     string
	ewma        float64
	lastMove    string
	pinState    string
}

var tierFillPct = func(path string) (int, error) {
	var st syscall.Statfs_t
	var used, total uint64

	if err := syscall.Statfs(path, &st); err != nil {
		return 0, err
	}
	total = st.Blocks * uint64(st.Bsize)
	if total == 0 {
		return 0, nil
	}
	used = (st.Blocks - st.Bavail) * uint64(st.Bsize)
	return int((used * 100) / total), nil
}

func LoadPlannerConfig(ctx context.Context, db *sql.DB) (PlannerConfig, error) {
	defaults := PlannerConfig{
		MinResidencySec:   3600,
		CooldownSec:       21600,
		HysteresisPct:     20,
		PromotePercentile: 80,
		DemotePercentile:  20,
		IntervalSec:       900,
	}
	keys := map[string]*int{
		"smoothfs_min_residency_seconds":     &defaults.MinResidencySec,
		"smoothfs_movement_cooldown_seconds": &defaults.CooldownSec,
		"smoothfs_hysteresis_pct":            &defaults.HysteresisPct,
		"smoothfs_promote_percentile":        &defaults.PromotePercentile,
		"smoothfs_demote_percentile":         &defaults.DemotePercentile,
		"smoothfs_planner_interval_seconds":  &defaults.IntervalSec,
	}
	for k, dst := range keys {
		var s sql.NullString
		if err := db.QueryRowContext(ctx,
			`SELECT value FROM control_plane_config WHERE key = ?`, k).Scan(&s); err != nil && err != sql.ErrNoRows {
			return defaults, err
		} else if s.Valid {
			var v int
			if _, err := fmt.Sscanf(s.String, "%d", &v); err == nil && v > 0 {
				*dst = v
			}
		}
	}
	return defaults, nil
}

func NewPlanner(db *sql.DB, plans chan<- MovementPlan, cfg PlannerConfig) *Planner {
	return &Planner{
		db:    db,
		plans: plans,
		pools: make(map[uuid.UUID]*Pool),
		cfg:   cfg,
	}
}

func (p *Planner) RegisterPool(pool *Pool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.pools[pool.UUID] = pool
}

func (p *Planner) Run(ctx context.Context) {
	interval := time.Duration(p.cfg.IntervalSec) * time.Second
	if interval <= 0 {
		interval = 15 * time.Minute
	}
	t := time.NewTicker(interval)
	defer t.Stop()

	for {
		if err := p.tick(ctx); err != nil && ctx.Err() == nil {
			// Best-effort planner loop; errors are retried next tick.
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

func (p *Planner) tick(ctx context.Context) error {
	p.mu.RLock()
	pools := make([]*Pool, 0, len(p.pools))
	for _, pool := range p.pools {
		pools = append(pools, pool)
	}
	p.mu.RUnlock()

	for _, pool := range pools {
		if err := p.planPool(ctx, pool); err != nil {
			return err
		}
	}
	return nil
}

func (p *Planner) planPool(ctx context.Context, pool *Pool) error {
	if len(pool.Tiers) < 2 {
		return nil
	}

	poolTiers := make([]TierInfo, len(pool.Tiers))
	copy(poolTiers, pool.Tiers)
	for i := range poolTiers {
		fillPct, err := tierFillPct(poolTiers[i].LowerDir)
		if err == nil {
			poolTiers[i].FillPct = fillPct
		}
	}

	rows, err := p.db.QueryContext(ctx, `
		SELECT object_id, current_tier_id, rel_path, ewma_value,
		       COALESCE(last_movement_at, created_at),
		       pin_state
		  FROM smoothfs_objects
		 WHERE namespace_id = ?
		   AND movement_state = 'placed'`, pool.NamespaceID)
	if err != nil {
		return err
	}
	defer rows.Close()

	var objs []candidate
	for rows.Next() {
		var c candidate
		if err := rows.Scan(&c.oid, &c.currentTier, &c.relPath, &c.ewma, &c.lastMove, &c.pinState); err != nil {
			return err
		}
		objs = append(objs, c)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	if len(objs) == 0 {
		return nil
	}

	sort.Slice(objs, func(i, j int) bool { return objs[i].ewma < objs[j].ewma })
	promoteIdx := percentileIndex(len(objs), p.cfg.PromotePercentile)
	demoteIdx := percentileIndex(len(objs), p.cfg.DemotePercentile)
	promoteGate, demoteGate := movementGates(objs, promoteIdx, demoteIdx, p.cfg.HysteresisPct)

	tiersByID := make(map[string]TierInfo, len(pool.Tiers))
	for _, t := range poolTiers {
		tiersByID[t.TargetID] = t
	}

	now := time.Now()
	for _, c := range objs {
		cur, ok := tiersByID[c.currentTier]
		if !ok {
			continue
		}
		if c.pinState != "" && c.pinState != "none" {
			continue
		}

		var dest *TierInfo
		sourceOverfull := cur.FullPct > 0 && cur.FillPct >= cur.FullPct
		switch {
		case c.ewma >= promoteGate && int(cur.Rank) > 0:
			if movedRecently(now, c.lastMove, p.cfg.MinResidencySec, p.cfg.CooldownSec) {
				continue
			}
			t := poolTiers[int(cur.Rank)-1]
			dest = &t
		case (c.ewma <= demoteGate || sourceOverfull) && int(cur.Rank) < len(poolTiers)-1:
			if !sourceOverfull &&
				movedRecently(now, c.lastMove, p.cfg.MinResidencySec, p.cfg.CooldownSec) {
				continue
			}
			t := poolTiers[int(cur.Rank)+1]
			dest = &t
		}
		if dest == nil {
			continue
		}
		if dest.FullPct > 0 && dest.FillPct >= dest.FullPct {
			continue
		}

		plan, err := buildMovementPlan(pool, cur, *dest, c)
		if err != nil {
			continue
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case p.plans <- plan:
		}
	}
	return nil
}

func movementGates(objs []candidate, promoteIdx, demoteIdx, hysteresisPct int) (float64, float64) {
	promoteCut := objs[promoteIdx].ewma
	demoteCut := objs[demoteIdx].ewma
	if hysteresisPct <= 0 {
		return promoteCut, demoteCut
	}
	if promoteIdx == len(objs)-1 && promoteIdx > 0 {
		promoteCut = objs[promoteIdx-1].ewma
	}
	if demoteIdx == 0 && len(objs) > 1 {
		demoteCut = objs[1].ewma
	}
	hysteresis := float64(hysteresisPct) / 100
	promoteGate := promoteCut * (1 + hysteresis)
	demoteGate := demoteCut * (1 - hysteresis)
	if demoteGate < 0 {
		demoteGate = 0
	}
	return promoteGate, demoteGate
}

func buildMovementPlan(pool *Pool, cur, dest TierInfo, c candidate) (MovementPlan, error) {
	var oid [OIDLen]byte
	raw, err := hex.DecodeString(c.oid)
	if err != nil || len(raw) != OIDLen {
		return MovementPlan{}, fmt.Errorf("decode oid %q: %w", c.oid, err)
	}
	copy(oid[:], raw)
	return MovementPlan{
		PoolUUID:       pool.UUID,
		ObjectID:       oid,
		NamespaceID:    pool.NamespaceID,
		SourceTierID:   cur.TargetID,
		SourceTierRank: cur.Rank,
		SourceLowerDir: cur.LowerDir,
		DestTierID:     dest.TargetID,
		DestTierRank:   dest.Rank,
		DestLowerDir:   dest.LowerDir,
		RelPath:        c.relPath,
		TransactionSeq: pool.NextSeq(),
	}, nil
}

func movedRecently(now time.Time, lastMove string, minResidencySec, cooldownSec int) bool {
	t, err := time.Parse(time.RFC3339Nano, lastMove)
	if err != nil {
		t, err = time.Parse("2006-01-02 15:04:05", lastMove)
		if err != nil {
			return false
		}
	}
	age := now.Sub(t)
	return age < time.Duration(minResidencySec)*time.Second ||
		age < time.Duration(cooldownSec)*time.Second
}

func percentileIndex(n, pct int) int {
	if n <= 1 {
		return 0
	}
	if pct <= 0 {
		return 0
	}
	if pct >= 100 {
		return n - 1
	}
	idx := (n * pct) / 100
	if idx >= n {
		idx = n - 1
	}
	return idx
}
