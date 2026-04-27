package controlplane

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/google/uuid"
)

var errSourceRaced = errors.New("source mtime changed during copy; cutover aborted")

// ErrLUNQuiesceRequired marks the Phase 8 safety gate: a LUN backing file
// must be quiesced and unpinned before the generic movement worker may plan it.
var ErrLUNQuiesceRequired = errors.New("lun backing file requires target quiesce before movement")

// ErrLUNResumeRequired marks a prepared LUN movement whose target lifecycle
// cannot be completed because no resumer was wired into the worker.
var ErrLUNResumeRequired = errors.New("lun target resume hook required after movement")

// MovementPlan is one queued promote/demote, dispatched by the planner
// and consumed by a Worker.
type MovementPlan struct {
	PoolUUID       uuid.UUID
	ObjectID       [OIDLen]byte
	NamespaceID    string
	SourceTierID   string
	SourceTierRank uint8
	SourceLowerDir string
	DestTierID     string
	DestTierRank   uint8
	DestLowerDir   string
	RelPath        string
	TransactionSeq uint64
	RePinLUN       bool
	LUNTargetID    string
}

type Worker struct {
	db              *sql.DB
	client          movementClient
	setLUNPin       func(string) error
	resumeLUNTarget func(context.Context, string) error
}

type movementClient interface {
	Inspect(poolUUID uuid.UUID, oid [OIDLen]byte) (*InspectResult, error)
	MovePlan(poolUUID uuid.UUID, oid [OIDLen]byte, destTier uint8, seq uint64) error
	MoveCutover(poolUUID uuid.UUID, oid [OIDLen]byte, seq uint64) error
}

type LUNTargetResumer interface {
	Resume(ctx context.Context, targetID string) error
}

func NewWorker(db *sql.DB, client movementClient) *Worker {
	return &Worker{db: db, client: client, setLUNPin: setLUNPin}
}

func NewWorkerWithLUNResumer(db *sql.DB, client movementClient, resumer LUNTargetResumer) *Worker {
	w := NewWorker(db, client)
	if resumer != nil {
		w.resumeLUNTarget = resumer.Resume
	}
	return w
}

// Execute runs one movement plan. Exported so the legacy in-tree
// compatibility package can delegate to the extracted implementation.
func (w *Worker) Execute(ctx context.Context, p MovementPlan) error {
	return w.execute(ctx, p)
}

func (w *Worker) Run(ctx context.Context, plans <-chan MovementPlan) {
	for {
		select {
		case <-ctx.Done():
			return
		case p, ok := <-plans:
			if !ok {
				return
			}
			if err := w.execute(ctx, p); err != nil {
				w.logTransition(ctx, p, "", string(StateFailed), err.Error())
			}
		}
	}
}

func (w *Worker) execute(ctx context.Context, p MovementPlan) error {
	oid := hex.EncodeToString(p.ObjectID[:])
	ins, err := w.client.Inspect(p.PoolUUID, p.ObjectID)
	if err != nil {
		return fmt.Errorf("inspect before movement: %w", err)
	}
	if ins == nil {
		return fmt.Errorf("inspect before movement returned nil for object %s", oid)
	}
	if ins.PinState == PinLUN {
		return fmt.Errorf("%w: object %s", ErrLUNQuiesceRequired, oid)
	}
	if p.RelPath == "" || p.RelPath == oid {
		if ins.RelPath == "" {
			return fmt.Errorf("kernel returned empty rel_path for object %s", oid)
		}
		p.RelPath = ins.RelPath
		_, _ = w.db.ExecContext(ctx,
			`UPDATE smoothfs_objects SET rel_path = ?, updated_at = datetime('now')
			  WHERE object_id = ?`, p.RelPath, oid)
	}
	srcPath := filepath.Join(p.SourceLowerDir, p.RelPath)
	dstPath := filepath.Join(p.DestLowerDir, p.RelPath)

	if err := w.client.MovePlan(p.PoolUUID, p.ObjectID, p.DestTierRank, p.TransactionSeq); err != nil {
		return fmt.Errorf("move_plan: %w", err)
	}
	w.logTransition(ctx, p, "", string(StatePlanAccepted), "")
	w.persistObject(ctx, oid, p, StatePlanAccepted)

	if err := os.MkdirAll(filepath.Dir(dstPath), 0o755); err != nil {
		return w.abort(ctx, p, fmt.Errorf("mkdir dest parent: %w", err))
	}
	w.logTransition(ctx, p, string(StatePlanAccepted), string(StateDestinationReserved), "")
	w.persistObject(ctx, oid, p, StateDestinationReserved)

	srcStatBefore, err := os.Stat(srcPath)
	if err != nil {
		os.Remove(dstPath)
		return w.abort(ctx, p, fmt.Errorf("pre-copy stat: %w", err))
	}

	srcSum, err := w.copyWithChecksum(srcPath, dstPath)
	if err != nil {
		os.Remove(dstPath)
		return w.abort(ctx, p, fmt.Errorf("copy: %w", err))
	}
	w.logTransition(ctx, p, string(StateDestinationReserved), string(StateCopyInProgress), "")
	w.persistObject(ctx, oid, p, StateCopyInProgress)

	dstSum, err := fileSHA256(dstPath)
	if err != nil {
		os.Remove(dstPath)
		return w.abort(ctx, p, fmt.Errorf("verify dst: %w", err))
	}
	if dstSum != srcSum {
		os.Remove(dstPath)
		return w.abort(ctx, p, errors.New("checksum mismatch"))
	}

	srcStatAfter, err := os.Stat(srcPath)
	if err != nil {
		os.Remove(dstPath)
		return w.abort(ctx, p, fmt.Errorf("post-copy stat: %w", err))
	}
	if !srcStatAfter.ModTime().Equal(srcStatBefore.ModTime()) ||
		srcStatAfter.Size() != srcStatBefore.Size() {
		os.Remove(dstPath)
		return w.abort(ctx, p, errSourceRaced)
	}
	w.logTransition(ctx, p, string(StateCopyInProgress), string(StateCopyVerified), "")
	w.persistObject(ctx, oid, p, StateCopyVerified)

	if err := w.client.MoveCutover(p.PoolUUID, p.ObjectID, p.TransactionSeq); err != nil {
		os.Remove(dstPath)
		return w.abort(ctx, p, fmt.Errorf("move_cutover: %w", err))
	}
	w.logTransition(ctx, p, string(StateCopyVerified), string(StateSwitched), "")
	w.persistObject(ctx, oid, p, StateSwitched)
	if p.RePinLUN {
		if err := w.setLUNPin(dstPath); err != nil {
			return w.failAfterSwitch(ctx, p, fmt.Errorf("repin lun: %w", err))
		}
		w.persistLUNPin(ctx, oid)
		if p.LUNTargetID != "" {
			if w.resumeLUNTarget == nil {
				return w.failAfterSwitch(ctx, p, fmt.Errorf("%w: target %s", ErrLUNResumeRequired, p.LUNTargetID))
			}
			if err := w.resumeLUNTarget(ctx, p.LUNTargetID); err != nil {
				return w.failAfterSwitch(ctx, p, fmt.Errorf("resume lun target %s: %w", p.LUNTargetID, err))
			}
		}
	}

	if err := os.Remove(srcPath); err != nil && !os.IsNotExist(err) {
		w.logTransition(ctx, p, string(StateSwitched), string(StateCleanupInProgress), err.Error())
	}
	w.logTransition(ctx, p, string(StateSwitched), string(StateCleanupComplete), "")
	w.finalize(ctx, oid, p)
	return nil
}

func (w *Worker) abort(ctx context.Context, p MovementPlan, cause error) error {
	w.logTransition(ctx, p, "", string(StateFailed), cause.Error())
	oid := hex.EncodeToString(p.ObjectID[:])
	_, _ = w.db.ExecContext(ctx,
		`UPDATE smoothfs_objects
		    SET movement_state = 'failed',
		        intended_tier_id = NULL,
		        failure_reason = ?,
		        last_movement_at = datetime('now'),
		        updated_at = datetime('now')
		  WHERE object_id = ?`,
		cause.Error(), oid)
	return cause
}

func (w *Worker) failAfterSwitch(ctx context.Context, p MovementPlan, cause error) error {
	w.logTransition(ctx, p, string(StateSwitched), string(StateFailed), cause.Error())
	oid := hex.EncodeToString(p.ObjectID[:])
	_, _ = w.db.ExecContext(ctx,
		`UPDATE smoothfs_objects
		    SET current_tier_id = ?,
		        intended_tier_id = NULL,
		        movement_state = 'failed',
		        failure_reason = ?,
		        last_movement_at = datetime('now'),
		        updated_at = datetime('now')
		  WHERE object_id = ?`,
		p.DestTierID, cause.Error(), oid)
	return cause
}

func (w *Worker) logTransition(ctx context.Context, p MovementPlan, from, to, reason string) {
	oid := hex.EncodeToString(p.ObjectID[:])
	payload := "{}"
	if reason != "" {
		payload = fmt.Sprintf(`{"reason":%q}`, reason)
	}
	_, _ = w.db.ExecContext(ctx,
		`INSERT INTO smoothfs_movement_log
		   (object_id, transaction_seq, from_state, to_state, source_tier, dest_tier, payload_json)
		 VALUES (?, ?, NULLIF(?, ''), ?, ?, ?, ?)`,
		oid, p.TransactionSeq, from, to, p.SourceTierID, p.DestTierID, payload)
}

func (w *Worker) persistObject(ctx context.Context, oid string, p MovementPlan, state MovementState) {
	_, _ = w.db.ExecContext(ctx,
		`INSERT INTO smoothfs_objects (object_id, namespace_id, current_tier_id, intended_tier_id,
		                                 movement_state, transaction_seq, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?, datetime('now'))
		 ON CONFLICT(object_id) DO UPDATE SET
		   intended_tier_id = excluded.intended_tier_id,
		   movement_state   = excluded.movement_state,
		   transaction_seq  = excluded.transaction_seq,
		   updated_at       = datetime('now')`,
		oid, p.NamespaceID, p.SourceTierID, p.DestTierID, string(state), p.TransactionSeq)
}

func (w *Worker) persistLUNPin(ctx context.Context, oid string) {
	_, _ = w.db.ExecContext(ctx,
		`UPDATE smoothfs_objects
		    SET pin_state = 'pin_lun',
		        updated_at = datetime('now')
		  WHERE object_id = ?`,
		oid)
}

func (w *Worker) finalize(ctx context.Context, oid string, p MovementPlan) {
	_, _ = w.db.ExecContext(ctx,
		`UPDATE smoothfs_objects
		    SET current_tier_id   = ?,
		        intended_tier_id  = NULL,
		        movement_state    = 'placed',
		        last_movement_at  = datetime('now'),
		        last_committed_cutover_gen = last_committed_cutover_gen + 1,
		        failure_reason    = '',
		        updated_at        = datetime('now')
		  WHERE object_id = ?`,
		p.DestTierID, oid)
}

func (w *Worker) copyWithChecksum(srcPath, dstPath string) ([32]byte, error) {
	var zero [32]byte
	src, err := os.Open(srcPath)
	if err != nil {
		return zero, err
	}
	defer src.Close()

	if err := os.MkdirAll(filepath.Dir(dstPath), 0o755); err != nil {
		return zero, err
	}
	dst, err := os.OpenFile(dstPath, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0o644)
	if err != nil {
		return zero, err
	}
	defer dst.Close()

	h := sha256.New()
	if _, err := io.Copy(io.MultiWriter(dst, h), src); err != nil {
		return zero, err
	}
	if err := dst.Sync(); err != nil {
		return zero, err
	}
	copy(zero[:], h.Sum(nil))
	return zero, nil
}

func fileSHA256(path string) ([32]byte, error) {
	var out [32]byte
	f, err := os.Open(path)
	if err != nil {
		return out, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return out, err
	}
	copy(out[:], h.Sum(nil))
	return out, nil
}
