package controlplane

import (
	"context"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

type intentMovementClient struct {
	fakeMovementClient
	beforePlan func()
}

func (c *intentMovementClient) MovePlan(p uuid.UUID, o [OIDLen]byte, d uint8, s uint64) error {
	if c.beforePlan != nil {
		c.beforePlan()
	}
	return c.fakeMovementClient.MovePlan(p, o, d, s)
}
func TestWorkerPersistsFullSyncIntentBeforeKernelPlan(t *testing.T) {
	db := testDB(t)
	db.SetMaxOpenConns(1)
	ns, fast, slow := seedPool(t, db)
	if _, err := db.Exec(`PRAGMA synchronous=NORMAL`); err != nil {
		t.Fatal(err)
	}
	a, b := t.TempDir(), t.TempDir()
	if err := os.WriteFile(filepath.Join(a, "chunk"), []byte("bytes"), 0o600); err != nil {
		t.Fatal(err)
	}
	oid := [OIDLen]byte{0x51}
	c := &intentMovementClient{fakeMovementClient: fakeMovementClient{inspectResult: &InspectResult{MovementState: StatePlaced, RelPath: "chunk"}}}
	c.beforePlan = func() {
		var state, rel string
		var seq int64
		if err := db.QueryRow(`SELECT movement_state,rel_path,transaction_seq FROM smoothfs_objects WHERE object_id=?`, hex.EncodeToString(oid[:])).Scan(&state, &rel, &seq); err != nil {
			t.Fatal(err)
		}
		if state != "plan_accepted" || rel != "chunk" || seq != 17 {
			t.Fatalf("missing prior intent: %s %s %d", state, rel, seq)
		}
		var n int
		if err := db.QueryRow(`SELECT count(*) FROM smoothfs_movement_log WHERE object_id=? AND transaction_seq=17`, hex.EncodeToString(oid[:])).Scan(&n); err != nil || n != 1 {
			t.Fatal("missing prior log", err, n)
		}
	}
	p := MovementPlan{PoolUUID: uuid.New(), ObjectID: oid, NamespaceID: ns, SourceTierID: fast, SourceLowerDir: a, DestTierID: slow, DestTierRank: 1, DestLowerDir: b, RelPath: "chunk", TransactionSeq: 17}
	if err := NewWorker(db, c).Execute(context.Background(), p); err != nil {
		t.Fatal(err)
	}
	var sync int
	if err := db.QueryRow(`PRAGMA synchronous`).Scan(&sync); err != nil || sync != 1 {
		t.Fatal("connection synchronous setting not restored", err, sync)
	}
}

func TestWorkerDoesNotPlanWithoutCommittedIntent(t *testing.T) {
	db := testDB(t)
	ns, fast, slow := seedPool(t, db)
	if _, err := db.Exec(`CREATE TRIGGER reject_intent BEFORE INSERT ON smoothfs_movement_log BEGIN SELECT RAISE(ABORT,'simulated database write failure'); END`); err != nil {
		t.Fatal(err)
	}
	a, b := t.TempDir(), t.TempDir()
	os.WriteFile(filepath.Join(a, "chunk"), []byte("bytes"), 0o600)
	c := &fakeMovementClient{inspectResult: &InspectResult{MovementState: StatePlaced, RelPath: "chunk"}}
	p := MovementPlan{PoolUUID: uuid.New(), ObjectID: [OIDLen]byte{0x52}, NamespaceID: ns, SourceTierID: fast, SourceLowerDir: a, DestTierID: slow, DestTierRank: 1, DestLowerDir: b, RelPath: "chunk", TransactionSeq: 18}
	if err := NewWorker(db, c).Execute(context.Background(), p); err == nil || c.movePlanned {
		t.Fatal("kernel plan issued without durable intent", err)
	}
	var n int
	if err := db.QueryRow(`SELECT count(*) FROM smoothfs_objects`).Scan(&n); err != nil || n != 0 {
		t.Fatal("intent transaction was not rolled back", err, n)
	}
}

func TestWorkerPreservesInterruptedTransaction(t *testing.T) {
	for _, state := range []MovementState{StatePlaced, StateSwitched} {
		t.Run(string(state), func(t *testing.T) {
			db := testDB(t)
			ns, fast, slow := seedPool(t, db)
			oid := [OIDLen]byte{0x53}
			seedMidMove(t, db, ns, fast, slow, StateSwitched, 19, oid)
			c := &fakeMovementClient{inspectResult: &InspectResult{MovementState: state, RelPath: "chunk"}}
			p := MovementPlan{PoolUUID: uuid.New(), ObjectID: oid, NamespaceID: ns, SourceTierID: fast, SourceLowerDir: t.TempDir(), DestTierID: slow, DestTierRank: 1, DestLowerDir: t.TempDir(), RelPath: "chunk", TransactionSeq: 20}
			if err := NewWorker(db, c).Execute(context.Background(), p); err == nil || c.movePlanned {
				t.Fatal("overwrote pending movement", err)
			}
			_, _, got, _, _ := readObjectState(t, db, oid)
			if got != "switched" {
				t.Fatal(got)
			}
			var seq uint64
			if err := db.QueryRow(`SELECT transaction_seq FROM smoothfs_objects`).Scan(&seq); err != nil || seq != 19 {
				t.Fatal("old transaction lost", err, seq)
			}
		})
	}
}
