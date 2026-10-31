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
  must(c.MovePlan(pool,oid,uint8(to),seq))
  if e=c.MoveComplete(pool,oid,seq);!errors.Is(e,unix.EBUSY){panic(fmt.Sprintf("premature completion: %v",e))}
  src:=filepath.Join(root,tiers[from],"chunk");dst:=filepath.Join(root,tiers[to],"chunk")
  in,e:=os.Open(src);must(e);out,e:=os.Create(dst);must(e)
  _,e=io.Copy(out,in);must(e);must(out.Sync());must(out.Close());must(in.Close())
  if before.HasWriteSeq{must(c.MoveCutoverVerifyWriteSeq(pool,oid,seq,before.WriteSeq))}else{must(c.MoveCutover(pool,oid,seq))}
  if e=c.MovePlan(pool,oid,uint8(from),seq+100);!errors.Is(e,unix.EBUSY){panic(fmt.Sprintf("new movement before cleanup ack: %v",e))}
  if e=c.MoveComplete(pool,oid,seq+1);!errors.Is(e,unix.ESTALE){panic(fmt.Sprintf("stale completion: %v",e))}
  must(os.Remove(src));dir,e:=os.Open(filepath.Dir(src));must(e);must(dir.Sync());must(dir.Close())
  must(c.MoveComplete(pool,oid,seq));must(c.MoveComplete(pool,oid,seq))
  after,e:=c.Inspect(pool,oid);must(e)
  if after.CurrentTier!=uint8(to)||after.MovementState!=smoothfs.StatePlaced{panic("cleanup did not restore placed state")}
  _,e=held.Seek(0,0);must(e);actual,e:=io.ReadAll(held);must(e)
  if !bytes.Equal(actual,payload){panic("held file changed after roundtrip")}
 }
 fmt.Println("movement_roundtrip: PASS (four moves, held descriptor, premature/stale/idempotent acknowledgements)")
}
GO
go -C "$REPO_ROOT" run "$SPILL_ROOT/probe.go" "$SPILL_ROOT" "$SPILL_UUID"
