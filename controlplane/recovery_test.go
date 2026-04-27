package controlplane

import (
	"context"
	"database/sql"
	"encoding/hex"
	"testing"
)

func seedMidMove(t *testing.T, sqlDB *sql.DB, nsID, curTier, intendedTier string,
	state MovementState, seq uint64, oid [OIDLen]byte) {
	t.Helper()
	var intended any
	if intendedTier != "" {
		intended = intendedTier
	}
	_, err := sqlDB.Exec(`INSERT INTO smoothfs_objects
	        (object_id, namespace_id, current_tier_id, intended_tier_id,
	         movement_state, transaction_seq)
	        VALUES (?, ?, ?, ?, ?, ?)`,
		hex.EncodeToString(oid[:]), nsID, curTier, intended, string(state), seq)
	if err != nil {
		t.Fatalf("seed mid-move: %v", err)
	}
}

func readObjectState(t *testing.T, sqlDB *sql.DB, oid [OIDLen]byte) (
	current, intended sql.NullString, state string, lastCommittedGen int64, reason string) {
	t.Helper()
	err := sqlDB.QueryRow(`
		SELECT current_tier_id, intended_tier_id, movement_state,
		       last_committed_cutover_gen, failure_reason
		  FROM smoothfs_objects WHERE object_id = ?`,
		hex.EncodeToString(oid[:])).Scan(
		&current, &intended, &state, &lastCommittedGen, &reason)
	if err != nil {
		t.Fatalf("read object: %v", err)
	}
	return
}

func TestRecoverRollsBackPreCutover(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	preCutoverStates := []MovementState{
		StatePlanAccepted,
		StateDestinationReserved,
		StateCopyInProgress,
		StateCopyComplete,
		StateCopyVerified,
	}
	oids := make([][OIDLen]byte, len(preCutoverStates))
	for i, st := range preCutoverStates {
		oids[i][0] = byte(0x10 + i)
		seedMidMove(t, sqlDB, nsID, tier0, tier1, st, uint64(i+1), oids[i])
	}

	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover: %v", err)
	}

	for i, st := range preCutoverStates {
		cur, intended, gotState, _, reason := readObjectState(t, sqlDB, oids[i])
		if gotState != string(StatePlaced) {
			t.Errorf("%s: state = %q, want %q", st, gotState, StatePlaced)
		}
		if cur.String != tier0 {
			t.Errorf("%s: current_tier = %q, want %q", st, cur.String, tier0)
		}
		if intended.Valid {
			t.Errorf("%s: intended_tier should be NULL, got %q", st, intended.String)
		}
		if reason == "" {
			t.Errorf("%s: failure_reason should be populated", st)
		}
	}
}

func TestRecoverRollsForwardPostCutover(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	postStates := []MovementState{
		StateCutoverInProgress,
		StateSwitched,
		StateCleanupInProgress,
	}
	oids := make([][OIDLen]byte, len(postStates))
	for i, st := range postStates {
		oids[i][0] = byte(0x20 + i)
		seedMidMove(t, sqlDB, nsID, tier0, tier1, st, uint64(i+1), oids[i])
	}

	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover: %v", err)
	}

	for i, st := range postStates {
		cur, intended, gotState, lastGen, reason := readObjectState(t, sqlDB, oids[i])
		if gotState != string(StatePlaced) {
			t.Errorf("%s: state = %q, want %q", st, gotState, StatePlaced)
		}
		if cur.String != tier1 {
			t.Errorf("%s: current_tier = %q, want %q (dest)", st, cur.String, tier1)
		}
		if intended.Valid {
			t.Errorf("%s: intended_tier should be NULL, got %q", st, intended.String)
		}
		if lastGen != 1 {
			t.Errorf("%s: last_committed_cutover_gen = %d, want 1", st, lastGen)
		}
		if reason == "" {
			t.Errorf("%s: failure_reason should be populated", st)
		}
	}
}

func TestRecoverLeavesTerminalStatesAlone(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, _ := seedPool(t, sqlDB)

	terminals := []MovementState{
		StatePlaced,
		StateCleanupComplete,
		StateFailed,
		StateStale,
	}
	oids := make([][OIDLen]byte, len(terminals))
	for i, st := range terminals {
		oids[i][0] = byte(0x30 + i)
		seedMidMove(t, sqlDB, nsID, tier0, "", st, uint64(i+1), oids[i])
	}

	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover: %v", err)
	}

	for i, st := range terminals {
		_, _, gotState, _, reason := readObjectState(t, sqlDB, oids[i])
		if gotState != string(st) {
			t.Errorf("%s: state = %q, want unchanged", st, gotState)
		}
		if reason != "" {
			t.Errorf("%s: terminal state should not stamp failure_reason, got %q", st, reason)
		}
	}
}

func TestRecoverPreservesFailedLUNAtDestination(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, tier1 := seedPool(t, sqlDB)

	var oid [OIDLen]byte
	oid[0] = 0x41
	seedMidMove(t, sqlDB, nsID, tier0, tier1, StateFailed, 1, oid)
	if _, err := sqlDB.Exec(`
		UPDATE smoothfs_objects
		   SET pin_state = 'pin_lun',
		       failure_reason = ?
		 WHERE object_id = ?`,
		ErrLUNResumeRequired.Error(),
		hex.EncodeToString(oid[:])); err != nil {
		t.Fatalf("mark failed lun: %v", err)
	}

	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover: %v", err)
	}

	cur, intended, gotState, lastGen, reason := readObjectState(t, sqlDB, oid)
	if gotState != string(StateFailed) {
		t.Fatalf("state = %q, want %q", gotState, StateFailed)
	}
	if cur.String != tier1 {
		t.Fatalf("current_tier = %q, want %q (dest)", cur.String, tier1)
	}
	if intended.Valid {
		t.Fatalf("intended_tier should be NULL, got %q", intended.String)
	}
	if lastGen != 0 {
		t.Fatalf("last_committed_cutover_gen = %d, want unchanged 0", lastGen)
	}
	if reason != ErrLUNResumeRequired.Error() {
		t.Fatalf("failure_reason = %q, want preserved", reason)
	}
}

func TestRecoverSkipsPostCutoverWithoutDest(t *testing.T) {
	sqlDB := testDB(t)
	nsID, tier0, _ := seedPool(t, sqlDB)

	var oid [OIDLen]byte
	oid[0] = 0x40
	seedMidMove(t, sqlDB, nsID, tier0, "", StateSwitched, 1, oid)

	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover: %v", err)
	}

	_, _, gotState, _, _ := readObjectState(t, sqlDB, oid)
	if gotState != string(StateSwitched) {
		t.Errorf("state = %q, want %q (left as-is when dest missing)", gotState, StateSwitched)
	}
}

func TestRecoverNoOpWhenEmpty(t *testing.T) {
	sqlDB := testDB(t)
	if err := Recover(context.Background(), sqlDB); err != nil {
		t.Fatalf("recover on empty DB: %v", err)
	}
}
