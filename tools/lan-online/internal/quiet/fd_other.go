//go:build !linux

package quiet

import "os"

// redirectFD1 is a no-op off Linux; os.Stdout replacement still applies.
func redirectFD1(*os.File) (func(), error) { return func() {}, nil }
