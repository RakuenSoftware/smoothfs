#!/bin/bash
# A freed source inode must never alias the object moved to another tier.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT=${SMOOTHFS_TEST_GO_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}
. "$SCRIPT_DIR/tier_spill_lib.sh"
SPILL_ROOT=${SMOOTHFS_INODE_REUSE_ROOT:-/tmp/smoothfs-movement-inode-reuse}
SPILL_UUID=00000000-0000-0000-0000-00000000f51d
trap spill_cleanup EXIT
spill_setup_pool inodereuse "$SPILL_UUID"
cat > "$SPILL_ROOT/probe.go" <<'GO'
package main
import (
 "bytes"
 "fmt"
 "io"
 "os"
 "path/filepath"
 "syscall"
 smoothfs "github.com/RakuenSoftware/smoothfs"
 "github.com/google/uuid"
 "golang.org/x/sys/unix"
)
func must(err error) { if err != nil { panic(err) } }
func main() {
 root:=os.Args[1]; pool:=uuid.MustParse(os.Args[2])
 upper:=filepath.Join(root,"server","victim")
 source:=filepath.Join(root,"fast","victim")
 dest:=filepath.Join(root,"slow","victim")
 payload:=bytes.Repeat([]byte("PBS chunk identity must survive inode reuse\n"),256)
 must(os.WriteFile(upper,payload,0600))
 dir,err:=os.Open(filepath.Join(root,"server"));must(err);must(dir.Sync());must(dir.Close())
 original,err:=os.Stat(source);must(err);oldIno:=original.Sys().(*syscall.Stat_t).Ino
 raw:=make([]byte,smoothfs.OIDLen);_,err=unix.Getxattr(upper,"trusted.smoothfs.oid",raw);must(err)
 var oid [smoothfs.OIDLen]byte;copy(oid[:],raw)
 client,err:=smoothfs.Open();must(err);defer client.Close()
 inspect,err:=client.Inspect(pool,oid);must(err)
 must(client.MovePlan(pool,oid,1,1))
 in,err:=os.Open(source);must(err);out,err:=os.Create(dest);must(err)
 _,err=io.Copy(out,in);must(err);must(out.Sync());must(out.Close());must(in.Close())
 before,err:=os.ReadFile(dest);must(err);if !bytes.Equal(before,payload){panic("copy differs before cutover")}
 if inspect.HasWriteSeq {must(client.MoveCutoverVerifyWriteSeq(pool,oid,1,inspect.WriteSeq))} else {must(client.MoveCutover(pool,oid,1))}
 must(os.Remove(source))
 // Hold the moved upper inode and its NEW backing file alive, while leaving
 // the unlinked source inode free for the backing filesystem to reuse.
 held,err:=os.Open(upper);must(err);defer held.Close()
 reused:=false
 for n:=0;n<16384;n++ {
  path:=filepath.Join(root,"server","replacement")
  must(os.WriteFile(path,[]byte("unrelated RRD journal data\n"),0600))
  info,err:=os.Stat(filepath.Join(root,"fast","replacement"));must(err)
  if info.Sys().(*syscall.Stat_t).Ino==oldIno {
   reused=true
   actual,err:=os.ReadFile(dest);must(err)
   if !bytes.Equal(actual,payload){panic("source inode reuse redirected an unrelated write into the moved object")}
   must(os.Remove(path));break
  }
  must(os.Remove(path))
 }
 if !reused {panic("backing filesystem did not reuse the source inode; regression was not exercised")}
 actualOID:=make([]byte,smoothfs.OIDLen);_,err=unix.Getxattr(dest,"trusted.smoothfs.oid",actualOID);must(err)
 if !bytes.Equal(actualOID,raw){panic("cutover did not preserve destination object identity")}
 fmt.Println("movement_inode_reuse: PASS (actual source inode reuse, payload and persistent identity)")
}
GO
go -C "$REPO_ROOT" run "$SPILL_ROOT/probe.go" "$SPILL_ROOT" "$SPILL_UUID"
