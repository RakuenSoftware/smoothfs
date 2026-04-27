package controlplane

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/google/uuid"
)

type fakeMovementClient struct {
	inspectResult *InspectResult
	movePlanErr   error
	movePlanned   bool
	cutoverCalled bool
}

func (f *fakeMovementClient) Inspect(uuid.UUID, [OIDLen]byte) (*InspectResult, error) {
	return f.inspectResult, nil
}

func (f *fakeMovementClient) MovePlan(uuid.UUID, [OIDLen]byte, uint8, uint64) error {
	f.movePlanned = true
	return f.movePlanErr
}

func (f *fakeMovementClient) MoveCutover(uuid.UUID, [OIDLen]byte, uint64) error {
	f.cutoverCalled = true
	return nil
}

type fakeLUNTargetResumer struct {
	resumedTarget string
	onResume      func(string)
}

func (f *fakeLUNTargetResumer) Resume(ctx context.Context, targetID string) error {
	f.resumedTarget = targetID
	if f.onResume != nil {
		f.onResume(targetID)
	}
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

func TestWorkerRePinsLUNDestinationAfterCutover(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	srcRoot := t.TempDir()
	dstRoot := t.TempDir()
	relPath := filepath.Join("luns", "web-app.img")
	srcPath := filepath.Join(srcRoot, relPath)
	dstPath := filepath.Join(dstRoot, relPath)
	if err := os.MkdirAll(filepath.Dir(srcPath), 0o755); err != nil {
		t.Fatalf("mkdir src: %v", err)
	}
	if err := os.WriteFile(srcPath, []byte("lun payload"), 0o644); err != nil {
		t.Fatalf("write src: %v", err)
	}

	var oid [OIDLen]byte
	oid[0] = 0x51
	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  relPath,
		},
	}
	var pinnedPath string
	var pinBeforeCutover bool
	var resumedTarget string
	var resumeBeforePin bool
	resumer := &fakeLUNTargetResumer{
		onResume: func(targetID string) {
			resumedTarget = targetID
			resumeBeforePin = pinnedPath == ""
		},
	}
	worker := NewWorkerWithLUNResumer(sqlDB, client, resumer)
	worker.setLUNPin = func(path string) error {
		pinnedPath = path
		pinBeforeCutover = !client.cutoverCalled
		return nil
	}

	plan := MovementPlan{
		PoolUUID:       uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		ObjectID:       oid,
		NamespaceID:    nsID,
		SourceTierID:   tier0,
		SourceTierRank: 0,
		SourceLowerDir: srcRoot,
		DestTierID:     tier1,
		DestTierRank:   1,
		DestLowerDir:   dstRoot,
		RelPath:        relPath,
		TransactionSeq: 8,
		RePinLUN:       true,
		LUNTargetID:    "iqn.2026-04.com.smoothnas:web-app",
	}

	if err := worker.Execute(context.Background(), plan); err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if pinnedPath != dstPath {
		t.Fatalf("pinned path = %q, want %q", pinnedPath, dstPath)
	}
	if pinBeforeCutover {
		t.Fatal("worker re-pinned LUN before cutover completed")
	}
	if resumedTarget != "iqn.2026-04.com.smoothnas:web-app" {
		t.Fatalf("resumed target = %q, want target ID from plan", resumedTarget)
	}
	if resumer.resumedTarget != "iqn.2026-04.com.smoothnas:web-app" {
		t.Fatalf("resumer target = %q, want target ID from plan", resumer.resumedTarget)
	}
	if resumeBeforePin {
		t.Fatal("worker resumed LUN target before destination re-pin")
	}
	got, err := os.ReadFile(dstPath)
	if err != nil {
		t.Fatalf("read dst: %v", err)
	}
	if string(got) != "lun payload" {
		t.Fatalf("dst payload = %q, want %q", got, "lun payload")
	}

	var currentTier, movementState, pinState string
	if err := sqlDB.QueryRow(`SELECT current_tier_id, movement_state, pin_state
		FROM smoothfs_objects WHERE object_id = ?`,
		"51000000000000000000000000000000").Scan(&currentTier, &movementState, &pinState); err != nil {
		t.Fatalf("read object state: %v", err)
	}
	if currentTier != tier1 {
		t.Fatalf("current_tier_id = %q, want %q", currentTier, tier1)
	}
	if movementState != string(StatePlaced) {
		t.Fatalf("movement_state = %q, want %q", movementState, StatePlaced)
	}
	if pinState != string(PinLUN) {
		t.Fatalf("pin_state = %q, want %q", pinState, PinLUN)
	}
}

func TestWorkerFailsPreparedLUNMoveWithoutResumer(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	srcRoot := t.TempDir()
	dstRoot := t.TempDir()
	relPath := filepath.Join("luns", "web-app.img")
	srcPath := filepath.Join(srcRoot, relPath)
	if err := os.MkdirAll(filepath.Dir(srcPath), 0o755); err != nil {
		t.Fatalf("mkdir src: %v", err)
	}
	if err := os.WriteFile(srcPath, []byte("lun payload"), 0o644); err != nil {
		t.Fatalf("write src: %v", err)
	}

	var oid [OIDLen]byte
	oid[0] = 0x52
	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  relPath,
		},
	}
	worker := NewWorker(sqlDB, client)
	worker.setLUNPin = func(string) error { return nil }

	plan := MovementPlan{
		PoolUUID:       uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		ObjectID:       oid,
		NamespaceID:    nsID,
		SourceTierID:   tier0,
		SourceTierRank: 0,
		SourceLowerDir: srcRoot,
		DestTierID:     tier1,
		DestTierRank:   1,
		DestLowerDir:   dstRoot,
		RelPath:        relPath,
		TransactionSeq: 9,
		RePinLUN:       true,
		LUNTargetID:    "iqn.2026-04.com.smoothnas:web-app",
	}

	err := worker.Execute(context.Background(), plan)
	if !errors.Is(err, ErrLUNResumeRequired) {
		t.Fatalf("Execute error = %v, want ErrLUNResumeRequired", err)
	}

	var currentTier, movementState, pinState, reason string
	if err := sqlDB.QueryRow(`SELECT current_tier_id, movement_state, pin_state, failure_reason
		FROM smoothfs_objects WHERE object_id = ?`,
		"52000000000000000000000000000000").Scan(&currentTier, &movementState, &pinState, &reason); err != nil {
		t.Fatalf("read object state: %v", err)
	}
	if currentTier != tier1 {
		t.Fatalf("current_tier_id = %q, want %q", currentTier, tier1)
	}
	if movementState != string(StateFailed) {
		t.Fatalf("movement_state = %q, want %q", movementState, StateFailed)
	}
	if pinState != string(PinLUN) {
		t.Fatalf("pin_state = %q, want %q", pinState, PinLUN)
	}
	if reason == "" {
		t.Fatal("failure_reason should be populated")
	}
}

func TestWorkerRePinsSourceAndResumesTargetBeforeSwitchFailure(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	srcRoot := t.TempDir()
	dstRoot := t.TempDir()
	relPath := filepath.Join("luns", "web-app.img")
	srcPath := filepath.Join(srcRoot, relPath)
	if err := os.MkdirAll(filepath.Dir(srcPath), 0o755); err != nil {
		t.Fatalf("mkdir src: %v", err)
	}
	if err := os.WriteFile(srcPath, []byte("lun payload"), 0o644); err != nil {
		t.Fatalf("write src: %v", err)
	}

	var oid [OIDLen]byte
	oid[0] = 0x53
	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  relPath,
		},
		movePlanErr: errors.New("kernel refused plan"),
	}
	var pinnedPath string
	var resumedTarget string
	resumer := &fakeLUNTargetResumer{
		onResume: func(targetID string) {
			resumedTarget = targetID
		},
	}
	worker := NewWorkerWithLUNResumer(sqlDB, client, resumer)
	worker.setLUNPin = func(path string) error {
		pinnedPath = path
		return nil
	}

	plan := MovementPlan{
		PoolUUID:       uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		ObjectID:       oid,
		NamespaceID:    nsID,
		SourceTierID:   tier0,
		SourceTierRank: 0,
		SourceLowerDir: srcRoot,
		DestTierID:     tier1,
		DestTierRank:   1,
		DestLowerDir:   dstRoot,
		RelPath:        relPath,
		TransactionSeq: 10,
		RePinLUN:       true,
		LUNTargetID:    "iqn.2026-04.com.smoothnas:web-app",
	}

	err := worker.Execute(context.Background(), plan)
	if err == nil || !errors.Is(err, client.movePlanErr) {
		t.Fatalf("Execute error = %v, want move_plan error", err)
	}
	if pinnedPath != srcPath {
		t.Fatalf("pinned path = %q, want source %q", pinnedPath, srcPath)
	}
	if resumedTarget != "iqn.2026-04.com.smoothnas:web-app" {
		t.Fatalf("resumed target = %q, want target ID from plan", resumedTarget)
	}
}
