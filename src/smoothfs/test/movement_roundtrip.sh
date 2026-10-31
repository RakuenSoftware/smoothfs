#!/bin/bash
# Successful cleanup must permit another movement without inode eviction/reboot.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT=${SMOOTHFS_TEST_GO_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_ROUNDTRIP_ROOT:-/tmp/smoothfs-movement-roundtrip}
SPILL_UUID=00000000-0000-0000-0000-00000000f51e
trap spill_cleanup EXIT
spill_setup_pool roundtrip "$SPILL_UUID"
cat > "$SPILL_ROOT/probe.go" <<'GO'
package main
import (
 "bytes"
 "errors"
 "fmt"
 "io"
 "os"
 "path/filepath"
 smoothfs "github.com/RakuenSoftware/smoothfs"
 "github.com/google/uuid"
 "golang.org/x/sys/unix"
)
func must(e error){if e!=nil{panic(e)}}
func main(){
 root:=os.Args[1];pool:=uuid.MustParse(os.Args[2]);upper:=filepath.Join(root,"server","chunk")
 payload:=bytes.Repeat([]byte("PBS repeated movement sentinel\n"),8192)
 must(os.WriteFile(upper,payload,0600))
 held,e:=os.Open(upper);must(e);defer held.Close()
 raw:=make([]byte,smoothfs.OIDLen);_,e=unix.Getxattr(upper,"trusted.smoothfs.oid",raw);must(e)
 var oid [smoothfs.OIDLen]byte;copy(oid[:],raw)
 c,e:=smoothfs.Open();must(e);defer c.Close()
 tiers:=[]string{"fast","slow"}
 for round:=0;round<4;round++{
  from,to:=round%2,1-round%2;seq:=uint64(500+round)
  before,e:=c.Inspect(pool,oid);must(e)
  if before.CurrentTier!=uint8(from)||before.MovementState!=smoothfs.StatePlaced{panic("object was not ready for the next movement")}
  // A crash before cutover must discard only the partial destination and
  // restore movement readiness on the original source.
  rollbackSeq:=seq+1000
  must(c.MovePlan(pool,oid,uint8(to),rollbackSeq))
  if e=c.MoveRollback(pool,oid,rollbackSeq+1);!errors.Is(e,unix.ESTALE){panic(fmt.Sprintf("stale rollback: %v",e))}
  partial:=filepath.Join(root,tiers[to],"chunk")
  must(os.WriteFile(partial,payload[:128],0600));must(os.Remove(partial))
  parent,e:=os.Open(filepath.Dir(partial));must(e);must(parent.Sync());must(parent.Close())
  must(c.MoveRollback(pool,oid,rollbackSeq));must(c.MoveRollback(pool,oid,rollbackSeq))
  rolledBack,e:=c.Inspect(pool,oid);must(e)
  if rolledBack.CurrentTier!=uint8(from)||rolledBack.MovementState!=smoothfs.StatePlaced{panic("rollback changed authoritative placement")}
  must(c.MovePlan(pool,oid,uint8(to),seq))
  if e=c.MoveComplete(pool,oid,seq);!errors.Is(e,unix.EBUSY){panic(fmt.Sprintf("premature completion: %v",e))}
  src:=filepath.Join(root,tiers[from],"chunk");dst:=filepath.Join(root,tiers[to],"chunk")
  in,e:=os.Open(src);must(e);out,e:=os.Create(dst);must(e)
  _,e=io.Copy(out,in);must(e);must(out.Sync());must(out.Close());must(in.Close())
  planned,e:=c.Inspect(pool,oid);must(e)
  fmt.Printf("round=%d seq=%d kernel_seq=%d write_before=%d write_after_rollback=%d write_planned=%d\n",round,seq,planned.TransactionSeq,before.WriteSeq,rolledBack.WriteSeq,planned.WriteSeq)
  if rolledBack.HasWriteSeq{must(c.MoveCutoverVerifyWriteSeq(pool,oid,seq,rolledBack.WriteSeq))}else{must(c.MoveCutover(pool,oid,seq))}
  if e=c.MoveRollback(pool,oid,seq);!errors.Is(e,unix.EBUSY){panic(fmt.Sprintf("rollback accepted after cutover: %v",e))}
  if e=c.MovePlan(pool,oid,uint8(from),seq+100);!errors.Is(e,unix.EBUSY){panic(fmt.Sprintf("new movement before cleanup ack: %v",e))}
  if e=c.MoveComplete(pool,oid,seq+1);!errors.Is(e,unix.ESTALE){panic(fmt.Sprintf("stale completion: %v",e))}
  must(os.Remove(src));dir,e:=os.Open(filepath.Dir(src));must(e);must(dir.Sync());must(dir.Close())
  must(c.MoveComplete(pool,oid,seq));must(c.MoveComplete(pool,oid,seq))
  after,e:=c.Inspect(pool,oid);must(e)
  if after.CurrentTier!=uint8(to)||after.MovementState!=smoothfs.StatePlaced{panic("cleanup did not restore placed state")}
  _,e=held.Seek(0,0);must(e);actual,e:=io.ReadAll(held);must(e)
  if !bytes.Equal(actual,payload){panic("held file changed after roundtrip")}
 }
 fmt.Println("movement_roundtrip: PASS (four moves and rollbacks, held descriptor, premature/stale/idempotent acknowledgements)")
}
GO
go -C "$REPO_ROOT" run "$SPILL_ROOT/probe.go" "$SPILL_ROOT" "$SPILL_UUID"
