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

// ErrLUNDestinationStale marks a prepared LUN movement whose post-cutover
// kernel view does not match the destination recorded in the movement plan.
var ErrLUNDestinationStale = errors.New("lun destination placement is stale after cutover")

// ErrLUNPinNotVisible marks a prepared LUN movement whose destination re-pin
// did not become visible in the kernel before target resume.
var ErrLUNPinNotVisible = errors.New("lun destination pin is not visible after re-pin")

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
	if p.RePinLUN {
		if err := w.requireLUNMoveRecord(ctx, p, oid); err != nil {
			return err
		}
		if ins.PinState != PinNone {
			return fmt.Errorf("quiesced lun has incompatible pin state %q", ins.PinState)
		}
		if ins.CurrentTier != p.SourceTierRank {
			return fmt.Errorf("%w: kernel source tier rank %d plan source tier rank %d",
				ErrLUNPlacementStale, ins.CurrentTier, p.SourceTierRank)
		}
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
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("move_plan: %w", err))
	}
	w.logTransition(ctx, p, "", string(StatePlanAccepted), "")
	w.persistObject(ctx, oid, p, StatePlanAccepted)

	if err := os.MkdirAll(filepath.Dir(dstPath), 0o755); err != nil {
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("mkdir dest parent: %w", err))
	}
	w.logTransition(ctx, p, string(StatePlanAccepted), string(StateDestinationReserved), "")
	w.persistObject(ctx, oid, p, StateDestinationReserved)

	srcStatBefore, err := os.Stat(srcPath)
	if err != nil {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("pre-copy stat: %w", err))
	}

	srcSum, err := w.copyWithChecksum(srcPath, dstPath)
	if err != nil {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("copy: %w", err))
	}
	w.logTransition(ctx, p, string(StateDestinationReserved), string(StateCopyInProgress), "")
	w.persistObject(ctx, oid, p, StateCopyInProgress)

	dstSum, err := fileSHA256(dstPath)
	if err != nil {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("verify dst: %w", err))
	}
	if dstSum != srcSum {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, errors.New("checksum mismatch"))
	}

	srcStatAfter, err := os.Stat(srcPath)
	if err != nil {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("post-copy stat: %w", err))
	}
	if !srcStatAfter.ModTime().Equal(srcStatBefore.ModTime()) ||
		srcStatAfter.Size() != srcStatBefore.Size() {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, errSourceRaced)
	}
	w.logTransition(ctx, p, string(StateCopyInProgress), string(StateCopyVerified), "")
	w.persistObject(ctx, oid, p, StateCopyVerified)

	if err := w.client.MoveCutover(p.PoolUUID, p.ObjectID, p.TransactionSeq); err != nil {
		os.Remove(dstPath)
		return w.rollbackLUNBeforeSwitch(ctx, p, srcPath, oid, fmt.Errorf("move_cutover: %w", err))
	}
	w.logTransition(ctx, p, string(StateCopyVerified), string(StateSwitched), "")
	w.persistObject(ctx, oid, p, StateSwitched)
	if p.RePinLUN {
		if err := w.verifyLUNDestination(p, oid); err != nil {
			return w.failAfterSwitch(ctx, p, err)
		}
		if err := w.setLUNPin(dstPath); err != nil {
			return w.failAfterSwitch(ctx, p, fmt.Errorf("repin lun: %w", err))
		}
		if err := w.verifyLUNRePin(p, oid); err != nil {
			return w.failAfterSwitch(ctx, p, err)
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

func (w *Worker) rollbackLUNBeforeSwitch(ctx context.Context, p MovementPlan, srcPath, oid string, cause error) error {
	if !p.RePinLUN {
		return w.abort(ctx, p, cause)
	}
	var errs []error
	repinned := false
	if err := w.setLUNPin(srcPath); err != nil {
		errs = append(errs, fmt.Errorf("repin source lun: %w", err))
	} else {
		repinned = true
		w.persistLUNPin(ctx, oid)
	}
	if p.LUNTargetID != "" && repinned {
		if w.resumeLUNTarget == nil {
			errs = append(errs, fmt.Errorf("%w: target %s", ErrLUNResumeRequired, p.LUNTargetID))
		} else if err := w.resumeLUNTarget(ctx, p.LUNTargetID); err != nil {
			errs = append(errs, fmt.Errorf("resume lun target %s: %w", p.LUNTargetID, err))
		}
	}
	if len(errs) > 0 {
		return w.abort(ctx, p, errors.Join(append([]error{cause}, errs...)...))
	}
	return w.abort(ctx, p, cause)
}

func (w *Worker) requireLUNMoveRecord(ctx context.Context, p MovementPlan, oid string) error {
	var namespaceID, currentTierID, pinState, movementState string
	err := w.db.QueryRowContext(ctx, `
		SELECT namespace_id, current_tier_id, pin_state, movement_state
		  FROM smoothfs_objects
		 WHERE object_id = ?`, oid).Scan(&namespaceID, &currentTierID, &pinState, &movementState)
	if errors.Is(err, sql.ErrNoRows) {
		return fmt.Errorf("%w: object %s", ErrLUNRecordRequired, oid)
	}
	if err != nil {
		return fmt.Errorf("read lun movement record: %w", err)
	}
	if namespaceID != p.NamespaceID {
		return fmt.Errorf("%w: object %s namespace=%q plan_namespace=%q",
			ErrLUNRecordRequired, oid, namespaceID, p.NamespaceID)
	}
	if pinState != string(PinLUN) {
		return fmt.Errorf("%w: object %s pin_state=%q", ErrLUNRecordRequired, oid, pinState)
	}
	if movementState != string(StatePlaced) {
		return fmt.Errorf("%w: object %s movement_state=%q", ErrLUNRecordRequired, oid, movementState)
	}
	if currentTierID != p.SourceTierID {
		return fmt.Errorf("%w: db source tier %q plan source tier %q",
			ErrLUNPlacementStale, currentTierID, p.SourceTierID)
	}
	return nil
}

func (w *Worker) verifyLUNDestination(p MovementPlan, oid string) error {
	ins, err := w.client.Inspect(p.PoolUUID, p.ObjectID)
	if err != nil {
		return fmt.Errorf("inspect after lun cutover: %w", err)
	}
	if ins == nil {
		return fmt.Errorf("inspect after lun cutover returned nil for object %s", oid)
	}
	if ins.CurrentTier != p.DestTierRank {
		return fmt.Errorf("%w: kernel tier rank %d dest tier rank %d",
			ErrLUNDestinationStale, ins.CurrentTier, p.DestTierRank)
	}
	if ins.RelPath != "" && p.RelPath != "" && ins.RelPath != p.RelPath {
		return fmt.Errorf("%w: kernel rel_path %q plan rel_path %q",
			ErrLUNDestinationStale, ins.RelPath, p.RelPath)
	}
	return nil
}

func (w *Worker) verifyLUNRePin(p MovementPlan, oid string) error {
	ins, err := w.client.Inspect(p.PoolUUID, p.ObjectID)
	if err != nil {
		return fmt.Errorf("inspect after lun re-pin: %w", err)
	}
	if ins == nil {
		return fmt.Errorf("inspect after lun re-pin returned nil for object %s", oid)
	}
	if ins.CurrentTier != p.DestTierRank {
		return fmt.Errorf("%w: kernel tier rank %d dest tier rank %d",
			ErrLUNDestinationStale, ins.CurrentTier, p.DestTierRank)
	}
	if ins.RelPath != "" && p.RelPath != "" && ins.RelPath != p.RelPath {
		return fmt.Errorf("%w: kernel rel_path %q plan rel_path %q",
			ErrLUNDestinationStale, ins.RelPath, p.RelPath)
	}
	if ins.PinState != PinLUN {
		return fmt.Errorf("%w: kernel pin_state=%q", ErrLUNPinNotVisible, ins.PinState)
	}
	return nil
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
