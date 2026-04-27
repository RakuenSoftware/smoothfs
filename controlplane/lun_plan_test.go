package controlplane

import (
	"context"
	"errors"
	"testing"

	"github.com/google/uuid"
)

func TestBuildQuiescedLUNMovementPlanMarksRePin(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir()},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir()},
		},
	}

	var oid [OIDLen]byte
	oid[0] = 0x71
	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, pin_state, rel_path)
		VALUES
			('71000000000000000000000000000000', ?, ?, 'placed', 'pin_lun', '')`,
		nsID, tier0ID)
	if err != nil {
		t.Fatalf("insert lun object: %v", err)
	}

	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  "luns/db.img",
		},
	}

	plan, err := BuildQuiescedLUNMovementPlan(
		context.Background(), sqlDB, client, pool, oid, tier1ID)
	if err != nil {
		t.Fatalf("BuildQuiescedLUNMovementPlan: %v", err)
	}
	if !plan.RePinLUN {
		t.Fatal("plan should request LUN re-pin")
	}
	if plan.SourceTierID != tier0ID || plan.DestTierID != tier1ID {
		t.Fatalf("plan tiers = %q -> %q, want %q -> %q",
			plan.SourceTierID, plan.DestTierID, tier0ID, tier1ID)
	}
	if plan.RelPath != "luns/db.img" {
		t.Fatalf("rel_path = %q, want kernel fallback", plan.RelPath)
	}
	if plan.TransactionSeq != 1 {
		t.Fatalf("transaction seq = %d, want 1", plan.TransactionSeq)
	}
}

func TestBuildQuiescedLUNMovementPlanRequiresKernelUnpinned(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir()},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir()},
		},
	}

	var oid [OIDLen]byte
	oid[0] = 0x72
	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, pin_state, rel_path)
		VALUES
			('72000000000000000000000000000000', ?, ?, 'placed', 'pin_lun', 'luns/db.img')`,
		nsID, tier0ID)
	if err != nil {
		t.Fatalf("insert lun object: %v", err)
	}

	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinLUN,
			RelPath:  "luns/db.img",
		},
	}

	_, err = BuildQuiescedLUNMovementPlan(
		context.Background(), sqlDB, client, pool, oid, tier1ID)
	if !errors.Is(err, ErrLUNQuiesceRequired) {
		t.Fatalf("error = %v, want ErrLUNQuiesceRequired", err)
	}
}

func TestBuildQuiescedLUNMovementPlanRequiresLUNRecord(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir()},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir()},
		},
	}

	var oid [OIDLen]byte
	oid[0] = 0x73
	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, pin_state, rel_path)
		VALUES
			('73000000000000000000000000000000', ?, ?, 'placed', 'none', 'luns/db.img')`,
		nsID, tier0ID)
	if err != nil {
		t.Fatalf("insert non-lun object: %v", err)
	}

	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  "luns/db.img",
		},
	}

	_, err = BuildQuiescedLUNMovementPlan(
		context.Background(), sqlDB, client, pool, oid, tier1ID)
	if !errors.Is(err, ErrLUNRecordRequired) {
		t.Fatalf("error = %v, want ErrLUNRecordRequired", err)
	}
}

func TestPrepareQuiescedLUNMovementPlanClearsPinAndBuildsPlan(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir()},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir()},
		},
	}

	var oid [OIDLen]byte
	oid[0] = 0x74
	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, pin_state, rel_path)
		VALUES
			('74000000000000000000000000000000', ?, ?, 'placed', 'pin_lun', 'luns/db.img')`,
		nsID, tier0ID)
	if err != nil {
		t.Fatalf("insert lun object: %v", err)
	}

	origRemoveXattr := removeXattr
	origSetXattr := setXattr
	t.Cleanup(func() {
		removeXattr = origRemoveXattr
		setXattr = origSetXattr
	})

	var clearedPath string
	var repinned bool
	removeXattr = func(path, name string) error {
		clearedPath = path
		return nil
	}
	setXattr = func(string, string, []byte, int) error {
		repinned = true
		return nil
	}

	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  "luns/db.img",
		},
	}
	plan, err := PrepareQuiescedLUNMovementPlan(
		context.Background(), sqlDB, client, pool, oid, "/mnt/pool/luns/db.img", tier1ID)
	if err != nil {
		t.Fatalf("PrepareQuiescedLUNMovementPlan: %v", err)
	}
	if clearedPath != "/mnt/pool/luns/db.img" {
		t.Fatalf("cleared path = %q, want backing file path", clearedPath)
	}
	if repinned {
		t.Fatal("successful plan preparation should not re-pin before movement")
	}
	if !plan.RePinLUN {
		t.Fatal("plan should request LUN re-pin")
	}
}

func TestPrepareQuiescedLUNMovementPlanRePinsOnPlanFailure(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0ID, tier1ID := seedPool(t, sqlDB)
	pool := &Pool{
		UUID:        uuid.MustParse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
		Name:        "pool-a",
		NamespaceID: nsID,
		Tiers: []TierInfo{
			{Rank: 0, TargetID: tier0ID, LowerDir: t.TempDir()},
			{Rank: 1, TargetID: tier1ID, LowerDir: t.TempDir()},
		},
	}

	var oid [OIDLen]byte
	oid[0] = 0x75
	_, err := sqlDB.Exec(`
		INSERT INTO smoothfs_objects
			(object_id, namespace_id, current_tier_id, movement_state, pin_state, rel_path)
		VALUES
			('75000000000000000000000000000000', ?, ?, 'placed', 'pin_lun', 'luns/db.img')`,
		nsID, tier0ID)
	if err != nil {
		t.Fatalf("insert lun object: %v", err)
	}

	origRemoveXattr := removeXattr
	origSetXattr := setXattr
	t.Cleanup(func() {
		removeXattr = origRemoveXattr
		setXattr = origSetXattr
	})

	var repinPath string
	removeXattr = func(string, string) error {
		return nil
	}
	setXattr = func(path, name string, value []byte, flags int) error {
		repinPath = path
		if name != lunPinXattr {
			t.Fatalf("re-pin xattr = %q, want %q", name, lunPinXattr)
		}
		if len(value) != 1 || value[0] != 1 {
			t.Fatalf("re-pin value = %v, want [1]", value)
		}
		return nil
	}

	client := &fakeMovementClient{
		inspectResult: &InspectResult{
			PinState: PinNone,
			RelPath:  "luns/db.img",
		},
	}
	_, err = PrepareQuiescedLUNMovementPlan(
		context.Background(), sqlDB, client, pool, oid, "/mnt/pool/luns/db.img", "missing-tier")
	if !errors.Is(err, ErrDestinationTierBad) {
		t.Fatalf("error = %v, want ErrDestinationTierBad", err)
	}
	if repinPath != "/mnt/pool/luns/db.img" {
		t.Fatalf("re-pin path = %q, want backing file path", repinPath)
	}
}
