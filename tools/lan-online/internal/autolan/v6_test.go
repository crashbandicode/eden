package autolan

import (
	"bufio"
	"crypto/tls"
	"encoding/json"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// Regression tests for TRANSPORT_REVIEW.md V1 (bounded startup), V2 (instance
// takeover) and V3 (protocol-checked probe). Loopback only.

// productionOptions keeps the harness wiring but production timings:
// DiscoveryWindow 1 s, LeaderWait 3 s, MemberTimeout 5 s, LeaderTimeout 3 s,
// PruneAfter 10 s, LeaderCooldown 30 s, join attempt 1 s, startup budget 5 s,
// LAN dial 750 ms and the server TLS handshake 2 s.
func (w *world) productionOptions(h host, instance string, controlTLS bool) Options {
	o := w.options(h, instance)
	o.DiscoveryWindow, o.LeaderWait, o.MemberTimeout, o.LeaderTimeout, o.PruneAfter = 0, 0, 0, 0, 0
	o.ControlTLS = controlTLS
	return o
}

// pretendingEndpoint accepts TCP and then stays silent; with cfg it first
// completes TLS with that certificate. It never answers a preface.
func pretendingEndpoint(t *testing.T, cfg *tls.Config) netip.AddrPort {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			t.Cleanup(func() { _ = c.Close() })
			if cfg != nil {
				go func() { _ = tls.Server(c, cfg).Handshake() }()
			}
		}
	}()
	return ln.Addr().(*net.TCPAddr).AddrPort()
}

func TestStartupReadyWithinBudgetUnderPretendingPeers(t *testing.T) {
	someFP := strings.Repeat("5a", 32)
	for _, sc := range []struct {
		name      string
		tls       bool
		endpoint  func(t *testing.T) (netip.AddrPort, string)
		candidate bool // also a lowest-instance candidate beacon that never leads
	}{
		{"silent-drop", false, func(t *testing.T) (netip.AddrPort, string) { return blackHoleEndpoint(t), "" }, false},
		{"silent-drop-tls", true, func(t *testing.T) (netip.AddrPort, string) { return blackHoleEndpoint(t), someFP }, false},
		{"tcp-accept-silent", false, func(t *testing.T) (netip.AddrPort, string) { return pretendingEndpoint(t, nil), "" }, false},
		{"tls-handshake-silent", true, func(t *testing.T) (netip.AddrPort, string) { return pretendingEndpoint(t, nil), someFP }, false},
		{"tls-registration-silent", true, func(t *testing.T) (netip.AddrPort, string) {
			cfg, fp, err := newControlTLS()
			if err != nil {
				t.Fatal(err)
			}
			return pretendingEndpoint(t, cfg), fp
		}, false},
		{"silent-drop-and-stuck-candidate", false, func(t *testing.T) (netip.AddrPort, string) { return blackHoleEndpoint(t), "" }, true},
	} {
		t.Run(sc.name, func(t *testing.T) {
			w := newWorld(t)
			ep, fp := sc.endpoint(t)
			w.hub.announcer(instanceID(1)).Publish(&Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000",
				Instance: instanceID(1), Role: roleLeader, Epoch: 77, TCP: ep.String(), TLS: sc.tls, TLSFP: fp})
			if sc.candidate {
				w.hub.announcer(instanceID(2)).Publish(&Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000",
					Instance: instanceID(2), Role: roleCandidate, TLS: sc.tls})
			}
			// Y starts first; X (lower instance) starts 2.5 s later. Both hear the
			// pretending leader. Eden's deadline is 10 s; the oracle is 8 s.
			const limit = 8 * time.Second
			startY := time.Now()
			y := startNode(t, w.productionOptions(w.newHost(1), instanceID(0x0f), sc.tls))
			var readyY, readyX time.Duration
			isReady := func(n *node) bool { // what Eden waits for
				_, err := os.Stat(filepath.Join(n.rd, readyFileName))
				return err == nil
			}
			for time.Since(startY) < 2500*time.Millisecond {
				if readyY == 0 && isReady(y) {
					readyY = time.Since(startY)
				}
				time.Sleep(10 * time.Millisecond)
			}
			startX := time.Now()
			x := startNode(t, w.productionOptions(w.newHost(2), instanceID(0x05), sc.tls))
			for time.Since(startX) < limit+time.Second && (readyY == 0 || readyX == 0) {
				if readyY == 0 && isReady(y) {
					readyY = time.Since(startY)
				}
				if readyX == 0 && isReady(x) {
					readyX = time.Since(startX)
				}
				time.Sleep(10 * time.Millisecond)
			}
			t.Logf("%s: Y ready after %v, X ready after %v", sc.name, readyY, readyX)
			if readyY == 0 || readyY > limit || readyX == 0 || readyX > limit {
				t.Fatalf("readiness over %v: Y=%v X=%v (0 = never)", limit, readyY, readyX)
			}
		})
	}
}

// registerRaw performs a member registration over TCP from the given loopback
// source address, as any LAN host could.
func registerRaw(t *testing.T, coord netip.AddrPort, from, instance, token string) (net.Conn, *bufio.Reader, coordReply) {
	t.Helper()
	conn, err := dialCoordinator("tcp4", coord.String(), "", netip.MustParseAddr(from), true, nil, time.Now().Add(time.Second))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	p := coordPreface{V: protocolVersion, Kind: "member", Title: "01006A800016E000", Instance: instance,
		LANIP: from, UDPPort: 40000, Token: token}
	if err := (&lineWriter{w: conn}).send(p); err != nil {
		t.Fatal(err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	r := bufio.NewReader(conn)
	line, err := readLine(r, maxCoordLine)
	if err != nil {
		t.Fatalf("registration reply: %v", err)
	}
	var rep coordReply
	if err := json.Unmarshal(line, &rep); err != nil {
		t.Fatal(err)
	}
	_ = conn.SetReadDeadline(time.Time{})
	return conn, r, rep
}

// waitOp reads coordinator events until one with the given op arrives (true)
// or the connection ends (false).
func waitOp(conn net.Conn, r *bufio.Reader, op string, d time.Duration) bool {
	_ = conn.SetReadDeadline(time.Now().Add(d))
	defer conn.SetReadDeadline(time.Time{})
	for {
		line, err := readLine(r, maxCoordLine)
		if err != nil {
			return false
		}
		var ev coordEvent
		if json.Unmarshal(line, &ev) == nil && ev.Op == op {
			return true
		}
	}
}

func TestActiveMemberRegistrationRequiresItsToken(t *testing.T) {
	w := newWorld(t)
	a := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	b := startNode(t, w.options(w.newHost(2), instanceID(2))).waitReady()
	ca := a.b.coordPub.Load()
	coordEP := ca.TCPAddr()

	// Raw protocol: a member, then takeover attempts from another LAN address.
	victim := instanceID(0x33)
	c1, r1, rep1 := registerRaw(t, coordEP, "127.0.0.3", victim, "")
	if !rep1.OK || !isHex32(rep1.Token) {
		t.Fatalf("victim registration: %+v", rep1)
	}
	for _, tok := range []string{"", strings.Repeat("0", 32), "not-a-token"} {
		_, _, rep := registerRaw(t, coordEP, "127.0.0.2", victim, tok)
		if rep.OK || (rep.Error != "instance_active" && rep.Error != "bad_member") {
			t.Fatalf("takeover with token %q: %+v", tok, rep)
		}
	}
	if !waitOp(c1, r1, "ping", 3*time.Second) {
		t.Fatal("the victim's connection was cut by a takeover attempt")
	}
	ca.mu.Lock()
	st := ca.members[victim]
	ca.mu.Unlock()
	if st == nil || st.info.LANIP != "127.0.0.3" || st.info.PID != rep1.PID {
		t.Fatalf("the victim's entry changed: %+v", st)
	}
	// The member itself, reconnecting with its token while its old link is
	// still open (it lost the link on its side), keeps its lease.
	_, _, rep4 := registerRaw(t, coordEP, "127.0.0.3", victim, rep1.Token)
	if !rep4.OK || rep4.PID != rep1.PID || rep4.VIP != rep1.VIP {
		t.Fatalf("token reconnect: %+v", rep4)
	}
	// ...and the coordinator closes the superseded connection.
	_ = c1.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		if _, err := readLine(r1, maxCoordLine); err != nil {
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				t.Fatal("the superseded connection was not closed")
			}
			break
		}
	}

	// Real brokers: an attacker naming B's instance leaves B's link and routes intact.
	eventually(t, 5*time.Second, "B linked", func() bool { return *status(t, b).Connected })
	linkB, _ := b.b.currentLink()
	if got := b.b.memberTokenFor(ca.epoch); got == "" || got != linkB.token {
		t.Fatal("member token not kept for this coordinator lifetime")
	}
	if _, _, rep := registerRaw(t, coordEP, "127.0.0.2", b.b.instance, ""); rep.OK || rep.Error != "instance_active" {
		t.Fatalf("takeover of a live broker: %+v", rep)
	}
	time.Sleep(1500 * time.Millisecond)
	if l, _ := b.b.currentLink(); l != linkB {
		t.Fatal("the live member's link was replaced")
	}
	ga, gb := a.guest("ga", 50700), b.guest("gb", 50700)
	eventually(t, 5*time.Second, "A->B route still to B", func() bool {
		ga.send(gb.ep, []byte("probe"))
		_, _, ok := gb.recv(100 * time.Millisecond)
		return ok
	})
}

// rejectingEndpoint accepts TCP (and with cfg completes TLS), reads one line and
// answers like an incompatible coordinator. lines counts prefaces it received.
func rejectingEndpoint(t *testing.T, cfg *tls.Config, lines *atomic.Int64) netip.AddrPort {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				_ = c.SetDeadline(time.Now().Add(3 * time.Second))
				conn := c
				if cfg != nil {
					tc := tls.Server(c, cfg)
					if tc.Handshake() != nil {
						return
					}
					conn = tc
				}
				if _, err := bufio.NewReader(conn).ReadString('\n'); err != nil {
					return
				}
				lines.Add(1)
				_, _ = conn.Write([]byte(`{"ok":false,"error":"bad_preface"}` + "\n"))
			}()
		}
	}()
	return ln.Addr().(*net.TCPAddr).AddrPort()
}

func TestHealthyLeaderIgnoresLowerEndpointThatRejectsProtocol(t *testing.T) {
	for _, mode := range []struct {
		name string
		tls  bool
	}{{"plaintext", false}, {"tls", true}} {
		t.Run(mode.name, func(t *testing.T) {
			w := newWorld(t)
			optA, optB := w.options(w.newHost(1), instanceID(5)), w.options(w.newHost(2), instanceID(6))
			optA.ControlTLS, optB.ControlTLS = mode.tls, mode.tls
			a := startNode(t, optA).waitReady()
			b := startNode(t, optB).waitReady()
			eventually(t, 5*time.Second, "B linked", func() bool { return a.b.leading.Load() && *status(t, b).Connected })
			epochA := a.b.coordPub.Load().epoch
			linkB, _ := b.b.currentLink()
			var cfg *tls.Config
			fp := ""
			if mode.tls {
				var err error
				if cfg, fp, err = newControlTLS(); err != nil {
					t.Fatal(err)
				}
			}
			var lines atomic.Int64
			ep := rejectingEndpoint(t, cfg, &lines)
			w.hub.announcer(instanceID(1)).Publish(&Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000",
				Instance: instanceID(1), Role: roleLeader, Epoch: 99, TCP: ep.String(), TLS: mode.tls, TLSFP: fp})
			eventually(t, 5*time.Second, "probe answered with a refusal and the endpoint excluded", func() bool {
				r := status(t, a)
				return lines.Load() > 0 && *r.CoordinatorUnreachable
			})
			time.Sleep(time.Second)
			if c := a.b.coordPub.Load(); !a.b.leading.Load() || c == nil || c.epoch != epochA {
				t.Fatal("the healthy leader stepped down for an endpoint that rejects the protocol")
			}
			if l, _ := b.b.currentLink(); l != linkB {
				t.Fatal("a member's link was reset")
			}
		})
	}
}
