package controlplane

import (
	"context"
	"database/sql"
	"encoding/hex"
	"math"
	"testing"
	"time"
)

func seedObject(t *testing.T, sqlDB *sql.DB, nsID, tierID string, oid [OIDLen]byte) {
	t.Helper()
	_, err := sqlDB.Exec(`INSERT INTO smoothfs_objects
	        (object_id, namespace_id, current_tier_id)
	        VALUES (?, ?, ?)`,
		hex.EncodeToString(oid[:]), nsID, tierID)
	if err != nil {
		t.Fatalf("seed object: %v", err)
	}
}

func readEWMA(t *testing.T, sqlDB *sql.DB, oid [OIDLen]byte) (ewma float64, lastSample sql.NullString) {
	t.Helper()
	err := sqlDB.QueryRow(
		`SELECT ewma_value, last_heat_sample_at FROM smoothfs_objects WHERE object_id = ?`,
		hex.EncodeToString(oid[:])).Scan(&ewma, &lastSample)
	if err != nil {
		t.Fatalf("read ewma: %v", err)
	}
	return
}

func TestHeatAggregatorFirstSample(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, _ := seedPool(t, sqlDB)

	var oid [OIDLen]byte
	oid[0] = 0x01
	seedObject(t, sqlDB, nsID, tier0, oid)

	fixedNow := time.Date(2026, 4, 17, 12, 0, 0, 0, time.UTC)
	h := NewHeatAggregator(sqlDB, 86400)
	h.SetNow(func() time.Time { return fixedNow })

	samples := []HeatSampleRecord{{
		OID:             oid,
		OpenCountDelta:  3,
		ReadBytesDelta:  1 << 20,
		WriteBytesDelta: 2 << 20,
	}}
	if err := h.Apply(context.Background(), samples); err != nil {
		t.Fatalf("apply: %v", err)
	}

	wantScore := float64(1<<20) + 2.0*float64(2<<20) + 4096.0*3
	gotEWMA, lastSample := readEWMA(t, sqlDB, oid)
	if gotEWMA != wantScore {
		t.Fatalf("ewma = %v, want %v", gotEWMA, wantScore)
	}
	if !lastSample.Valid {
		t.Fatalf("last_heat_sample_at should be set after first sample")
	}
}

func TestHeatAggregatorDecayMerge(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, _ := seedPool(t, sqlDB)

	var oid [OIDLen]byte
	oid[0] = 0x02
	seedObject(t, sqlDB, nsID, tier0, oid)

	halfLife := 3600
	t0 := time.Date(2026, 4, 17, 12, 0, 0, 0, time.UTC)
	t1 := t0.Add(time.Duration(halfLife) * time.Second)

	var tNow time.Time
	h := NewHeatAggregator(sqlDB, halfLife)
	h.SetNow(func() time.Time { return tNow })

	tNow = t0
	if err := h.Apply(context.Background(), []HeatSampleRecord{{OID: oid, ReadBytesDelta: 1000}}); err != nil {
		t.Fatalf("first apply: %v", err)
	}
	ewma0, _ := readEWMA(t, sqlDB, oid)
	if ewma0 != 1000 {
		t.Fatalf("first ewma = %v, want 1000", ewma0)
	}

	tNow = t1
	if err := h.Apply(context.Background(), []HeatSampleRecord{{OID: oid, ReadBytesDelta: 500}}); err != nil {
		t.Fatalf("second apply: %v", err)
	}
	ewma1, _ := readEWMA(t, sqlDB, oid)
	want := 1000.0*0.5 + 500.0
	if math.Abs(ewma1-want) > 1e-6 {
		t.Fatalf("merged ewma = %v, want %v", ewma1, want)
	}
}

func TestHeatAggregatorSkipsUnknown(t *testing.T) {
	sqlDB := testDB(t)
	_, _, _ = seedPool(t, sqlDB)

	var oid [OIDLen]byte
	oid[0] = 0x99

	h := NewHeatAggregator(sqlDB, 86400)
	if err := h.Apply(context.Background(), []HeatSampleRecord{{OID: oid, ReadBytesDelta: 12345}}); err != nil {
		t.Fatalf("apply: %v", err)
	}

	var count int
	if err := sqlDB.QueryRow(
		`SELECT COUNT(*) FROM smoothfs_objects WHERE object_id = ?`,
		hex.EncodeToString(oid[:])).Scan(&count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 0 {
		t.Fatalf("unknown-oid sample should not create a row (count=%d)", count)
	}
}

func TestHeatAggregatorEmptyBatch(t *testing.T) {
	sqlDB := testDB(t)
	h := NewHeatAggregator(sqlDB, 86400)
	if err := h.Apply(context.Background(), nil); err != nil {
		t.Fatalf("apply nil: %v", err)
	}
	if err := h.Apply(context.Background(), []HeatSampleRecord{}); err != nil {
		t.Fatalf("apply empty: %v", err)
	}
}
