package controlplane

import (
	"context"
	"database/sql"
	"fmt"
)

// Recover scans smoothfs_objects for non-terminal movement states and
// returns each row to a clean state.
func Recover(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx, `
		SELECT object_id, current_tier_id, intended_tier_id, movement_state, transaction_seq
		  FROM smoothfs_objects
		 WHERE movement_state NOT IN ('placed','cleanup_complete','failed','stale')`)
	if err != nil {
		return fmt.Errorf("scan in-flight movements: %w", err)
	}
	defer rows.Close()

	type pending struct {
		oid, src, dst, state string
		seq                  uint64
	}
	var todo []pending
	for rows.Next() {
		var p pending
		var dst sql.NullString
		if err := rows.Scan(&p.oid, &p.src, &dst, &p.state, &p.seq); err != nil {
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
		switch MovementState(p.state) {
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
	}
	return nil
}
