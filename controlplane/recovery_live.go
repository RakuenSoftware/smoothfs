package controlplane

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"syscall"

	"golang.org/x/sys/unix"

	"github.com/google/uuid"
)

type movementRollbackClient interface {
	MoveRollback(uuid.UUID, [OIDLen]byte, uint64) error
}

type interruptedObject struct {
	oid, namespace, source, destination, state, pin, rel string
	seq                                                  uint64
}

// The database transition can lag the kernel cutover or completion ack. Never
// choose which copy to delete, or report PLACED, using the SQL state alone.
func recoverLive(ctx context.Context, db *sql.DB, client movementClient) error {
	rows, err := db.QueryContext(ctx, `SELECT object_id, namespace_id, current_tier_id,
 COALESCE(intended_tier_id,''), movement_state, pin_state, rel_path, transaction_seq
 FROM smoothfs_objects WHERE movement_state NOT IN ('placed','cleanup_complete','stale')`)
	if err != nil {
		return fmt.Errorf("scan in-flight movements: %w", err)
	}
	var pending []interruptedObject
	for rows.Next() {
		var p interruptedObject
		if err = rows.Scan(&p.oid, &p.namespace, &p.source, &p.destination, &p.state, &p.pin, &p.rel, &p.seq); err != nil {
			rows.Close()
			return err
		}
		pending = append(pending, p)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	var failures []error
	for _, p := range pending {
		if err = recoverObject(ctx, db, client, p); err != nil {
			failures = append(failures, fmt.Errorf("recover %s: %w", p.oid, err))
		}
	}
	return errors.Join(failures...)
}

func recoverObject(ctx context.Context, db *sql.DB, client movementClient, p interruptedObject) error {
	if client == nil {
		return errors.New("kernel client unavailable")
	}
	raw, err := hex.DecodeString(p.oid)
	if err != nil || len(raw) != OIDLen {
		return errors.New("invalid object id")
	}
	var oid [OIDLen]byte
	copy(oid[:], raw)
	var backend, domain, exposed string
	if err = db.QueryRowContext(ctx, `SELECT backend_ref,placement_domain,exposed_path FROM managed_namespaces WHERE id=? AND backend_kind='smoothfs'`, p.namespace).Scan(&backend, &domain, &exposed); err != nil {
		return err
	}
	pool, err := uuid.Parse(backend)
	if err != nil {
		return fmt.Errorf("namespace has no pool UUID: %w", err)
	}
	ins, err := client.Inspect(pool, oid)
	if errors.Is(err, syscall.ENOENT) {
		// An asynchronous path index may not contain a recently moved object after
		// reboot. Instantiate its persisted name, then confirm the same OID through
		// the target pool's INSPECT before selecting or deleting any lower copy.
		if err = warmRecoveryObject(exposed, p.rel, oid); err != nil {
			return fmt.Errorf("instantiate interrupted object: %w", err)
		}
		log.Printf("smoothfs: resolved recovery path for object %s", p.oid)
		ins, err = client.Inspect(pool, oid)
	}
	if err != nil {
		return fmt.Errorf("inspect: %w", err)
	}
	if ins == nil {
		return errors.New("inspect returned no object")
	}
	if ins.MovementState != StatePlaced && ins.TransactionSeq != p.seq {
		return errors.New("kernel transaction differs from interrupted movement")
	}
	rel := ins.RelPath
	if rel == "" {
		rel = p.rel
	}
	if rel == "" || filepath.IsAbs(rel) || filepath.Clean(rel) != rel || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(os.PathSeparator)) {
		return errors.New("invalid recovery relative path")
	}
	type tier struct {
		id, lower string
		rank      uint8
	}
	tiers := map[string]tier{}
	var current tier
	rows, err := db.QueryContext(ctx, `SELECT id,rank,backing_ref FROM tier_targets WHERE placement_domain=? AND backend_kind='smoothfs'`, domain)
	if err != nil {
		return err
	}
	for rows.Next() {
		var t tier
		if err = rows.Scan(&t.id, &t.rank, &t.lower); err != nil {
			rows.Close()
			return err
		}
		if !filepath.IsAbs(t.lower) {
			rows.Close()
			return errors.New("tier has no absolute backing path")
		}
		tiers[t.id] = t
		if t.rank == ins.CurrentTier {
			current = t
		}
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return err
	}
	if current.id == "" {
		return errors.New("kernel tier is not in namespace")
	}
	// A failed retry or older database-only recovery may have cleared intended.
	// Its durable movement log still identifies the two paths for this sequence.
	source, destination := p.source, p.destination
	if destination == "" {
		err = db.QueryRowContext(ctx, `SELECT source_tier,dest_tier FROM smoothfs_movement_log WHERE object_id=? AND transaction_seq=? ORDER BY id DESC LIMIT 1`, p.oid, p.seq).Scan(&source, &destination)
		if err != nil && err != sql.ErrNoRows {
			return err
		}
	}
	if destination == "" || source == destination {
		return errors.New("interrupted movement has no distinct recorded tiers")
	}
	if _, ok := tiers[source]; !ok {
		return errors.New("recorded source tier is unavailable")
	}
	if _, ok := tiers[destination]; !ok {
		return errors.New("recorded destination tier is unavailable")
	}
	if current.id != source && current.id != destination {
		return errors.New("kernel placement is outside recorded movement")
	}
	authoritative := filepath.Join(current.lower, rel)
	live, err := os.Lstat(authoritative)
	if err != nil {
		return fmt.Errorf("authoritative copy: %w", err)
	}
	if !live.Mode().IsRegular() {
		return errors.New("authoritative copy is not regular")
	}
	other := destination
	if current.id == destination {
		other = source
	}
	otherPath := filepath.Join(tiers[other].lower, rel)
	info, err := os.Lstat(otherPath)
	if err == nil {
		if !info.Mode().IsRegular() || os.SameFile(info, live) {
			return errors.New("cleanup path is not a distinct regular copy")
		}
	} else if !os.IsNotExist(err) {
		return err
	}
	// Validate the phase before touching either path. A switched kernel wins
	// even if the process died before recording the SQL 'switched' transition.
	switch ins.MovementState {
	case StateSwitched:
		if current.id != destination {
			return errors.New("switched object is not on recorded destination")
		}
		if _, ok := client.(movementCompletionClient); !ok {
			return errors.New("kernel completion acknowledgement unavailable")
		}
	case StatePlanAccepted, StateDestinationReserved, StateCopyInProgress, StateCopyComplete, StateCopyVerified, StateFailed:
		if current.id != source {
			return errors.New("pre-cutover object is not on recorded source")
		}
		if _, ok := client.(movementRollbackClient); !ok {
			return errors.New("kernel rollback acknowledgement unavailable")
		}
	case StatePlaced:
		// Mount replay or an ack preceding the final SQL commit already settled it.
	default:
		return fmt.Errorf("kernel movement is still %s", ins.MovementState)
	}
	if err == nil {
		if err = os.Remove(otherPath); err != nil {
			return fmt.Errorf("remove interrupted copy: %w", err)
		}
	}
	// The other name may already be absent after a crash. Still issue its parent
	// barrier before telling the kernel or database that cleanup is complete.
	parent := filepath.Dir(otherPath)
	for {
		err = syncDir(parent)
		if !os.IsNotExist(err) || parent == filepath.Clean(tiers[other].lower) {
			break
		}
		parent = filepath.Dir(parent)
	}
	if err != nil {
		return fmt.Errorf("sync interrupted cleanup: %w", err)
	}
	switch ins.MovementState {
	case StateSwitched:
		err = client.(movementCompletionClient).MoveComplete(pool, oid, ins.TransactionSeq)
	case StatePlaced:
	default:
		err = client.(movementRollbackClient).MoveRollback(pool, oid, ins.TransactionSeq)
	}
	if err != nil {
		return fmt.Errorf("kernel recovery acknowledgement: %w", err)
	}
	settled, err := client.Inspect(pool, oid)
	if err != nil {
		return err
	}
	if settled == nil || settled.MovementState != StatePlaced || settled.CurrentTier != ins.CurrentTier {
		return errors.New("kernel recovery did not settle on authoritative tier")
	}
	if p.pin == string(PinLUN) {
		if err = restoreLUNPin(ctx, db, p.namespace, current.id, rel); err != nil {
			return err
		}
	}
	state := string(StatePlaced)
	if p.pin == string(PinLUN) && p.state == string(StateFailed) {
		state = string(StateFailed)
	}
	_, err = db.ExecContext(ctx, `UPDATE smoothfs_objects SET current_tier_id=?,intended_tier_id=NULL,
 movement_state=?,rel_path=?,last_movement_at=CASE WHEN ? THEN datetime('now') ELSE last_movement_at END,
 last_committed_cutover_gen=MAX(last_committed_cutover_gen,?),
 failure_reason=CASE WHEN ?='failed' THEN failure_reason ELSE 'recovered: kernel placement and cleanup confirmed' END,
 updated_at=datetime('now') WHERE object_id=?`, current.id, state, rel, current.id == destination, settled.Generation, state, p.oid)
	return err
}

func warmRecoveryObject(exposed, rel string, oid [OIDLen]byte) error {
	if !filepath.IsAbs(exposed) || rel == "" || filepath.IsAbs(rel) || filepath.Clean(rel) != rel || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(os.PathSeparator)) {
		return errors.New("namespace has no usable exposed recovery path")
	}
	exposed, err := filepath.EvalSymlinks(exposed)
	if err != nil {
		return err
	}
	mounts, err := os.ReadFile("/proc/self/mountinfo")
	if err != nil {
		return err
	}
	mount := recoveryMountpoint(string(mounts), exposed)
	if mount == "" {
		return errors.New("namespace is not below a mounted smoothfs root")
	}
	path := filepath.Join(mount, rel)
	if _, err = os.Lstat(path); err != nil {
		return err
	}
	raw := make([]byte, OIDLen)
	n, err := unix.Getxattr(path, "trusted.smoothfs.oid", raw)
	if err != nil {
		return err
	}
	if n != OIDLen || !bytes.Equal(raw, oid[:]) {
		return errors.New("persisted recovery path belongs to another object")
	}
	return nil
}

func recoveryMountpoint(mounts, exposed string) string {
	decode := strings.NewReplacer(`\040`, " ", `\011`, "\t", `\012`, "\n", `\134`, `\`)
	best := ""
	for _, line := range strings.Split(mounts, "\n") {
		before, after, ok := strings.Cut(line, " - ")
		if !ok {
			continue
		}
		fields, fs := strings.Fields(before), strings.Fields(after)
		if len(fields) < 5 || len(fs) < 1 || fs[0] != "smoothfs" || fields[3] != "/" {
			continue
		}
		path := decode.Replace(fields[4])
		if exposed != path && !strings.HasPrefix(exposed, path+string(os.PathSeparator)) {
			continue
		}
		if len(path) > len(best) {
			best = path
		}
	}
	return best
}
