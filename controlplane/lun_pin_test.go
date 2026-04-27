package controlplane

import (
	"errors"
	"testing"

	"golang.org/x/sys/unix"
)

func TestClearLUNPinRemovesSmoothfsXattr(t *testing.T) {
	origRemoveXattr := removeXattr
	t.Cleanup(func() { removeXattr = origRemoveXattr })

	var gotPath, gotName string
	removeXattr = func(path, name string) error {
		gotPath = path
		gotName = name
		return nil
	}

	if err := ClearLUNPin("/mnt/pool/luns/db.img"); err != nil {
		t.Fatalf("ClearLUNPin: %v", err)
	}
	if gotPath != "/mnt/pool/luns/db.img" {
		t.Fatalf("path = %q, want backing file path", gotPath)
	}
	if gotName != lunPinXattr {
		t.Fatalf("xattr = %q, want %q", gotName, lunPinXattr)
	}
}

func TestClearLUNPinTreatsMissingXattrAsSuccess(t *testing.T) {
	origRemoveXattr := removeXattr
	t.Cleanup(func() { removeXattr = origRemoveXattr })

	removeXattr = func(string, string) error {
		return unix.ENODATA
	}

	if err := ClearLUNPin("/mnt/pool/luns/db.img"); err != nil {
		t.Fatalf("ClearLUNPin missing xattr: %v", err)
	}
}

func TestClearLUNPinWrapsUnexpectedError(t *testing.T) {
	origRemoveXattr := removeXattr
	t.Cleanup(func() { removeXattr = origRemoveXattr })

	want := unix.EPERM
	removeXattr = func(string, string) error {
		return want
	}

	err := ClearLUNPin("/mnt/pool/luns/db.img")
	if !errors.Is(err, want) {
		t.Fatalf("ClearLUNPin error = %v, want wrapped %v", err, want)
	}
}
