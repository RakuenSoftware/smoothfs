package controlplane

import (
	"os"
	"path/filepath"
	"testing"
)

// TestCopyWithChecksum_CreatesDurableDestChain verifies the move copy
// materialises the full destination directory chain and file (each level is
// also fsynced for crash-safety; fsync itself is exercised implicitly). A
// promotion must never remove the source until the destination namespace is
// durable, so the copy is responsible for creating + syncing it.
func TestCopyWithChecksum_CreatesDurableDestChain(t *testing.T) {
	dir := t.TempDir()
	srcPath := filepath.Join(dir, "src", "obj.bin")
	if err := os.MkdirAll(filepath.Dir(srcPath), 0o755); err != nil {
		t.Fatal(err)
	}
	want := []byte("hello smoothfs durable move")
	if err := os.WriteFile(srcPath, want, 0o644); err != nil {
		t.Fatal(err)
	}
	// Deep, not-yet-existing destination chain (as a fresh promotion would hit).
	dstPath := filepath.Join(dir, "dst", "video", "Show", "Season 01", "obj.bin")

	w := &Worker{}
	sum, err := w.copyWithChecksum(srcPath, dstPath)
	if err != nil {
		t.Fatalf("copyWithChecksum: %v", err)
	}
	got, err := os.ReadFile(dstPath)
	if err != nil {
		t.Fatalf("destination not created durably: %v", err)
	}
	if string(got) != string(want) {
		t.Fatalf("destination content mismatch")
	}
	if again, err := fileSHA256(dstPath); err != nil || again != sum {
		t.Fatalf("checksum mismatch: err=%v", err)
	}
}

func TestMkdirAllSync_RejectsFileInPath(t *testing.T) {
	dir := t.TempDir()
	fp := filepath.Join(dir, "afile")
	if err := os.WriteFile(fp, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := mkdirAllSync(filepath.Join(fp, "sub"), 0o755); err == nil {
		t.Fatal("expected error creating dir under a file, got nil")
	}
}
