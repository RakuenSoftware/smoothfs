package controlplane

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

type completingMovementClient struct {
	fakeMovementClient
	cutover  func()
	complete func() error
}

func (c *completingMovementClient) MoveCutover(uuid.UUID, [OIDLen]byte, uint64) error {
	if c.cutover != nil {
		c.cutover()
	}
	return nil
}
func (c *completingMovementClient) MoveComplete(uuid.UUID, [OIDLen]byte, uint64) error {
	return c.complete()
}

func TestWorkerAcknowledgesOnlyDurableCleanup(t *testing.T) {
	for _, name := range []string{"success", "ack failure", "unlink failure"} {
		t.Run(name, func(t *testing.T) {
			db := testDB(t)
			ns, srcID, dstID := seedPool(t, db)
			srcDir, dstDir := t.TempDir(), t.TempDir()
			src := filepath.Join(srcDir, "chunk")
			dst := filepath.Join(dstDir, "chunk")
			payload := []byte("a completed PBS chunk must remain readable")
			if err := os.WriteFile(src, payload, 0600); err != nil {
				t.Fatal(err)
			}
			var oid [OIDLen]byte
			oid[0] = 0xb7
			called := false
			c := &completingMovementClient{fakeMovementClient: fakeMovementClient{inspectResult: &InspectResult{CurrentTier: 0, RelPath: "chunk", PinState: PinNone}}}
			c.complete = func() error {
				called = true
				if _, err := os.Stat(src); !os.IsNotExist(err) {
					t.Fatalf("completion before source cleanup: %v", err)
				}
				if data, err := os.ReadFile(dst); err != nil || string(data) != string(payload) {
					t.Fatalf("destination lost: %v", err)
				}
				_, _, state, _, _ := readObjectState(t, db, oid)
				if state != string(StateSwitched) {
					t.Fatalf("state before acknowledgement=%q", state)
				}
				if name == "ack failure" {
					return errors.New("kernel rejected completion")
				}
				return nil
			}
			if name == "unlink failure" {
				c.cutover = func() {
					if err := os.Remove(src); err != nil {
						t.Fatal(err)
					}
					if err := os.Mkdir(src, 0700); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(filepath.Join(src, "held"), nil, 0600); err != nil {
						t.Fatal(err)
					}
				}
			}
			plan := MovementPlan{PoolUUID: uuid.New(), ObjectID: oid, NamespaceID: ns, SourceTierID: srcID, SourceLowerDir: srcDir, DestTierID: dstID, DestTierRank: 1, DestLowerDir: dstDir, RelPath: "chunk", TransactionSeq: 77}
			err := NewWorker(db, c).Execute(context.Background(), plan)
			cur, intended, state, _, _ := readObjectState(t, db, oid)
			if name == "success" {
				if err != nil || !called || state != string(StatePlaced) || cur.String != dstID || intended.Valid {
					t.Fatalf("success: err=%v called=%v state=%q current=%q intended=%v", err, called, state, cur.String, intended)
				}
			} else {
				if err == nil || state != string(StateCleanupInProgress) || cur.String != srcID || intended.String != dstID {
					t.Fatalf("failed cleanup reported complete: err=%v state=%q current=%q intended=%v", err, state, cur.String, intended)
				}
				if called != (name == "ack failure") {
					t.Fatalf("completion called=%v after %s", called, name)
				}
				if data, err := os.ReadFile(dst); err != nil || string(data) != string(payload) {
					t.Fatalf("cleanup failure lost destination: %v", err)
				}
			}
		})
	}
}
