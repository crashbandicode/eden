//go:build !linux && !windows

// Package autolan is the automatic LAN runtime. It supports Linux and Windows
// (AF_UNIX peer identity, parent tracking); other platforms report that.
package autolan

import (
	"log"
	"os"
)

// Main reports that automatic mode is unsupported on this platform.
func Main(args []string) int {
	log.New(os.Stderr, "", log.LstdFlags).Printf("[auto] unsupported_platform: the automatic LAN runtime requires Linux or Windows")
	return 1
}
