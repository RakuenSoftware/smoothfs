package controlplane

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

type recoveringMovementClient struct {
	fakeMovementClient
	result                InspectResult
	ackErr                error
	beforeAck             func()
	completed, rolledBack int
}

func (c *recoveringMovementClient) Inspect(uuid.UUID, [OIDLen]byte) (*InspectResult, error) {
	r := c.result
	return &r, nil
}
func (c *recoveringMovementClient) MoveComplete(uuid.UUID, [OIDLen]byte, uint64) error {
	c.completed++
	return c.ack()
}
func (c *recoveringMovementClient) MoveRollback(uuid.UUID, [OIDLen]byte, uint64) error {
	c.rolledBack++
	return c.ack()
}
func (c *recoveringMovementClient) ack() error {
	if c.beforeAck != nil {
		c.beforeAck()
	}
	if c.ackErr != nil {
		return c.ackErr
	}
	c.result.MovementState = StatePlaced
	return nil
}

func TestRecoverLiveUsesKernelPlacementAndDurableCleanup(t *testing.T) {
	for _, tc := range []struct {
		name        string
		sql, kernel MovementState
		rank        uint8
	}{
		{"before-copy", StatePlanAccepted, StatePlanAccepted, 0},
		{"after-copy", StateCopyVerified, StatePlanAccepted, 0},
		{"cutover-before-sql", StateCopyVerified, StateSwitched, 1},
		{"cleanup-before-ack", StateSwitched, StateSwitched, 1},
		{"ack-before-sql", StateSwitched, StatePlaced, 1},
		{"mount-replay-forward", StateCopyVerified, StatePlaced, 1},
		{"mount-replay-rollback", StateSwitched, StatePlaced, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			db := testDB(t)
			ns, fast, slow := seedPool(t, db)
			fastDir, slowDir := t.TempDir(), t.TempDir()
			pool := uuid.New()
			if _, err := db.Exec(`UPDATE managed_namespaces SET backend_ref=? WHERE id=?`, pool.String(), ns); err != nil {
				t.Fatal(err)
			}
			for id, path := range map[string]string{fast: fastDir, slow: slowDir} {
				if _, err := db.Exec(`UPDATE tier_targets SET backing_ref=? WHERE id=?`, path, id); err != nil {
					t.Fatal(err)
				}
			}
			oid := [OIDLen]byte{0x42}
			seedMidMove(t, db, ns, fast, slow, tc.sql, 11, oid)
			for _, dir := range []string{fastDir, slowDir} {
				if err := os.WriteFile(filepath.Join(dir, "chunk"), []byte("bytes"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			canonical, other, tier := fastDir, slowDir, fast
			if tc.rank == 1 {
				canonical, other, tier = slowDir, fastDir, slow
			}
			client := &recoveringMovementClient{result: InspectResult{MovementState: tc.kernel, CurrentTier: tc.rank, TransactionSeq: 11, RelPath: "chunk", Generation: 4}}
			client.beforeAck = func() {
				if _, err := os.Stat(filepath.Join(other, "chunk")); !os.IsNotExist(err) {
					t.Fatal("ack preceded deletion")
				}
				_, _, state, _, _ := readObjectState(t, db, oid)
				if state != string(tc.sql) {
					t.Fatal("SQL reported recovery before kernel ack")
				}
			}
			if err := Recover(context.Background(), db, client); err != nil {
				t.Fatal(err)
			}
			cur, intended, state, gen, _ := readObjectState(t, db, oid)
			if cur.String != tier || intended.Valid || state != "placed" || gen != 4 {
				t.Fatalf("recovery cur=%s intended=%v state=%s generation=%d", cur.String, intended, state, gen)
			}
			if data, err := os.ReadFile(filepath.Join(canonical, "chunk")); err != nil || string(data) != "bytes" {
				t.Fatal("authoritative copy lost", err)
			}
			if _, err := os.Stat(filepath.Join(other, "chunk")); !os.IsNotExist(err) {
				t.Fatal("old copy remains")
			}
			if tc.kernel == StateSwitched && client.completed != 1 {
				t.Fatal("missing completion ack")
			}
			if tc.kernel == StatePlanAccepted && client.rolledBack != 1 {
				t.Fatal("missing rollback ack")
			}
		})
	}
}

func TestRecoverLiveAckFailurePreservesPendingStateAndRetriesAbsentSource(t *testing.T) {
	db := testDB(t)
	ns, fast, slow := seedPool(t, db)
	a, b := t.TempDir(), t.TempDir()
	pool := uuid.New()
	db.Exec(`UPDATE managed_namespaces SET backend_ref=? WHERE id=?`, pool.String(), ns)
	db.Exec(`UPDATE tier_targets SET backing_ref=? WHERE id=?`, a, fast)
	db.Exec(`UPDATE tier_targets SET backing_ref=? WHERE id=?`, b, slow)
	oid := [OIDLen]byte{0x43}
	seedMidMove(t, db, ns, fast, slow, StateSwitched, 12, oid)
	os.WriteFile(filepath.Join(a, "chunk"), []byte("old"), 0o600)
	os.WriteFile(filepath.Join(b, "chunk"), []byte("current"), 0o600)
	client := &recoveringMovementClient{result: InspectResult{MovementState: StateSwitched, CurrentTier: 1, TransactionSeq: 12, RelPath: "chunk"}, ackErr: errors.New("ack failed")}
	if err := Recover(context.Background(), db, client); err == nil {
		t.Fatal("ack failure accepted")
	}
	_, _, state, _, _ := readObjectState(t, db, oid)
	if state != "switched" {
		t.Fatal("failed acknowledgement finalized SQL")
	}
	client.ackErr = nil
	if err := Recover(context.Background(), db, client); err != nil {
		t.Fatal(err)
	}
	_, _, state, _, _ = readObjectState(t, db, oid)
	if state != "placed" {
		t.Fatal(state)
	}
}

func TestRecoverLiveRejectsStaleOrAmbiguousCleanup(t *testing.T) {
	for _, failure := range []string{"stale", "directory", "invalid-relative-path", "missing-authority", "missing-pool"} {
		t.Run(failure, func(t *testing.T) {
			db := testDB(t)
			ns, fast, slow := seedPool(t, db)
			a, b := t.TempDir(), t.TempDir()
			db.Exec(`UPDATE managed_namespaces SET backend_ref=? WHERE id=?`, uuid.NewString(), ns)
			db.Exec(`UPDATE tier_targets SET backing_ref=? WHERE id=?`, a, fast)
			db.Exec(`UPDATE tier_targets SET backing_ref=? WHERE id=?`, b, slow)
			oid := [OIDLen]byte{0x44}
			seedMidMove(t, db, ns, fast, slow, StateSwitched, 12, oid)
			os.WriteFile(filepath.Join(b, "chunk"), []byte("current"), 0o600)
			if failure == "directory" {
				os.Mkdir(filepath.Join(a, "chunk"), 0o700)
			} else {
				os.WriteFile(filepath.Join(a, "chunk"), []byte("old"), 0o600)
			}
			client := &recoveringMovementClient{result: InspectResult{MovementState: StateSwitched, CurrentTier: 1, TransactionSeq: 12, RelPath: "chunk"}}
			switch failure {
			case "stale":
				client.result.TransactionSeq = 13
			case "invalid-relative-path":
				client.result.RelPath = "../chunk"
			case "missing-authority":
				os.Remove(filepath.Join(b, "chunk"))
			case "missing-pool":
				db.Exec(`UPDATE managed_namespaces SET backend_ref='' WHERE id=?`, ns)
			}
			if err := Recover(context.Background(), db, client); err == nil {
				t.Fatal("unsafe recovery accepted")
			}
			if _, err := os.Lstat(filepath.Join(a, "chunk")); err != nil {
				t.Fatal("removed copy on failed recovery")
			}
			_, _, state, _, _ := readObjectState(t, db, oid)
			if state != "switched" {
				t.Fatal(state)
			}
			if client.completed != 0 || client.rolledBack != 0 {
				t.Fatal("acknowledged rejected recovery")
			}
		})
	}
}

func TestRecoveryMountpointFindsPoolRootForNamespace(t *testing.T) {
	mounts := `10 1 0:20 / /mnt/pbs rw - smoothfs none rw,pool=media
11 1 0:21 / /mnt/pbs2 rw - smoothfs none rw,pool=other
12 1 0:22 /sub /bind-sub rw - smoothfs none rw
13 1 0:23 / /mnt/my\040pool rw - smoothfs none rw
14 1 8:1 / /ordinary rw - ext4 /dev/sda1 rw
`
	for exposed, want := range map[string]string{"/mnt/pbs": "/mnt/pbs", "/mnt/pbs/nested/ns": "/mnt/pbs", "/mnt/pbs2/sub": "/mnt/pbs2", "/mnt/pbs-not-a-pool": "", "/bind-sub": "", "/ordinary": "", "/mnt/my pool/ns": "/mnt/my pool"} {
		if got := recoveryMountpoint(mounts, exposed); got != want {
			t.Errorf("%q: got %q want %q", exposed, got, want)
		}
	}
}
