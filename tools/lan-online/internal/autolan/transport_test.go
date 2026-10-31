//go:build linux

package autolan

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestFrameCodecBoundaries(t *testing.T) {
	src := netip.MustParseAddrPort("10.253.1.2:50000")
	dst := netip.MustParseAddrPort("10.253.3.4:50001")
	frame, err := AppendFrame(nil, src, dst, []byte("payload"))
	if err != nil {
		t.Fatal(err)
	}
	// Independent oracle: the documented byte layout written out literally.
	want := []byte{'E', 'L', 'N', '1', 1, 0, 0, 0, 10, 253, 1, 2, 10, 253, 3, 4, 0xC3, 0x50, 0xC3, 0x51, 0, 0, 0, 7}
	want = append(want, "payload"...)
	if !bytes.Equal(frame, want) {
		t.Fatalf("encoding mismatch:\n got %x\nwant %x", frame, want)
	}
	f, err := DecodeFrame(frame)
	if err != nil || f.Src != src || f.Dst != dst || string(f.Payload) != "payload" {
		t.Fatalf("decode: %+v %v", f, err)
	}
	maxFrame, err := AppendFrame(nil, src, dst, make([]byte, MaxPayload))
	if err != nil || len(maxFrame) != 65507 {
		t.Fatalf("max frame: len=%d err=%v", len(maxFrame), err)
	}
	if _, err := DecodeFrame(maxFrame); err != nil {
		t.Fatalf("max frame rejected: %v", err)
	}
	if _, err := AppendFrame(nil, src, dst, make([]byte, MaxPayload+1)); err == nil {
		t.Fatal("oversized payload encoded")
	}
	mutate := func(f func([]byte) []byte) []byte { return f(append([]byte(nil), frame...)) }
	for name, bad := range map[string][]byte{
		"short":         frame[:HeaderLen-1],
		"truncated":     frame[:len(frame)-1],
		"trailing":      append(append([]byte(nil), frame...), 0),
		"magic":         mutate(func(b []byte) []byte { b[0] = 'X'; return b }),
		"version":       mutate(func(b []byte) []byte { b[4] = 2; return b }),
		"reserved":      mutate(func(b []byte) []byte { b[6] = 1; return b }),
		"length-over":   mutate(func(b []byte) []byte { binary.BigEndian.PutUint32(b[20:24], MaxPayload+1); return b }),
		"length-differ": mutate(func(b []byte) []byte { binary.BigEndian.PutUint32(b[20:24], 6); return b }),
	} {
		if _, err := DecodeFrame(bad); err == nil {
			t.Errorf("%s frame accepted", name)
		}
	}
}

func TestSameHostTwoInstancesSameGuestPortUseUnixOnly(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	a := startNode(t, w.options(h, instanceID(1))).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	if a.ready.AccountID == b.ready.AccountID || a.vip() == b.vip() {
		t.Fatalf("identities not unique: %d/%s %d/%s", a.ready.AccountID, a.vip(), b.ready.AccountID, b.vip())
	}
	for _, n := range []*node{a, b} {
		if n.ready.AccountID == 0 || n.ready.AccountID == 2 || !instanceNet.Contains(n.vip()) {
			t.Fatalf("invalid lease %d %s", n.ready.AccountID, n.vip())
		}
	}
	ga := a.guest("ga", 50000)
	gb := b.guest("gb", 50000)
	if ga.ep.Port() != 50000 || gb.ep.Port() != 50000 {
		t.Fatal("identical guest port not granted to both instances")
	}
	eventually(t, 5*time.Second, "peer tables", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
	gb.drain()
	ga.send(gb.ep, []byte("hello-from-a"))
	f, sender := gb.mustRecv(2 * time.Second)
	if f.Src != ga.ep || f.Dst != gb.ep || string(f.Payload) != "hello-from-a" || sender != b.ready.AutoTransport.DatagramSocket {
		t.Fatalf("A->B delivery wrong: src=%v dst=%v payload=%q sender=%q", f.Src, f.Dst, f.Payload, sender)
	}
	gb.send(ga.ep, []byte("hello-from-b"))
	f, _ = ga.mustRecv(2 * time.Second)
	if f.Src != gb.ep || string(f.Payload) != "hello-from-b" {
		t.Fatalf("B->A delivery wrong: %v %q", f.Src, f.Payload)
	}
	// Asserted through the control-socket status counters a parent process sees.
	for _, n := range []*node{a, b} {
		c := n.control().call(t, `{"op":"status"}`).Counters
		if c == nil || c.UnixPeerFrames == 0 || c.UDPPeerFrames != 0 || c.UDPFramesIn != 0 {
			t.Fatalf("same-host gameplay must use AF_UNIX only: %+v", c)
		}
	}
	// Loopback within one instance.
	ga2 := a.guest("ga2", 50001)
	ga2.send(ga.ep, []byte("self"))
	if f, _ := ga.mustRecv(2 * time.Second); f.Src != ga2.ep {
		t.Fatalf("intra-instance delivery src %v", f.Src)
	}
}

func TestCrossHostDirectUDPAndForgedSourcesDropped(t *testing.T) {
	w := newWorld(t)
	a := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	b := startNode(t, w.options(w.newHost(2), instanceID(2))).waitReady()
	ga := a.guest("ga", 50000)
	gb := b.guest("gb", 50000)
	eventually(t, 5*time.Second, "cross-host route", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
	gb.drain()
	ga.send(gb.ep, []byte("over-udp"))
	f, _ := gb.mustRecv(2 * time.Second)
	if f.Src != ga.ep || string(f.Payload) != "over-udp" {
		t.Fatalf("wrong delivery %v %q", f.Src, f.Payload)
	}
	if a.b.stats.toRemotePeer.Load() == 0 || b.b.stats.udpIn.Load() == 0 || a.b.stats.toLocalPeer.Load() != 0 {
		t.Fatal("cross-host datagrams did not use the direct UDP path")
	}
	// An unregistered UDP source and a registered peer claiming another VIP are both dropped.
	raw, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer raw.Close()
	forged, _ := AppendFrame(nil, ga.ep, gb.ep, []byte("forged"))
	before := b.b.stats.udpRejected.Load()
	if _, err := raw.WriteToUDPAddrPort(forged, netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), b.b.udpPort)); err != nil {
		t.Fatal(err)
	}
	gb.expectNone(300 * time.Millisecond)
	if b.b.stats.udpRejected.Load() != before+1 {
		t.Fatal("unregistered UDP source not rejected")
	}
	wrongVIP, _ := AppendFrame(nil, netip.AddrPortFrom(netip.MustParseAddr("10.253.200.200"), 50000), gb.ep, []byte("x"))
	if err := a.b.sendUDP(netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), b.b.udpPort), wrongVIP); err != nil {
		t.Fatal(err)
	}
	gb.expectNone(300 * time.Millisecond)
	if b.b.stats.udpRejected.Load() != before+2 {
		t.Fatal("registered peer forging another virtual source was not rejected")
	}
}

func TestControlBindRulesAndPathValidation(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	cc := n.control()
	mk := func(name string) string {
		p := filepath.Join(n.rd, name)
		s, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: p, Net: "unixgram"})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = s.Close() })
		return p
	}
	s1, s2, s3 := mk("s1.sock"), mk("s2.sock"), mk("s3.sock")
	call := func(line string) controlResponse { return cc.call(t, line) }
	expect := func(line, code string) {
		t.Helper()
		if r := call(line); r.OK || r.Error != code {
			t.Fatalf("%s: got ok=%v err=%q want %q", line, r.OK, r.Error, code)
		}
	}
	if r := call(fmt.Sprintf(`{"op":"bind","socket":%q,"port":50500,"id":7}`, s1)); !r.OK || r.Port != 50500 || string(r.ID) != "7" {
		t.Fatalf("fixed bind: %+v", r)
	}
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":50500}`, s2), "port_in_use")
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":50501}`, s1), "already_bound")
	r := call(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, s2))
	if !r.OK || r.Port < 49152 || r.Port == 50500 {
		t.Fatalf("ephemeral bind: %+v", r)
	}
	if r := call(fmt.Sprintf(`{"op":"unbind","socket":%q}`, s1)); !r.OK {
		t.Fatalf("unbind: %+v", r)
	}
	expect(fmt.Sprintf(`{"op":"unbind","socket":%q}`, s1), "not_bound")
	if r := call(fmt.Sprintf(`{"op":"bind","socket":%q,"port":50500}`, s3)); !r.OK {
		t.Fatalf("port reuse after unbind: %+v", r)
	}
	// Path validation: outside, broker-owned, regular file, symlink, missing, relative.
	outside := filepath.Join(shortDir(t, "elo"), "x.sock")
	if s, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: outside, Net: "unixgram"}); err == nil {
		defer s.Close()
	}
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, outside), "bad_path")
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, n.ready.AutoTransport.DatagramSocket), "bad_path")
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, n.rd+"/../"+filepath.Base(n.rd)+"/s3.sock"), "bad_path")
	regular := filepath.Join(n.rd, "regular")
	_ = os.WriteFile(regular, nil, 0o600)
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, regular), "not_socket")
	link := filepath.Join(n.rd, "link.sock")
	_ = os.Symlink(outside, link)
	if r := call(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, link)); r.OK {
		t.Fatal("symlinked socket accepted")
	}
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, filepath.Join(n.rd, "missing.sock")), "not_socket")
	expect(`{"op":"bind","socket":"rel.sock","port":0}`, "bad_path")
	expect(fmt.Sprintf(`{"op":"bind","socket":%q,"port":70000}`, s1), "bad_request")
	expect(fmt.Sprintf(`{"op":"bind","socket":%q}`, s1), "bad_request")
	expect(`{"op":"bind","socket":1,"port":0}`, "bad_request")
	expect(`{"op":"bind","socket":"x","port":0,"extra":1}`, "bad_request")
	expect(`not json`, "bad_request")
	expect(`{"op":"frobnicate"}`, "unknown_op")
	if r := call(`{"op":"status"}`); !r.OK || r.InstanceIP != n.ready.AutoTransport.InstanceIP || r.AccountID != n.ready.AccountID || r.Leader == nil || !*r.Leader {
		t.Fatalf("status: %+v", r)
	}
	// Oversized request: error reply then close.
	big := `{"op":"status","id":"` + strings.Repeat("x", maxControlLine) + `"}`
	if r := call(big); r.Error != "too_large" {
		t.Fatalf("oversized line: %+v", r)
	}
	if _, err := cc.r.ReadByte(); err == nil {
		t.Fatal("connection stayed open after an oversized line")
	}
}

func TestControlRejectsOtherProcesses(t *testing.T) {
	w := newWorld(t)
	opt := w.options(w.newHost(1), instanceID(1))
	opt.ControlPeerPID = os.Getppid() // the test process is then a foreign peer
	n := startNode(t, opt).waitReady()
	// Authentication rejects immediately, before reading any request. Sending
	// first races that deliberate close and can get EPIPE on a correct server.
	cc := n.control()
	_ = cc.c.SetReadDeadline(time.Now().Add(5 * time.Second))
	line, err := cc.r.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var r controlResponse
	if err := strictUnmarshal(line, &r); err != nil {
		t.Fatal(err)
	}
	if r.OK || r.Error != "forbidden" {
		t.Fatalf("foreign peer accepted: %+v", r)
	}
	if _, err := cc.r.ReadByte(); err == nil {
		t.Fatal("foreign peer connection remained open")
	} else {
		var netErr net.Error
		if errors.As(err, &netErr) && netErr.Timeout() {
			t.Fatal("foreign peer connection did not close before the deadline")
		}
	}
}

func TestGuestSenderForgeryAndMalformedFramesDropped(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	a := startNode(t, w.options(h, instanceID(1))).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	ga := a.guest("ga", 50001)
	gb := b.guest("gb", 50002)
	eventually(t, 5*time.Second, "peer tables", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
	gb.drain()
	st := &a.b.stats
	forged0, malformed0, unknown0 := st.forged.Load(), st.malformed.Load(), st.unknownSender.Load()
	// Wrong source port and wrong source VIP from a bound client.
	f1, _ := AppendFrame(nil, netip.AddrPortFrom(ga.ep.Addr(), 50009), gb.ep, []byte("x"))
	f2, _ := AppendFrame(nil, netip.AddrPortFrom(b.vip(), 50001), gb.ep, []byte("x"))
	ga.sendRaw(f1)
	ga.sendRaw(f2)
	// Unregistered sender path with an otherwise valid frame.
	stranger, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: filepath.Join(a.rd, "stranger.sock"), Net: "unixgram"})
	if err != nil {
		t.Fatal(err)
	}
	defer stranger.Close()
	valid, _ := AppendFrame(nil, ga.ep, gb.ep, []byte("x"))
	if _, err := stranger.WriteToUnix(valid, &net.UnixAddr{Name: a.ready.AutoTransport.DatagramSocket, Net: "unixgram"}); err != nil {
		t.Fatal(err)
	}
	// Truncated and corrupted frames from a legitimate client.
	for _, bad := range [][]byte{valid[:10], valid[:HeaderLen], valid[:len(valid)-1], append([]byte("XLN1"), valid[4:]...)} {
		ga.sendRaw(bad)
	}
	gb.expectNone(300 * time.Millisecond)
	eventually(t, 2*time.Second, "drop counters", func() bool {
		return st.forged.Load() == forged0+2 && st.malformed.Load() == malformed0+4 && st.unknownSender.Load() == unknown0+1
	})
	// The path still works afterwards.
	ga.send(gb.ep, []byte("still-alive"))
	if f, _ := gb.mustRecv(2 * time.Second); string(f.Payload) != "still-alive" {
		t.Fatalf("payload %q", f.Payload)
	}
	// Unknown destination VIP: dropped, not delivered anywhere.
	ga.send(netip.MustParseAddrPort("10.253.250.250:50002"), []byte("nobody"))
	eventually(t, 2*time.Second, "no-route counter", func() bool { return st.noRoute.Load() > 0 })
}

func TestNNCSRecordedAtCoordinatorBeforeReply(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	g := n.guest("g", 50100)
	coord := n.b.coordPub.Load()
	if coord == nil {
		t.Fatal("single broker is not leading")
	}
	for _, server := range []netip.AddrPort{netip.MustParseAddrPort("10.254.0.1:10025"), netip.MustParseAddrPort("10.254.0.2:10125")} {
		probe := make([]byte, 16)
		binary.BigEndian.PutUint32(probe, 101)
		g.send(server, probe)
		f, _ := g.mustRecv(2 * time.Second)
		vip := g.ep.Addr().As4()
		srv := server.Addr().As4()
		want := []byte{0, 0, 0, 101, 0, 0, 0xC3, 0xB4, vip[0], vip[1], vip[2], vip[3], srv[0], srv[1], srv[2], srv[3]}
		if f.Src != server || f.Dst != g.ep || !bytes.Equal(f.Payload, want) {
			t.Fatalf("NNCS reply: src=%v dst=%v payload=%x want %x", f.Src, f.Dst, f.Payload, want)
		}
		// The coordinator already holds the observation when the reply arrives.
		if ep, ok := coord.EndpointByPID(uint64(n.ready.AccountID)); !ok || ep != g.ep {
			t.Fatalf("observation %v %v, want %v", ep, ok, g.ep)
		}
	}
	g.send(netip.MustParseAddrPort("10.254.0.1:10025"), make([]byte, 15))
	g.expectNone(300 * time.Millisecond)
	g.send(netip.MustParseAddrPort("10.254.0.1:9999"), make([]byte, 16))
	g.expectNone(300 * time.Millisecond)
	if n.b.stats.natDropped.Load() == 0 || n.b.stats.noRoute.Load() == 0 {
		t.Fatal("short probe or non-NNCS service port was not dropped")
	}
}
