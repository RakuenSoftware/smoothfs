package controlplane

import (
	"context"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"

	"github.com/google/uuid"
)

var (
	ErrLUNRecordRequired  = errors.New("tracked LUN record required for active LUN movement")
	ErrDestinationTierBad = errors.New("destination tier is not valid for active LUN movement")
)

// ObjectInspector is the narrow kernel query surface needed to prove a LUN
// backing file has been administratively quiesced before planning movement.
type ObjectInspector interface {
	Inspect(poolUUID uuid.UUID, oid [OIDLen]byte) (*InspectResult, error)
}

// BuildQuiescedLUNMovementPlan creates an opt-in movement plan for the narrow
// Phase 8 path: the kernel pin has already been cleared by administrative
// quiesce, while the DB row still records the object as a LUN so the worker
// knows it must re-pin the destination before completion.
func BuildQuiescedLUNMovementPlan(
	ctx context.Context,
	db *sql.DB,
	client ObjectInspector,
	pool *Pool,
	oid [OIDLen]byte,
	destTierID string,
) (MovementPlan, error) {
	if pool == nil {
		return MovementPlan{}, ErrDestinationTierBad
	}
	ins, err := client.Inspect(pool.UUID, oid)
	if err != nil {
		return MovementPlan{}, fmt.Errorf("inspect quiesced lun: %w", err)
	}
	if ins == nil {
		return MovementPlan{}, fmt.Errorf("inspect quiesced lun returned nil for object %s", hex.EncodeToString(oid[:]))
	}
	if ins.PinState == PinLUN {
		return MovementPlan{}, fmt.Errorf("%w: object %s", ErrLUNQuiesceRequired, hex.EncodeToString(oid[:]))
	}
	if ins.PinState != PinNone {
		return MovementPlan{}, fmt.Errorf("quiesced lun has incompatible pin state %q", ins.PinState)
	}

	oidText := hex.EncodeToString(oid[:])
	var row struct {
		namespaceID string
		currentTier string
		state       string
		relPath     string
		pinState    string
	}
	if err := db.QueryRowContext(ctx, `
		SELECT namespace_id, current_tier_id, movement_state, rel_path, pin_state
		  FROM smoothfs_objects
		 WHERE object_id = ?`, oidText).Scan(
		&row.namespaceID, &row.currentTier, &row.state, &row.relPath, &row.pinState); err != nil {
		if errors.Is(err, sql.ErrNoRows) {
			return MovementPlan{}, ErrLUNRecordRequired
		}
		return MovementPlan{}, fmt.Errorf("load lun object %s: %w", oidText, err)
	}
	if row.pinState != string(PinLUN) {
		return MovementPlan{}, fmt.Errorf("%w: object %s pin_state=%q", ErrLUNRecordRequired, oidText, row.pinState)
	}
	if row.namespaceID != pool.NamespaceID {
		return MovementPlan{}, fmt.Errorf("%w: object %s namespace=%q pool_namespace=%q",
			ErrLUNRecordRequired, oidText, row.namespaceID, pool.NamespaceID)
	}
	if row.state != string(StatePlaced) {
		return MovementPlan{}, fmt.Errorf("quiesced lun object %s movement_state=%q", oidText, row.state)
	}
	if row.relPath == "" {
		row.relPath = ins.RelPath
	}
	if row.relPath == "" {
		return MovementPlan{}, fmt.Errorf("quiesced lun object %s has no rel_path", oidText)
	}

	tiersByID := make(map[string]TierInfo, len(pool.Tiers))
	for _, t := range pool.Tiers {
		tiersByID[t.TargetID] = t
	}
	cur, ok := tiersByID[row.currentTier]
	if !ok {
		return MovementPlan{}, fmt.Errorf("current tier %q is not registered for pool %s", row.currentTier, pool.UUID)
	}
	dest, ok := tiersByID[destTierID]
	if !ok || dest.TargetID == cur.TargetID {
		return MovementPlan{}, ErrDestinationTierBad
	}

	return MovementPlan{
		PoolUUID:       pool.UUID,
		ObjectID:       oid,
		NamespaceID:    row.namespaceID,
		SourceTierID:   cur.TargetID,
		SourceTierRank: cur.Rank,
		SourceLowerDir: cur.LowerDir,
		DestTierID:     dest.TargetID,
		DestTierRank:   dest.Rank,
		DestLowerDir:   dest.LowerDir,
		RelPath:        row.relPath,
		TransactionSeq: pool.NextSeq(),
		RePinLUN:       true,
	}, nil
}
