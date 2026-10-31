package lanstations

import (
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// Stations as the core stores them after Register (derived public copy) and a
// later ReplaceURL carrying the real UDP port and CID.
func registered() []*nex.StationURL {
	local := nex.ParseStationURL("prudp:/address=192.168.50.11;port=51234;sid=15;stream=3;type=0;natm=0;natf=0;PID=49153;RVCID=7")
	public := nex.ParseStationURL("prudp:/address=192.168.50.11;port=51234;sid=15;stream=3;type=11;natm=0;natf=0;PID=49153;RVCID=7;Pa=192.168.50.11")
	return []*nex.StationURL{local, public}
}

func replaced() []*nex.StationURL {
	return []*nex.StationURL{nex.ParseStationURL("prudp:/address=192.168.50.11;port=49821;sid=15;stream=3;type=0;natm=1;natf=1;PID=49153;CID=9;RVCID=7")}
}

func TestRawModePassesThrough(t *testing.T) {
	in := replaced()
	out := Build(ModeRaw, 7, in)
	if len(out) != 1 || out[0] != in[0] {
		t.Fatalf("raw mode must hand back the host's stations unchanged: %v", out)
	}
}

func TestLANModePrefersReplaceURLStation(t *testing.T) {
	in := append(registered(), replaced()...)
	out := Build(ModeLAN, 7, in)
	if len(out) != 2 {
		t.Fatalf("want two candidates, got %d", len(out))
	}
	lan, pub := out[0], out[1]
	for _, u := range out {
		if u.Get("address") != "192.168.50.11" || u.GetInt("port") != 49821 || u.GetInt("CID") != 9 || u.GetInt("RVCID") != 7 {
			t.Fatalf("candidate lost the UDP endpoint/CID/RVCID: %s", u)
		}
	}
	if lan.Has("type") || lan.Has("Pa") {
		t.Fatalf("LAN candidate must carry neither type nor Pa: %s", lan)
	}
	if pub.GetInt("type") != 0x0B || pub.Get("Pa") != "192.168.50.11" {
		t.Fatalf("public candidate must be type=11 with Pa=<address>: %s", pub)
	}
	// Inputs are not mutated (the core shares StationURL pointers across goroutines).
	if in[2].Has("Pa") || in[2].GetInt("type") != 0 {
		t.Fatalf("source station was mutated: %s", in[2])
	}
}

func TestLANModeBeforeReplaceURLUsesRegisteredStation(t *testing.T) {
	out := Build(ModeLAN, 7, registered())
	if len(out) != 2 || out[0].GetInt("port") != 51234 || out[0].Has("type") || out[1].GetInt("type") != 0x0B {
		t.Fatalf("fallback shape: %v", out)
	}
}

func TestLANModeEmptyAndAddressless(t *testing.T) {
	if out := Build(ModeLAN, 1, nil); out != nil {
		t.Fatalf("nil in, want nil out: %v", out)
	}
	in := []*nex.StationURL{nex.ParseStationURL("prudp:/port=1;RVCID=3")}
	if out := Build(ModeLAN, 1, in); len(out) != 1 || out[0] != in[0] {
		t.Fatalf("addressless station must pass through: %v", out)
	}
}
