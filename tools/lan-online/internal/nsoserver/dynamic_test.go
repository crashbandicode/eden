package nsoserver

import (
	"net/netip"
	"strconv"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"

	"edennsolan/backend/internal/fixture"
)

func newDynamicServer(t *testing.T) *Server {
	t.Helper()
	// The shared loginEx helper asserts the fixture advertise address.
	cfg, err := fixture.NewDynamic("192.168.50.10", "")
	if err != nil {
		t.Fatal(err)
	}
	s, err := New(cfg, (&logSink{}).logf)
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestDynamicServerResolvesOnlyLeasedIdentities(t *testing.T) {
	s := newDynamicServer(t)
	if pid, ct, retval := loginEx(t, s, "305419896", "10.253.1.2:1024"); retval != (nex.ResultAuthTokenParseError|nex.ResultErrorMask) || pid != 0 || ct != nil {
		t.Fatalf("unleased username accepted: retval=%#x pid=%d", retval, pid)
	}
	if err := s.Resolver.Add("305419896", 305419896); err != nil {
		t.Fatal(err)
	}
	if pid, ct, retval := loginEx(t, s, "305419896", "10.253.1.2:1025"); retval&nex.ResultErrorMask != 0 || pid != 305419896 || ct == nil {
		t.Fatalf("leased username refused: retval=%#x pid=%d", retval, pid)
	}
	s.Resolver.Remove("305419896")
	if pid, _, _ := loginEx(t, s, "305419896", "10.253.1.2:1026"); pid != 0 {
		t.Fatal("identity still accepted after its lease ended")
	}
}

func TestDynamicStationsUseExplicitPIDTable(t *testing.T) {
	s := newDynamicServer(t)
	for _, pid := range []string{"305419896", "305419897"} {
		n, _ := strconv.ParseUint(pid, 10, 64)
		if err := s.Resolver.Add(pid, n); err != nil {
			t.Fatal(err)
		}
	}
	a := connectPlayer(t, s, "305419896", "10.253.1.2:1024")
	b := connectPlayer(t, s, "305419897", "10.253.1.3:1024")
	a.conn.SetStations(placeholderStations("10.253.1.2"))
	b.conn.SetStations(placeholderStations("10.253.1.3"))
	table := map[uint64]netip.AddrPort{305419896: netip.MustParseAddrPort("10.253.1.2:50100")}
	s.EndpointByPID = func(pid uint64) (netip.AddrPort, bool) {
		v, ok := table[pid]
		return v, ok
	}
	out := s.MM.SessionStations(b.conn, a.conn)
	if len(out) != 2 {
		t.Fatalf("candidate count %d", len(out))
	}
	for _, st := range out {
		if st.Get("address") != "10.253.1.2" || st.GetInt("port") != 50100 || st.GetInt("RVCID") != int(a.conn.ID) || st.Has("CID") {
			t.Fatalf("explicit PID endpoint not applied exactly: %s", st)
		}
	}
	// No observation for b: the placeholder is not replaced by a guess.
	if out := s.MM.SessionStations(a.conn, b.conn); out[0].GetInt("port") != 1 {
		t.Fatalf("missing table entry fabricated an endpoint: %s", out[0])
	}
	// A CID-bearing reported station wins over the table.
	a.conn.SetStations(append(placeholderStations("10.253.1.2"), nex.ParseStationURL("prudp:/address=10.253.1.2;port=49821;CID=9;RVCID=777;sid=15")))
	if out := s.MM.SessionStations(b.conn, a.conn); out[0].GetInt("port") != 49821 {
		t.Fatalf("reported endpoint lost: %s", out[0])
	}
}
