package controlplane

import (
	"fmt"

	"golang.org/x/sys/unix"
)

const lunPinXattr = "trusted.smoothfs.lun"

func setLUNPin(path string) error {
	if err := unix.Setxattr(path, lunPinXattr, []byte{1}, 0); err != nil {
		return fmt.Errorf("set %s on %s: %w", lunPinXattr, path, err)
	}
	return nil
}
