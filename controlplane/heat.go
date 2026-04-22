package controlplane

import (
	"context"
	"database/sql"
	"encoding/hex"
	"fmt"
	"math"
	"time"
)

// HeatAggregator persists per-object heat samples as an EWMA over
// time (Phase 0 §0.5). Samples arrive batched via netlink; this
// type just feeds them into smoothfs_objects.
type HeatAggregator struct {
	db          *sql.DB
	halfLifeSec float64
	now         func() time.Time
}

// NewHeatAggregator. halfLifeSec defaults to 24h if 0.
func NewHeatAggregator(db *sql.DB, halfLifeSec int) *HeatAggregator {
	if halfLifeSec <= 0 {
		halfLifeSec = 86400
	}
	return &HeatAggregator{
		db:          db,
		halfLifeSec: float64(halfLifeSec),
		now:         time.Now,
	}
}

// SetNow overrides the clock source for tests.
func (h *HeatAggregator) SetNow(now func() time.Time) {
	if now == nil {
		h.now = time.Now
		return
	}
	h.now = now
}

// Apply ingests a batch of heat samples for one object. The score is
// computed as a weighted sum of the deltas (write_bytes weighted 2x
// vs reads, opens count as 4 KiB-equivalent), then EWMA-merged with
// the existing value using the gap since last_heat_sample_at.
func (h *HeatAggregator) Apply(ctx context.Context, samples []HeatSampleRecord) error {
	if len(samples) == 0 {
		return nil
	}
	tx, err := h.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()

	now := h.now().UTC().Format(time.RFC3339Nano)
	var (
		curEWMA   sql.NullFloat64
		curSample sql.NullString
	)

	stmtSelect, err := tx.PrepareContext(ctx,
		`SELECT ewma_value, last_heat_sample_at FROM smoothfs_objects WHERE object_id = ?`)
	if err != nil {
		return err
	}
	defer stmtSelect.Close()

	stmtUpdate, err := tx.PrepareContext(ctx,
		`UPDATE smoothfs_objects
		    SET ewma_value = ?,
		        last_heat_sample_at = ?,
		        updated_at = datetime('now')
		  WHERE object_id = ?`)
	if err != nil {
		return err
	}
	defer stmtUpdate.Close()

	for _, s := range samples {
		oid := hex.EncodeToString(s.OID[:])
		err := stmtSelect.QueryRowContext(ctx, oid).Scan(&curEWMA, &curSample)
		if err != nil && err != sql.ErrNoRows {
			return fmt.Errorf("read object %s: %w", oid, err)
		}
		if err == sql.ErrNoRows {
			continue
		}

		score := float64(s.ReadBytesDelta) +
			2.0*float64(s.WriteBytesDelta) +
			4096.0*float64(s.OpenCountDelta)

		var merged float64
		if curEWMA.Valid && curSample.Valid {
			gap := h.gapSeconds(curSample.String)
			decay := math.Exp2(-gap / h.halfLifeSec)
			merged = curEWMA.Float64*decay + score
		} else {
			merged = score
		}
		if _, err := stmtUpdate.ExecContext(ctx, merged, now, oid); err != nil {
			return fmt.Errorf("update object %s: %w", oid, err)
		}
	}
	return tx.Commit()
}

func (h *HeatAggregator) gapSeconds(prev string) float64 {
	t, err := time.Parse(time.RFC3339Nano, prev)
	if err != nil {
		return 0
	}
	d := h.now().Sub(t).Seconds()
	if d < 0 {
		d = 0
	}
	return d
}
