package controlplane

import (
	"errors"
	"fmt"

	"golang.org/x/sys/unix"
)

const lunPinXattr = "trusted.smoothfs.lun"

var (
	setXattr    = unix.Setxattr
	removeXattr = unix.Removexattr
)

func setLUNPin(path string) error {
	if err := setXattr(path, lunPinXattr, []byte{1}, 0); err != nil {
		return fmt.Errorf("set %s on %s: %w", lunPinXattr, path, err)
	}
	return nil
}

// ClearLUNPin clears the smoothfs LUN pin xattr after the target path has been
// administratively quiesced. A missing xattr is already unpinned and is treated
// as success so retrying quiesce cleanup is idempotent.
func ClearLUNPin(path string) error {
	if err := removeXattr(path, lunPinXattr); err != nil {
		if errors.Is(err, unix.ENODATA) {
			return nil
		}
		return fmt.Errorf("remove %s on %s: %w", lunPinXattr, path, err)
	}
	return nil
}
