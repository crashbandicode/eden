package natcheck

import (
	"net/netip"
	"strconv"
)

func itoa(i int) string { return strconv.Itoa(i) }

func mustAddrPort(s string) netip.AddrPort { return netip.MustParseAddrPort(s) }
