//go:build linux

package quiet

import (
	"os"
	"syscall"
)

// redirectFD1 duplicates target onto file descriptor 1 and returns an undo.
func redirectFD1(target *os.File) (func(), error) {
	saved, err := syscall.Dup(1)
	if err != nil {
		return nil, err
	}
	if err := syscall.Dup3(int(target.Fd()), 1, 0); err != nil {
		_ = syscall.Close(saved)
		return nil, err
	}
	return func() {
		_ = syscall.Dup3(saved, 1, 0)
		_ = syscall.Close(saved)
	}, nil
}
