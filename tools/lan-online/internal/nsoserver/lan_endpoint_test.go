package nsoserver

import (
	"context"
	"net"
	"testing"
	"time"

	nex "github.com/NextendoNetwork/nextendo-nex"

	"edennsolan/backend/internal/lanstations"
	"edennsolan/backend/internal/natcheck"
)

func placeholderStations(address string) []*nex.StationURL {
	return []*nex.StationURL{
		nex.ParseStationURL("prudp:/address=" + address + ";port=1;sid=15;stream=3;PID=49153;natm=1;natf=2;Pl=metadata"),
		nex.ParseStationURL("prudp:/address=" + address + ";port=2;sid=15;stream=3"),
	}
}

func TestLANEndpointObservedHostAndIndependentPeers(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.0.137:40001")
	b := connectPlayer(t, s, "49154", "192.168.0.166:40002")
	a.conn.SetStations(placeholderStations("192.168.0.137"))
	b.conn.SetStations(placeholderStations("192.168.0.166"))
	observations := []natcheck.Observation{{IP: "192.168.0.137", Port: 51190}, {IP: "192.168.0.166", Port: 51189}}
	builder := lanstations.Builder(lanstations.ModeLAN, nil, func() []natcheck.Observation { return observations })
	// Original presentation reproduces the observed bug: placeholder port1 survives.
	if baseline := lanstations.Build(lanstations.ModeLAN, b.conn.ID, b.conn.Stations()); baseline[0].GetInt("port") != 1 {
		t.Fatal("baseline control did not reproduce placeholder port1")
	}
	for _, test := range []struct {
		host, joiner *nex.Connection
		ip           string
		port         int
	}{{a.conn, b.conn, "192.168.0.137", 51190}, {b.conn, a.conn, "192.168.0.166", 51189}} {
		out := builder(test.joiner, test.host)
		if len(out) != 2 {
			t.Fatalf("candidate count %d", len(out))
		}
		for _, station := range out {
			if station.Get("address") != test.ip || station.GetInt("port") != test.port || station.GetInt("RVCID") != int(test.host.ID) {
				t.Fatalf("host endpoint crosswired: %s", station)
			}
			if station.Has("CID") || station.Get("Pl") != "metadata" || station.GetInt("sid") != 15 || station.GetInt("natf") != 2 {
				t.Fatalf("metadata lost or CID fabricated: %s", station)
			}
		}
		if test.host.Stations()[0].GetInt("port") != 1 {
			t.Fatal("shared source station mutated")
		}
	}
}

func TestLANEndpointAmbiguousSharedIPRefusesFallback(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.0.137:40001")
	b := connectPlayer(t, s, "49154", "192.168.0.137:40002")
	a.conn.SetStations(placeholderStations("192.168.0.137"))
	b.conn.SetStations(placeholderStations("192.168.0.137"))
	builder := lanstations.Builder(lanstations.ModeLAN, nil, func() []natcheck.Observation {
		return []natcheck.Observation{{IP: "192.168.0.137", Port: 51189}}
	})
	for _, host := range []*nex.Connection{a.conn, b.conn} {
		if got := builder(a.conn, host)[0].GetInt("port"); got != 1 {
			t.Fatalf("shared-IP fallback crosswired PID %d to port%d", host.PID, got)
		}
	}
}

func TestLANEndpointReportedPortWinsAndRawUnchanged(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.0.137:40001")
	b := connectPlayer(t, s, "49154", "192.168.0.166:40002")
	stations := append(placeholderStations("192.168.0.137"), nex.ParseStationURL("prudp:/address=192.168.0.137;port=49821;CID=9;RVCID=777;sid=15"))
	a.conn.SetStations(stations)
	observations := func() []natcheck.Observation { t.Fatal("reported/raw path must not read NNCS"); return nil }
	for _, mode := range []lanstations.Mode{lanstations.ModeLAN, lanstations.ModeRaw} {
		out := lanstations.Builder(mode, nil, observations)(b.conn, a.conn)
		if mode == lanstations.ModeRaw {
			if len(out) != len(stations) || out[0] != stations[0] || out[2] != stations[2] {
				t.Fatal("raw stations changed")
			}
		} else if out[0].GetInt("port") != 49821 || out[0].GetInt("CID") != 9 || out[0].GetInt("RVCID") != int(a.conn.ID) {
			t.Fatalf("reported endpoint lost: %s", out[0])
		}
	}
	placeholders := placeholderStations("192.168.0.137")
	a.conn.SetStations(placeholders)
	out := lanstations.Builder(lanstations.ModeRaw, nil, observations)(b.conn, a.conn)
	if len(out) != 2 || out[0] != placeholders[0] || out[1] != placeholders[1] || out[0].GetInt("port") != 1 {
		t.Fatal("raw placeholder stations must bypass NNCS unchanged")
	}
}

func TestLANEndpointMissingInvalidAndUnauthenticatedRefused(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.0.137:40001")
	b := connectPlayer(t, s, "49154", "192.168.0.166:40002")
	a.conn.SetStations(placeholderStations("192.168.0.137"))
	for _, observations := range [][]natcheck.Observation{
		nil,
		{{IP: "192.168.0.166", Port: 51189}},
		{{IP: "invalid", Port: 51189}},
		{{IP: "192.168.0.137", Port: 0}},
		{{IP: "192.168.0.137", Port: 1}},
		{{IP: "192.168.0.137", Port: 2}},
		{{IP: "192.168.0.137", Port: -1}},
		{{IP: "192.168.0.137", Port: 65536}},
		{{IP: "192.168.0.137", Port: 51189}, {IP: "192.168.0.137", Port: 51190}},
	} {
		out := lanstations.Builder(lanstations.ModeLAN, nil, func() []natcheck.Observation { return observations })(b.conn, a.conn)
		if out[0].GetInt("port") != 1 {
			t.Fatalf("invalid/missing observation used: %+v", observations)
		}
	}
	valid := func() []natcheck.Observation { return []natcheck.Observation{{IP: "192.168.0.137", Port: 51189}} }
	for _, remote := range []string{"invalid", "8.8.8.8:40001", "[::1]:40001"} {
		other, _ := newServer(t)
		invalid := connectPlayer(t, other, "49153", remote)
		invalid.conn.SetStations(placeholderStations("192.168.0.137"))
		if out := lanstations.Builder(lanstations.ModeLAN, nil, valid)(b.conn, invalid.conn); out[0].GetInt("port") != 1 {
			t.Fatalf("invalid connection address received fallback: %s", remote)
		}
	}
	ghost := nex.NewConnection(s.Secure, "192.168.0.137:40003", func([]byte) {})
	ghost.SetStations(placeholderStations("192.168.0.137"))
	if out := lanstations.Builder(lanstations.ModeLAN, nil, valid)(b.conn, ghost); out[0].GetInt("port") != 1 {
		t.Fatal("unauthenticated connection received fallback")
	}
}

// Exercise New's real closure against a responder's in-memory observation, not a
// file or a replacement callback. All sockets are ephemeral loopback test sockets.
func TestLANEndpointServerUsesResponderMemory(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "127.0.0.137:40001")
	b := connectPlayer(t, s, "49154", "127.0.0.166:40002")
	body := nex.NewStreamOut(s.SecureSettings)
	nex.WriteList(body, placeholderStations("127.0.0.166"), func(out *nex.StreamOut, station *nex.StationURL) { out.StationURL(station) })
	response, err := b.call(nex.ProtocolSecureConnection, nex.MethodRegister, 17, body.Bytes())
	if err != nil || response.IsError {
		t.Fatal("Register with two LAN placeholders failed")
	}
	stored := b.conn.Stations()
	if len(stored) < 2 || stored[0].GetInt("port") != 1 || stored[1].GetInt("port") != 40002 || stored[1].Has("CID") {
		t.Fatal("Register control must contain local placeholder plus synthesized public TCP port")
	}
	r := &natcheck.Responder{Addrs: []string{"127.0.0.1"}, Ports: []int{0}}
	if err := r.Listen(); err != nil {
		t.Fatal(err)
	}
	s.nat = r
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { r.Serve(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	client, err := net.ListenPacket("udp4", "127.0.0.166:0")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	destination, _ := net.ResolveUDPAddr("udp4", r.LocalAddrs()[0])
	probe := make([]byte, 16)
	probe[3] = 101
	if _, err := client.WriteTo(probe, destination); err != nil {
		t.Fatal(err)
	}
	_ = client.SetReadDeadline(time.Now().Add(time.Second))
	if _, _, err := client.ReadFrom(make([]byte, 16)); err != nil {
		t.Fatal(err)
	}
	out := s.MM.SessionStations(a.conn, b.conn)
	if out[0].Get("address") != "127.0.0.166" || out[0].GetInt("port") != client.LocalAddr().(*net.UDPAddr).Port {
		t.Fatalf("server did not use responder memory: %s", out[0])
	}
}
