package controlplane

import (
	"context"
	"errors"
	"testing"

	"github.com/google/uuid"
)

type fakeMovementClient struct {
	inspectResult *InspectResult
	movePlanned   bool
}

func (f *fakeMovementClient) Inspect(uuid.UUID, [OIDLen]byte) (*InspectResult, error) {
	return f.inspectResult, nil
}

func (f *fakeMovementClient) MovePlan(uuid.UUID, [OIDLen]byte, uint8, uint64) error {
	f.movePlanned = true
	return nil
}

func (f *fakeMovementClient) MoveCutover(uuid.UUID, [OIDLen]byte, uint64) error {
	return nil
}

func TestWorkerRefusesPinnedLUNBeforeMovePlan(t *testing.T) {
	sqlDB := testDB(t)
	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinLUN,
			RelPath:  "luns/web-app.img",
		},
	}
	worker := NewWorker(sqlDB, client)

	plan := MovementPlan{
		PoolUUID:       uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		NamespaceID:    "ns",
		SourceTierID:   "fast",
		SourceTierRank: 0,
		SourceLowerDir: t.TempDir(),
		DestTierID:     "slow",
		DestTierRank:   1,
		DestLowerDir:   t.TempDir(),
		RelPath:        "luns/web-app.img",
		TransactionSeq: 7,
	}

	err := worker.Execute(context.Background(), plan)
	if !errors.Is(err, ErrLUNQuiesceRequired) {
		t.Fatalf("Execute error = %v, want ErrLUNQuiesceRequired", err)
	}
	if client.movePlanned {
		t.Fatal("worker called MovePlan for a PIN_LUN object")
	}
}
