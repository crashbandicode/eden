//go:build windows

package autolan

import (
	"encoding/json"
	"fmt"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// Windows-specific regression tests for the AF_UNIX stream transport and the DACL-based
// private-directory model. The shared suites (lifecycle, tls, v6, reelect, process) run on
// Windows through guest_windows_test.go; the Linux-only datagram tests have Windows
// counterparts here.

func TestWindowsPrivateDirectoryModel(t *testing.T) {
	good := shortDir(t, "elw")
	if err := checkPrivateDir(good); err != nil {
		t.Fatalf("protected directory refused: %v", err)
	}
	// A directory created the ordinary way inherits the profile's ACL (Administrators included).
	plain, err := os.MkdirTemp("", "elp")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(plain) })
	if err := checkPrivateDir(plain); err == nil {
		t.Fatal("directory with inherited profile ACL accepted as private")
	}
	widened := shortDir(t, "elx")
	makeDirUntrusted(t, widened)
	if err := checkPrivateDir(widened); err == nil {
		t.Fatal("directory readable by Authenticated Users accepted as private")
	}
	if checkPrivateDir(filepath.ToSlash(good)) == nil {
		t.Fatal("non-native path form accepted")
	}
	if normalizeDir(filepath.ToSlash(good)) != good {
		t.Fatalf("normalizeDir(%q) = %q", filepath.ToSlash(good), normalizeDir(filepath.ToSlash(good)))
	}
	if !ownedByCurrentUser(nil, good) {
		t.Fatal("own directory not recognised as owned")
	}
}

func TestWindowsPrivateSecurityAcrossElevation(t *testing.T) {
	const user = "S-1-5-21-111-222-333-1001"
	const other = "S-1-5-21-111-222-333-1002"
	private := []aceInfo{{sid: systemSID}, {sid: user}}
	for _, tc := range []struct {
		name, owner, tokenOwner string
		aces                    []aceInfo
		want                    bool
	}{
		{"unelevated_read_of_elevated_creation", administratorsSID, user, private, true},
		{"elevated_read_of_normal_creation", user, administratorsSID, private, true},
		{"normal_same_account", user, user, private, true},
		{"different_account_owner", other, user, private, false},
		{"administrators_owner_other_account_acl", administratorsSID, user, []aceInfo{{sid: systemSID}, {sid: other}}, false},
		{"administrators_owner_broad_acl", administratorsSID, user, []aceInfo{{sid: user}, {sid: "S-1-5-11"}}, false},
		{"administrators_owner_no_acl", administratorsSID, user, nil, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			info := &securityInfo{owner: tc.owner, daclPresent: true, aces: tc.aces}
			if err := validatePrivateSecurity(info, user, tc.tokenOwner); (err == nil) != tc.want {
				t.Fatalf("private=%v, want %v: %v", err == nil, tc.want, err)
			}
		})
	}
}

func TestWindowsParentMonitorAndProcessFacts(t *testing.T) {
	m, err := newParentMonitor(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	defer m.close()
	if !m.alive() || !processAlive(os.Getpid()) {
		t.Fatal("own process reported dead")
	}
	if _, err := newParentMonitor(1); err == nil {
		t.Fatal("monitor on the idle process pseudo-id succeeded")
	}
	if hostBootKey() == "" || currentUserID() == "unknown-account" {
		t.Fatal("host or account identity unavailable")
	}
}

func TestWindowsSameHostTwoInstancesSameGuestPortUseStreams(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	a := startNode(t, w.options(h, instanceID(1))).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	if a.ready.AutoTransport.InstanceIP == b.ready.AutoTransport.InstanceIP {
		t.Fatal("two instances leased the same virtual address")
	}
	ga := a.guest("ga", 50001)
	gb := b.guest("gb", 50001) // same guest port on both instances
	eventually(t, 5*time.Second, "peer tables", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
	gb.drain()
	ga.send(gb.ep, []byte("a-to-b"))
	f, sender := gb.mustRecv(2 * time.Second)
	if f.Src != ga.ep || f.Dst != gb.ep || string(f.Payload) != "a-to-b" || sender != b.ready.AutoTransport.DatagramSocket {
		t.Fatalf("delivery %v -> %v %q via %s", f.Src, f.Dst, f.Payload, sender)
	}
	gb.send(ga.ep, []byte("b-to-a"))
	if f, _ := ga.mustRecv(2 * time.Second); string(f.Payload) != "b-to-a" || f.Src != gb.ep {
		t.Fatalf("reverse delivery %v %q", f.Src, f.Payload)
	}
	// Zero-length and maximum payloads keep their boundaries on the stream.
	ga.send(gb.ep, nil)
	if f, _ := gb.mustRecv(2 * time.Second); len(f.Payload) != 0 {
		t.Fatalf("zero-length payload became %d bytes", len(f.Payload))
	}
	big := make([]byte, MaxPayload)
	for i := range big {
		big[i] = byte(i)
	}
	ga.send(gb.ep, big)
	if f, _ := gb.mustRecv(2 * time.Second); len(f.Payload) != MaxPayload || f.Payload[MaxPayload-1] != big[MaxPayload-1] {
		t.Fatalf("maximum payload damaged (%d bytes)", len(f.Payload))
	}
	for _, n := range []*node{a, b} {
		c := &n.b.stats
		if c.toLocalPeer.Load() == 0 || c.toRemotePeer.Load() != 0 || c.udpIn.Load() != 0 {
			t.Fatalf("instance %s: local=%d remote=%d udpIn=%d", n.b.instance, c.toLocalPeer.Load(), c.toRemotePeer.Load(), c.udpIn.Load())
		}
	}
}

func TestWindowsGuestDeliveryAfterHelloWriteDeadline(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	a := n.guest("idle-a", 50101)
	b := n.guest("idle-b", 50102)

	// lineWriter gives the hello reply a two-second write budget. An accepted
	// stream must remain usable after that budget, including its first reply.
	time.Sleep(2500 * time.Millisecond)
	a.send(b.ep, []byte("first delivery after idle"))
	f, sender := b.mustRecv(2 * time.Second)
	if f.Src != a.ep || f.Dst != b.ep || string(f.Payload) != "first delivery after idle" ||
		sender != n.ready.AutoTransport.DatagramSocket {
		t.Fatalf("delayed delivery: %v -> %v %q via %s", f.Src, f.Dst, f.Payload, sender)
	}
	b.send(a.ep, []byte("reverse delivery after idle"))
	if f, _ := a.mustRecv(2 * time.Second); f.Src != b.ep || f.Dst != a.ep || string(f.Payload) != "reverse delivery after idle" {
		t.Fatalf("delayed reverse delivery: %v -> %v %q", f.Src, f.Dst, f.Payload)
	}

	// A later idle interval must not expire an otherwise healthy stream either.
	time.Sleep(2500 * time.Millisecond)
	a.send(b.ep, []byte("continued delivery"))
	if f, _ := b.mustRecv(2 * time.Second); f.Src != a.ep || string(f.Payload) != "continued delivery" {
		t.Fatalf("continued delivery: %v %q", f.Src, f.Payload)
	}
	if n.b.stats.sendDrops.Load() != 0 {
		t.Fatal("uncongested delayed delivery dropped a frame")
	}
}

func TestWindowsDataHelloAuthentication(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	n := startNode(t, w.options(h, instanceID(1))).waitReady()
	broker := n.ready.AutoTransport.DatagramSocket
	node := filepath.Join(n.rd, "node.sock")
	closeNode := createSocketNode(t, node)
	defer closeNode()
	// Without a hello'd stream the socket node cannot bind.
	if r := n.control().call(t, fmt.Sprintf(`{"op":"bind","socket":%q,"port":0}`, node)); r.OK || r.Error != "not_connected" {
		t.Fatalf("bind without stream: %+v", r)
	}
	// A path outside the runtime directory, a regular file, a missing node, bad JSON.
	outside := filepath.Join(shortDir(t, "elo"), "x.sock")
	closeOutside := createSocketNode(t, outside)
	defer closeOutside()
	regular := filepath.Join(n.rd, "regular")
	_ = os.WriteFile(regular, nil, 0o600)
	for _, tc := range []struct{ socket, want string }{
		{outside, "bad_path"}, {broker, "bad_path"}, {regular, "not_socket"},
		{filepath.Join(n.rd, "missing.sock"), "not_socket"}, {"rel.sock", "bad_path"},
	} {
		c, _, reply := dialDataStream(t, broker, tc.socket)
		_ = c.Close()
		if reply.OK || reply.Error != tc.want {
			t.Fatalf("hello %q: got %+v want %s", tc.socket, reply, tc.want)
		}
	}
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: broker, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	_ = c.SetDeadline(time.Now().Add(5 * time.Second))
	_, _ = c.Write([]byte("not json\n"))
	var reply dataHelloReply
	buf := make([]byte, 256)
	k, _ := c.Read(buf)
	_ = json.Unmarshal(buf[:k], &reply)
	_ = c.Close()
	if reply.OK || reply.Error != "bad_request" {
		t.Fatalf("malformed hello: %+v", reply)
	}
	// Invalid instance IDs are rejected before any shared-directory path lookup.
	for _, instance := range []string{"../escape", "ABCDEF0123456789abcdef0123456789", "not-hex", ""} {
		cp, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: broker, Net: "unix"})
		if err != nil {
			t.Fatal(err)
		}
		_ = cp.SetDeadline(time.Now().Add(2 * time.Second))
		hello, _ := json.Marshal(dataHello{Kind: "peer", Instance: instance})
		_, _ = cp.Write(append(hello, '\n'))
		k, err := cp.Read(buf)
		_ = cp.Close()
		var invalid dataHelloReply
		if err != nil || json.Unmarshal(buf[:k], &invalid) != nil || invalid.OK || invalid.Error != "bad_request" {
			t.Fatalf("invalid peer instance %q: reply %+v error %v", instance, invalid, err)
		}
	}
	// A second stream for the same node is refused while the first is open.
	c1, _, r1 := dialDataStream(t, broker, node)
	defer c1.Close()
	c2, _, r2 := dialDataStream(t, broker, node)
	_ = c2.Close()
	if !r1.OK || r2.OK || r2.Error != "already_connected" {
		t.Fatalf("duplicate stream: first %+v second %+v", r1, r2)
	}
	// A peer hello for an instance without a record in our shared directory is refused.
	cp, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: broker, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	_ = cp.SetDeadline(time.Now().Add(5 * time.Second))
	hello, _ := json.Marshal(dataHello{Kind: "peer", Instance: instanceID(0x999)})
	_, _ = cp.Write(append(hello, '\n'))
	k, _ = cp.Read(buf)
	_ = cp.Close()
	reply = dataHelloReply{}
	_ = json.Unmarshal(buf[:k], &reply)
	if reply.OK || reply.Error != "forbidden" {
		t.Fatalf("unknown peer accepted: %+v", reply)
	}
}

func TestWindowsControlAndDataRejectOtherProcesses(t *testing.T) {
	w := newWorld(t)
	opt := w.options(w.newHost(1), instanceID(1))
	opt.ControlPeerPID = os.Getppid() // the test process is then a foreign peer
	n := startNode(t, opt).waitReady()
	if r := n.control().call(t, `{"op":"status"}`); r.OK || r.Error != "forbidden" {
		t.Fatalf("foreign control peer accepted: %+v", r)
	}
	node := filepath.Join(n.rd, "node.sock")
	closeNode := createSocketNode(t, node)
	defer closeNode()
	c, _, reply := dialDataStream(t, n.ready.AutoTransport.DatagramSocket, node)
	_ = c.Close()
	if reply.OK || reply.Error != "forbidden" {
		t.Fatalf("foreign data peer accepted: %+v", reply)
	}
}

func TestWindowsForgedSourcesUnknownSendersAndBadRecordsDropped(t *testing.T) {
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
	forged0, unknown0, malformed0 := st.forged.Load(), st.unknownSender.Load(), st.malformed.Load()
	// Wrong source port and wrong source VIP from a bound client.
	f1, _ := AppendFrame(nil, netip.AddrPortFrom(ga.ep.Addr(), 50009), gb.ep, []byte("x"))
	f2, _ := AppendFrame(nil, netip.AddrPortFrom(b.vip(), 50001), gb.ep, []byte("x"))
	ga.sendRaw(f1)
	ga.sendRaw(f2)
	// A hello'd but unbound stream sending an otherwise valid frame.
	strangerNode := filepath.Join(a.rd, "stranger.sock")
	closeStranger := createSocketNode(t, strangerNode)
	defer closeStranger()
	sc, _, reply := dialDataStream(t, a.ready.AutoTransport.DatagramSocket, strangerNode)
	if !reply.OK {
		t.Fatalf("stranger hello: %+v", reply)
	}
	valid, _ := AppendFrame(nil, ga.ep, gb.ep, []byte("x"))
	if _, err := sc.Write(valid); err != nil {
		t.Fatal(err)
	}
	gb.expectNone(300 * time.Millisecond)
	eventually(t, 2*time.Second, "drop counters", func() bool {
		return st.forged.Load() == forged0+2 && st.unknownSender.Load() == unknown0+1
	})
	// An oversized record length closes the sending stream and releases nothing else.
	bad := append([]byte(nil), valid...)
	bad[20], bad[21], bad[22], bad[23] = 0xff, 0xff, 0xff, 0xff
	if _, err := sc.Write(bad); err != nil {
		t.Fatal(err)
	}
	eventually(t, 2*time.Second, "malformed counter", func() bool { return st.malformed.Load() == malformed0+1 })
	eventually(t, 2*time.Second, "stranger stream closed", func() bool {
		_ = sc.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
		_, err := sc.Read(make([]byte, 1))
		return err != nil && !os.IsTimeout(err)
	})
	_ = sc.Close()
	// The legitimate path still works afterwards.
	ga.send(gb.ep, []byte("still-alive"))
	if f, _ := gb.mustRecv(2 * time.Second); string(f.Payload) != "still-alive" {
		t.Fatalf("payload %q", f.Payload)
	}
}

func TestWindowsGuestStreamCloseReleasesBinding(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	g := n.guest("g", 50400)
	g.tr.close()
	eventually(t, 2*time.Second, "binding released", func() bool {
		n.b.mu.RLock()
		defer n.b.mu.RUnlock()
		return len(n.b.bindings) == 0
	})
	g2 := n.guest("g2", 50400) // the port is free again
	g2.send(g2.ep, []byte("self"))
	if f, _ := g2.mustRecv(2 * time.Second); string(f.Payload) != "self" {
		t.Fatal("rebound port does not route")
	}
}

// Review N1: a guest that stops draining loses records (bounded queue), never its binding or stream.
func TestWindowsStalledGuestLosesRecordsNotItsBinding(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	ga := n.guest("ga", 50001)
	gb := n.guest("gb", 50002) // does not read until later
	big := make([]byte, 60000)
	drops0 := n.b.stats.sendDrops.Load()
	const sent = 60 // 3.6 MB: more than the kernel buffers plus the 2 MiB / 256-record queue
	for i := 0; i < sent; i++ {
		ga.send(gb.ep, big)
	}
	eventually(t, 10*time.Second, "queue drops", func() bool { return n.b.stats.sendDrops.Load() > drops0 })
	n.b.mu.RLock()
	_, bound := n.b.bindings[gb.path]
	n.b.mu.RUnlock()
	if !bound || !n.b.dataT.hasClient(gb.path) {
		t.Fatal("stalled guest lost its binding or its stream")
	}
	got := 0
	for {
		if _, _, ok := gb.recv(500 * time.Millisecond); !ok {
			break
		}
		got++
	}
	if got == 0 || got >= sent {
		t.Fatalf("expected some but not all records to arrive, got %d of %d", got, sent)
	}
	ga.send(gb.ep, []byte("after"))
	if f, _ := gb.mustRecv(2 * time.Second); string(f.Payload) != "after" {
		t.Fatalf("payload after the stall %q", f.Payload)
	}
}

func TestWindowsStopMarkerStopsBroker(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	if err := os.WriteFile(filepath.Join(n.rd, stopFileName), nil, 0o600); err != nil {
		t.Fatal(err)
	}
	select {
	case <-n.done:
		if n.err != nil {
			t.Fatalf("stop marker produced an error: %v", n.err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("broker ignored the stop marker")
	}
	for _, p := range []string{readyFileName, controlSocketName, dataSocketName} {
		if _, err := os.Lstat(filepath.Join(n.rd, p)); err == nil {
			t.Fatalf("%s left after stop", p)
		}
	}
}
