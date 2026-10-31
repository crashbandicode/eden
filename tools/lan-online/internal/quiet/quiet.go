// Package quiet silences the process's standard output.
//
// The pinned NEX core writes diagnostics with fmt.Printf, and some of those lines
// carry per-login source keys and ticket bytes. The fixture's own logging uses the
// log package (standard error) and prints metadata only, so the binary discards
// standard output before the core is initialised. Standard error is left alone.
package quiet

import (
	"os"
)

// SuppressStdout points os.Stdout at the null device. With dupFD it also
// redirects file descriptor 1 itself, so writes that bypass the os.Stdout
// variable are discarded as well. The returned function restores both.
func SuppressStdout(dupFD bool) (restore func(), err error) {
	devnull, err := os.OpenFile(os.DevNull, os.O_WRONLY, 0)
	if err != nil {
		return nil, err
	}
	prev := os.Stdout
	os.Stdout = devnull
	var undoFD func()
	if dupFD {
		undoFD, err = redirectFD1(devnull)
		if err != nil {
			os.Stdout = prev
			_ = devnull.Close()
			return nil, err
		}
	}
	return func() {
		if undoFD != nil {
			undoFD()
		}
		os.Stdout = prev
		_ = devnull.Close()
	}, nil
}
