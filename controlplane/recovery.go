package controlplane

import (
	"context"
	"database/sql"
	"fmt"
	"path/filepath"
)

// Recover scans smoothfs_objects for interrupted movement states and failed
// LUN resume rows that need destination placement reconciliation.
func Recover(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx, `
		SELECT object_id, current_tier_id, intended_tier_id, movement_state, transaction_seq, pin_state,
		       namespace_id, rel_path
		  FROM smoothfs_objects
		 WHERE movement_state NOT IN ('placed','cleanup_complete','stale')`)
	if err != nil {
		return fmt.Errorf("scan in-flight movements: %w", err)
	}
	defer rows.Close()

	type pending struct {
		oid, src, dst, state, pinState, namespaceID, relPath string
		seq                                                  uint64
	}
	var todo []pending
	for rows.Next() {
		var p pending
		var dst sql.NullString
		if err := rows.Scan(&p.oid, &p.src, &dst, &p.state, &p.seq, &p.pinState, &p.namespaceID, &p.relPath); err != nil {
			return err
		}
		if dst.Valid {
			p.dst = dst.String
		}
		todo = append(todo, p)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	for _, p := range todo {
		finalTierID := p.src
		switch MovementState(p.state) {
		case StateFailed:
			if p.pinState == string(PinLUN) && p.dst != "" {
				finalTierID = p.dst
				if _, err := db.ExecContext(ctx, `
					UPDATE smoothfs_objects
					   SET current_tier_id  = ?,
					       intended_tier_id = NULL,
					       updated_at       = datetime('now')
					 WHERE object_id = ?`, p.dst, p.oid); err != nil {
					return fmt.Errorf("preserve failed lun %s: %w", p.oid, err)
				}
			}
		case StatePlanAccepted, StateDestinationReserved,
			StateCopyInProgress, StateCopyComplete, StateCopyVerified:
			if _, err := db.ExecContext(ctx, `
					UPDATE smoothfs_objects
				   SET movement_state   = 'placed',
				       intended_tier_id = NULL,
				       failure_reason   = 'recovered: rolled back from ' || ?,
				       updated_at       = datetime('now')
				 WHERE object_id = ?`, p.state, p.oid); err != nil {
				return fmt.Errorf("rollback %s: %w", p.oid, err)
			}
		case StateCutoverInProgress, StateSwitched, StateCleanupInProgress:
			if p.dst == "" {
				continue
			}
			finalTierID = p.dst
			if _, err := db.ExecContext(ctx, `
				UPDATE smoothfs_objects
				   SET current_tier_id   = ?,
				       intended_tier_id  = NULL,
				       movement_state    = 'placed',
				       last_movement_at  = datetime('now'),
				       last_committed_cutover_gen = last_committed_cutover_gen + 1,
				       failure_reason    = 'recovered: forwarded from ' || ?,
				       updated_at        = datetime('now')
				 WHERE object_id = ?`, p.dst, p.state, p.oid); err != nil {
				return fmt.Errorf("forward %s: %w", p.oid, err)
			}
		}
		if p.pinState == string(PinLUN) {
			if p.relPath == "" {
				continue
			}
			if finalTierID == "" {
				return fmt.Errorf("recover %s: missing tier id for pin repair", p.oid)
			}
			if err := restoreLUNPin(ctx, db, p.namespaceID, finalTierID, p.relPath); err != nil {
				return err
			}
		}
	}
	return nil
}

func restoreLUNPin(ctx context.Context, db *sql.DB, namespaceID, tierID, relPath string) error {
	var lower sql.NullString
	if err := db.QueryRowContext(ctx, `
		SELECT COALESCE(tt.backing_ref, '')
		  FROM managed_namespaces mn
		  JOIN tier_targets tt
		    ON tt.placement_domain = mn.placement_domain
		 WHERE mn.id = ?
		   AND tt.id = ?
		   AND tt.backend_kind = 'smoothfs'`,
		namespaceID, tierID).Scan(&lower); err != nil {
		return fmt.Errorf("recover pin: resolve %s:%s backing path: %w", namespaceID, tierID, err)
	}
	if !lower.Valid || lower.String == "" {
		return fmt.Errorf("recover pin: resolve %s:%s backing path: missing backing_ref",
			namespaceID, tierID)
	}
	if err := setLUNPin(filepath.Join(lower.String, relPath)); err != nil {
		return err
	}
	return nil
}
