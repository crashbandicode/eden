//go:build linux

package autolan

import (
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

// Regression tests for TRANSPORT_REVIEW.md T1 (an unreachable coordinator must
// not capture the LAN) and T2 (remote metadata must not steer local writes).

// deadLoopbackEndpoint returns a TCP endpoint that refuses connections: a
// leader whose beacon is heard but whose coordinator cannot be joined.
func deadLoopbackEndpoint(t *testing.T) netip.AddrPort {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ap := ln.Addr().(*net.TCPAddr).AddrPort()
	_ = ln.Close()
	return ap
}

func injectUnreachableLeader(a Announcer, instance string, ep netip.AddrPort) {
	a.Publish(&Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000", Instance: instance,
		Role: roleLeader, TCP: ep.String()})
}

// T1 counterexample A: the only leader every broker hears cannot be joined.
// The brokers must fall back to electing among themselves and become ready.
func TestUnreachableLeaderDoesNotBlockStartup(t *testing.T) {
	w := newWorld(t)
	injectUnreachableLeader(w.hub.announcer(instanceID(1)), instanceID(1), deadLoopbackEndpoint(t))
	optA, optB := w.options(w.newHost(1), instanceID(5)), w.options(w.newHost(2), instanceID(6))
	var a, b *node
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); a = startNode(t, optA) }()
	go func() { defer wg.Done(); b = startNode(t, optB) }()
	wg.Wait()
	a.waitReady()
	b.waitReady()
	eventually(t, 5*time.Second, "fallback coordinator", func() bool {
		r := status(t, b)
		return a.b.leading.Load() && !b.b.leading.Load() && r.Connected != nil && *r.Connected
	})
	for _, n := range []*node{a, b} {
		if r := status(t, n); r.CoordinatorUnreachable == nil || !*r.CoordinatorUnreachable {
			t.Fatalf("unreachable coordinator not reported: %+v", r)
		}
	}
	ga, gb := a.guest("ga", 50500), b.guest("gb", 50500)
	eventually(t, 5*time.Second, "datagram route", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
}

// T1 counterexample B: a healthy LAN hears a lower-instance leader it cannot
// reach. The current coordinator must not step down, its members' links must
// not reset, and a late joiner must still join the healthy coordinator.
func TestUnreachableLowerLeaderDoesNotCaptureHealthyLAN(t *testing.T) {
	w := newWorld(t)
	a := startNode(t, w.options(w.newHost(1), instanceID(5))).waitReady()
	b := startNode(t, w.options(w.newHost(2), instanceID(6))).waitReady()
	eventually(t, 5*time.Second, "B linked to A", func() bool {
		r := status(t, b)
		return a.b.leading.Load() && r.Connected != nil && *r.Connected
	})
	epochA := a.b.coordPub.Load().epoch
	linkB, _ := b.b.currentLink()
	injectUnreachableLeader(w.hub.announcer(instanceID(1)), instanceID(1), deadLoopbackEndpoint(t))
	eventually(t, 5*time.Second, "A excludes the unreachable lower leader", func() bool {
		r := status(t, a)
		return r.CoordinatorUnreachable != nil && *r.CoordinatorUnreachable
	})
	time.Sleep(time.Second) // several more step-down checks
	if c := a.b.coordPub.Load(); !a.b.leading.Load() || c == nil || c.epoch != epochA {
		t.Fatal("the healthy coordinator stepped down for an unreachable leader")
	}
	if l, _ := b.b.currentLink(); l != linkB {
		t.Fatal("a member's coordinator link was reset")
	}
	c := startNode(t, w.options(w.newHost(3), instanceID(7))).waitReady()
	eventually(t, 5*time.Second, "late joiner joins the healthy coordinator", func() bool {
		r := status(t, c)
		return !c.b.leading.Load() && r.Connected != nil && *r.Connected && a.b.leading.Load()
	})
}

// partitionable is a test network where a group hears only itself until merged.
type partitionable struct {
	mu     sync.Mutex
	merged bool
	m      map[string]struct {
		group int
		b     Beacon
	}
}

type partAnnouncer struct {
	p     *partitionable
	self  string
	group int
}

func (a *partAnnouncer) Publish(b *Beacon) {
	a.p.mu.Lock()
	defer a.p.mu.Unlock()
	c, ok := networkCopy(b)
	if !ok {
		delete(a.p.m, a.self)
		return
	}
	a.p.m[a.self] = struct {
		group int
		b     Beacon
	}{a.group, c}
}

func (a *partAnnouncer) Heard() []Beacon {
	a.p.mu.Lock()
	defer a.p.mu.Unlock()
	var out []Beacon
	for k, e := range a.p.m {
		if k != a.self && (a.p.merged || e.group == a.group) {
			out = append(out, e.b)
		}
	}
	return out
}

func (a *partAnnouncer) Close() error { a.Publish(nil); return nil }

// The positive side of the probe: after a partition heals, a leader that can
// reach a lower-instance leader still steps down and joins it.
func TestReachableLowerLeaderStillMergesPartition(t *testing.T) {
	w := newWorld(t)
	p := &partitionable{m: map[string]struct {
		group int
		b     Beacon
	}{}}
	optHigh, optLow := w.options(w.newHost(1), instanceID(5)), w.options(w.newHost(2), instanceID(1))
	optHigh.Network = &partAnnouncer{p: p, self: instanceID(5), group: 1}
	optLow.Network = &partAnnouncer{p: p, self: instanceID(1), group: 2}
	high := startNode(t, optHigh).waitReady()
	low := startNode(t, optLow).waitReady()
	if !high.b.leading.Load() || !low.b.leading.Load() {
		t.Fatal("partitioned brokers did not each lead")
	}
	p.mu.Lock()
	p.merged = true
	p.mu.Unlock()
	eventually(t, 10*time.Second, "merge under the lower reachable leader", func() bool {
		r := status(t, high)
		return low.b.leading.Load() && !high.b.leading.Load() && r.Connected != nil && *r.Connected
	})
	if r := status(t, high); r.AccountID != high.ready.AccountID || r.InstanceIP != high.ready.AutoTransport.InstanceIP {
		t.Fatalf("identity changed across the merge: %+v", r)
	}
}

// T2: two instances on one host whose coordinator is on another host still
// exchange datagrams over AF_UNIX (no host UDP), while the coordinator's host
// is reached over UDP; the members' coordinator links are TCP.
func TestSameHostPeersUseUnixUnderRemoteCoordinator(t *testing.T) {
	w := newWorld(t)
	remote, shared := w.newHost(2), w.newHost(1)
	c := startNode(t, w.options(remote, instanceID(1))).waitReady()
	a := startNode(t, w.options(shared, instanceID(2))).waitReady()
	b := startNode(t, w.options(shared, instanceID(3))).waitReady()
	if !c.b.leading.Load() || a.b.leading.Load() || b.b.leading.Load() {
		t.Fatal("expected the remote broker to coordinate")
	}
	if l, _ := a.b.currentLink(); l == nil || l.conn.RemoteAddr().Network() != "tcp" {
		t.Fatal("member link to the remote coordinator is not TCP")
	}
	ga, gb, gc := a.guest("ga", 50400), b.guest("gb", 50400), c.guest("gc", 50400)
	// Wait for membership without sending probe datagrams: a receive can beat
	// the sender's post-send counter increment, contaminating a baseline.
	for _, pair := range [][2]*node{{a, b}, {b, a}, {a, c}, {c, a}} {
		eventually(t, 5*time.Second, "peer route installed", func() bool {
			vip := pair[1].vip()
			pair[0].b.mu.RLock()
			defer pair[0].b.mu.RUnlock()
			return pair[0].b.peerByVIP[vip] != nil
		})
	}
	before := status(t, a).Counters
	ga.send(gb.ep, []byte("same-host"))
	if f, sender := gb.mustRecv(2 * time.Second); string(f.Payload) != "same-host" || sender != b.ready.AutoTransport.DatagramSocket {
		t.Fatalf("same-host delivery %q from %q", f.Payload, sender)
	}
	eventually(t, time.Second, "successful AF_UNIX send counted", func() bool {
		return status(t, a).Counters.UnixPeerFrames == before.UnixPeerFrames+1
	})
	after := status(t, a).Counters
	if after.UnixPeerFrames != before.UnixPeerFrames+1 || after.UDPPeerFrames != before.UDPPeerFrames {
		t.Fatalf("same-host datagram did not take AF_UNIX: before %+v after %+v", before, after)
	}
	ga.send(gc.ep, []byte("cross-host"))
	if f, _ := gc.mustRecv(2 * time.Second); string(f.Payload) != "cross-host" {
		t.Fatalf("cross-host payload %q", f.Payload)
	}
	eventually(t, time.Second, "successful UDP send counted", func() bool {
		final := status(t, a).Counters
		return final.UDPPeerFrames == after.UDPPeerFrames+1 && final.UnixPeerFrames == after.UnixPeerFrames
	})
}

// T2: whether a peer is local, and which socket we write to, comes only from a
// live record in our own shared directory naming a socket we own. Coordinator
// data cannot designate a local socket, and loopback is refused in production.
func TestPeerLocalityComesOnlyFromOwnedSharedMetadata(t *testing.T) {
	shared := shortDir(t, "els")
	if err := os.Mkdir(filepath.Join(shared, "brokers"), 0o700); err != nil {
		t.Fatal(err)
	}
	key := instanceID(0xabc)
	b := &Broker{title: "01006A800016E000", instance: instanceID(9), sharedDir: shared, localKey: key,
		peers: map[string]*peer{}, peerByVIP: map[netip.Addr]*peer{}, peerByPath: map[string]*peer{},
		peerByUDP: map[netip.AddrPort]*peer{}}
	sockPath := filepath.Join(shortDir(t, "elk"), "peer.sock")
	sock, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: sockPath, Net: "unixgram"})
	if err != nil {
		t.Fatal(err)
	}
	defer sock.Close()
	regular := filepath.Join(shared, "not-a-socket")
	_ = os.WriteFile(regular, nil, 0o600)
	record := func(inst, dataSocket, vip, localKey string) {
		t.Helper()
		r := Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000", Instance: inst, Role: roleMember,
			LocalKey: localKey, OSPID: os.Getpid(), DataSocket: dataSocket, VIP: vip}
		if err := writeJSONAtomic(filepath.Join(shared, "brokers", inst+".json"), r); err != nil {
			t.Fatal(err)
		}
	}
	member := func(n int, vip, lan string) MemberInfo {
		return MemberInfo{Instance: instanceID(n), PID: uint32(70000 + n), VIP: vip, LANIP: lan, UDPPort: 40000}
	}
	check := func(name string, n int, wantLocal bool, wantPath string, wantUDP bool) {
		t.Helper()
		b.mu.RLock()
		p := b.peers[instanceID(n)]
		b.mu.RUnlock()
		switch {
		case wantLocal && (p == nil || !p.local || p.dataSocket != wantPath):
			t.Fatalf("%s: want local via %s, got %+v", name, wantPath, p)
		case !wantLocal && wantUDP && (p == nil || p.local):
			t.Fatalf("%s: want remote UDP peer, got %+v", name, p)
		case !wantLocal && !wantUDP && p != nil:
			t.Fatalf("%s: want no peer, got %+v", name, p)
		}
	}

	b.upsertPeer(member(1, "10.253.0.1", "127.0.0.1"), 1)
	check("loopback endpoint in production", 1, false, "", false)
	b.opt.allowLoopback = true
	b.upsertPeer(member(1, "10.253.0.1", "127.0.0.1"), 1)
	check("loopback endpoint under the test option", 1, false, "", true)
	b.opt.allowLoopback = false

	b.upsertPeer(member(2, "10.253.0.2", "192.168.50.7"), 1)
	check("LAN peer without a local record", 2, false, "", true)

	record(instanceID(3), sockPath, "10.253.0.3", key)
	b.upsertPeer(member(3, "10.253.0.3", "192.168.50.8"), 1)
	check("owned live record", 3, true, sockPath, false)

	record(instanceID(4), regular, "10.253.0.4", key)
	b.upsertPeer(member(4, "10.253.0.4", "192.168.50.9"), 1)
	check("record naming a non-socket", 4, false, "", true)

	record(instanceID(5), sockPath, "10.253.0.5", instanceID(0xdef))
	b.upsertPeer(member(5, "10.253.0.5", "192.168.50.10"), 1)
	check("record of another host key", 5, false, "", true)

	record(instanceID(6), sockPath, "10.253.0.66", key)
	b.upsertPeer(member(6, "10.253.0.6", "192.168.50.11"), 1)
	check("record with a different VIP", 6, false, "", true)

	// The coordinator protocol cannot carry a socket path or host key at all.
	for _, line := range []string{
		`{"op":"member","member":{"instance":"` + instanceID(7) + `","pid":7,"vip":"10.253.0.7","data_socket":"/dev/log"}}`,
		`{"op":"members","members":[{"instance":"` + instanceID(7) + `","pid":7,"vip":"10.253.0.7","local_key":"` + key + `"}]}`,
	} {
		var ev coordEvent
		if strictUnmarshal([]byte(line), &ev) == nil {
			t.Fatalf("coordinator event with same-host fields decoded: %s", line)
		}
	}
	var pre coordPreface
	if strictUnmarshal([]byte(`{"v":3,"kind":"member","title":"01006A800016E000","instance":"`+instanceID(7)+`","data_socket":"/dev/log"}`), &pre) == nil {
		t.Fatal("member preface with a socket path decoded")
	}
}
