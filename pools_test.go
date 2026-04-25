package smoothfs

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/uuid"
)

func TestValidatePoolName(t *testing.T) {
	ok := []string{"tank", "a", "pool0", "fast-and-slow", "pool.v2", "abc_123"}
	bad := []string{
		"",
		"Tank",
		"-leading",
		".leading",
		"_leading",
		"has space",
		"has/slash",
		"has:colon",
		"has,comma",
		strings.Repeat("a", 64),
	}
	for _, n := range ok {
		if err := ValidatePoolName(n); err != nil {
			t.Errorf("ValidatePoolName(%q) = %v, want nil", n, err)
		}
	}
	for _, n := range bad {
		if err := ValidatePoolName(n); err == nil {
			t.Errorf("ValidatePoolName(%q) = nil, want error", n)
		}
	}
}

func TestValidateTiers(t *testing.T) {
	dir := t.TempDir()
	fast := filepath.Join(dir, "fast")
	slow := filepath.Join(dir, "slow")
	notdir := filepath.Join(dir, "not-a-dir.txt")
	for _, d := range []string{fast, slow} {
		if err := os.Mkdir(d, 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", d, err)
		}
	}
	if err := os.WriteFile(notdir, []byte("x"), 0o644); err != nil {
		t.Fatalf("seed file: %v", err)
	}

	cases := []struct {
		name    string
		tiers   []string
		wantErr bool
	}{
		{"two valid", []string{fast, slow}, false},
		{"one valid", []string{fast}, false},
		{"empty", nil, true},
		{"relative", []string{"./fast"}, true},
		{"missing", []string{filepath.Join(dir, "absent")}, true},
		{"file not dir", []string{notdir}, true},
		{"duplicate", []string{fast, fast}, true},
		{"contains colon", []string{"/tmp:with:colons"}, true},
		{"contains comma", []string{"/tmp,comma"}, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := ValidateTiers(tc.tiers)
			if tc.wantErr && err == nil {
				t.Fatalf("expected error")
			}
			if !tc.wantErr && err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
		})
	}
}

func TestUnitFilenameFor(t *testing.T) {
	cases := map[string]string{
		"/mnt/smoothfs/tank":              "mnt-smoothfs-tank.mount",
		"/mnt/smoothfs/pool0":             "mnt-smoothfs-pool0.mount",
		"/var/lib/fast":                   "var-lib-fast.mount",
		"/mnt/.tierd-backing/media/HDD":   "mnt-.tierd\\x2dbacking-media-HDD.mount",
		"/mnt/.tierd-backing/media/NVME/": "mnt-.tierd\\x2dbacking-media-NVME.mount",
	}
	for input, want := range cases {
		if got := UnitFilenameFor(input); got != want {
			t.Errorf("UnitFilenameFor(%q) = %q, want %q", input, got, want)
		}
	}
}

func TestRenderMountUnit(t *testing.T) {
	dir := t.TempDir()
	fast := filepath.Join(dir, "fast")
	slow := filepath.Join(dir, "slow")
	for _, d := range []string{fast, slow} {
		if err := os.Mkdir(d, 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", d, err)
		}
	}
	p := ManagedPool{
		Name:       "tank",
		UUID:       uuid.MustParse("00000000-0000-0000-0000-000000000001"),
		Tiers:      []string{fast, slow},
		Mountpoint: "/mnt/smoothfs/tank",
	}
	body, err := RenderMountUnit(p)
	if err != nil {
		t.Fatalf("RenderMountUnit: %v", err)
	}

	mustContain := []string{
		"[Unit]",
		"[Mount]",
		"[Install]",
		"Description=smoothfs pool tank",
		"Where=/mnt/smoothfs/tank",
		"Type=smoothfs",
		"Options=pool=tank,uuid=00000000-0000-0000-0000-000000000001,tiers=" + fast + ":" + slow,
		"What=none",
		"WantedBy=local-fs.target",
	}
	for _, s := range mustContain {
		if !strings.Contains(body, s) {
			t.Errorf("unit body missing %q\n---\n%s", s, body)
		}
	}
	for _, tier := range []string{fast, slow} {
		unitName := UnitFilenameFor(tier)
		if !strings.Contains(body, "Requires="+unitName) {
			t.Errorf("unit body missing Requires=%s\n---\n%s", unitName, body)
		}
		if !strings.Contains(body, "After="+unitName) {
			t.Errorf("unit body missing After=%s\n---\n%s", unitName, body)
		}
	}
}

func TestRenderMountUnitRejectsInvalidName(t *testing.T) {
	dir := t.TempDir()
	tier := filepath.Join(dir, "t")
	if err := os.Mkdir(tier, 0o755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	p := ManagedPool{
		Name:       "Has Space",
		UUID:       uuid.New(),
		Tiers:      []string{tier},
		Mountpoint: "/mnt/smoothfs/bad",
	}
	if _, err := RenderMountUnit(p); err == nil {
		t.Fatal("expected error on invalid pool name")
	}
}

func TestMountpointForPool(t *testing.T) {
	if got := MountpointForPool("", "tank"); got != "/mnt/smoothfs/tank" {
		t.Errorf("default base: got %q", got)
	}
	if got := MountpointForPool("/srv/smoothfs", "tank"); got != "/srv/smoothfs/tank" {
		t.Errorf("custom base: got %q", got)
	}
}
