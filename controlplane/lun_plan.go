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

// LUNTargetQuiescer is implemented by the caller that owns the storage target
// lifecycle. StopAndDrain must return only after the LUN path is no longer live.
type LUNTargetQuiescer interface {
	StopAndDrain(ctx context.Context, targetID string) error
	Resume(ctx context.Context, targetID string) error
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

// PrepareQuiescedLUNMovementPlan clears the LUN pin xattr for an already
// stopped or drained target and then builds the opt-in movement plan. If plan
// construction fails after the pin was cleared, it attempts to re-pin the
// backing file before returning the planning error.
func PrepareQuiescedLUNMovementPlan(
	ctx context.Context,
	db *sql.DB,
	client ObjectInspector,
	pool *Pool,
	oid [OIDLen]byte,
	backingPath string,
	destTierID string,
) (MovementPlan, error) {
	if err := ClearLUNPin(backingPath); err != nil {
		return MovementPlan{}, err
	}
	plan, err := BuildQuiescedLUNMovementPlan(ctx, db, client, pool, oid, destTierID)
	if err != nil {
		if pinErr := setLUNPin(backingPath); pinErr != nil {
			return MovementPlan{}, errors.Join(err, pinErr)
		}
		return MovementPlan{}, err
	}
	return plan, nil
}

// PrepareStoppedLUNMovementPlan stops or drains the target before clearing the
// LUN pin and preparing the movement plan. If preparation fails, the target is
// resumed after the fail-closed re-pin path has run.
func PrepareStoppedLUNMovementPlan(
	ctx context.Context,
	db *sql.DB,
	client ObjectInspector,
	quiescer LUNTargetQuiescer,
	pool *Pool,
	oid [OIDLen]byte,
	targetID string,
	backingPath string,
	destTierID string,
) (MovementPlan, error) {
	if err := quiescer.StopAndDrain(ctx, targetID); err != nil {
		return MovementPlan{}, fmt.Errorf("stop and drain lun target %s: %w", targetID, err)
	}
	plan, err := PrepareQuiescedLUNMovementPlan(ctx, db, client, pool, oid, backingPath, destTierID)
	if err != nil {
		if resumeErr := quiescer.Resume(ctx, targetID); resumeErr != nil {
			return MovementPlan{}, errors.Join(err, resumeErr)
		}
		return MovementPlan{}, err
	}
	plan.LUNTargetID = targetID
	return plan, nil
}
