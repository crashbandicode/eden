package autolan

import (
	"bufio"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/netip"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// Optional control TLS (--control-tls). All "cross-host" cases run on loopback
// through the unexported allowLoopback test option; no LAN traffic.

func TestControlTLSArgsAndBeaconSchema(t *testing.T) {
	base := []string{"--parent-pid", "4242", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle}
	if o, err := ParseArgs(base); err != nil || o.ControlTLS {
		t.Fatalf("default: %+v %v", o, err)
	}
	if o, err := ParseArgs(append(append([]string(nil), base...), "--control-tls", "--local-only")); err != nil || !o.ControlTLS || !o.LocalOnly {
		t.Fatalf("--control-tls: %+v %v", o, err)
	}
	fp := strings.Repeat("0f", 32)
	leader := Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000", Instance: instanceID(1),
		Role: roleLeader, TCP: "192.168.10.20:41000"}
	for name, c := range map[string]struct {
		mut  func(*Beacon)
		want bool
	}{
		"plaintext leader":           {func(*Beacon) {}, true},
		"TLS leader with pin":        {func(b *Beacon) { b.TLS, b.TLSFP = true, fp }, true},
		"TLS leader without pin":     {func(b *Beacon) { b.TLS = true }, false},
		"plaintext leader with pin":  {func(b *Beacon) { b.TLSFP = fp }, false},
		"upper-case pin":             {func(b *Beacon) { b.TLS, b.TLSFP = true, strings.ToUpper(fp) }, false},
		"TLS candidate":              {func(b *Beacon) { b.Role, b.TCP, b.TLS = roleCandidate, "", true }, true},
		"candidate carrying pin":     {func(b *Beacon) { b.Role, b.TCP, b.TLS, b.TLSFP = roleCandidate, "", true, fp }, false},
	} {
		b := leader
		c.mut(&b)
		if b.valid() != c.want {
			t.Errorf("%s: valid=%v want %v", name, !c.want, c.want)
		}
	}
	tlsLeader := leader
	tlsLeader.TLS, tlsLeader.TLSFP, tlsLeader.LocalKey = true, fp, instanceID(7)
	nc, _ := networkCopy(&tlsLeader)
	if !nc.TLS || nc.TLSFP != fp || nc.LocalKey != "" {
		t.Fatalf("network copy: %+v", nc)
	}
	data, _ := json.Marshal(nc)
	if got, ok := parseNetworkBeacon(data, netip.MustParseAddrPort("192.168.10.20:50731"), instanceID(9), false); !ok || !got.TLS || got.TLSFP != fp {
		t.Fatalf("TLS leader beacon rejected: %s", data)
	}
	// Plaintext beacons keep the v4 wire form (no TLS fields at all).
	plain, _ := networkCopy(&leader)
	if data, _ := json.Marshal(plain); strings.Contains(string(data), "tls") {
		t.Fatalf("plaintext beacon carries TLS fields: %s", data)
	}
}

func tlsOptions(w *world, h host, instance string) Options {
	o := w.options(h, instance)
	o.ControlTLS = true
	return o
}

// tlsLinkState returns the TLS state of a broker's current coordinator link,
// or false when the link is not TLS.
func tlsLinkState(n *node) (tls.ConnectionState, bool) {
	l, _ := n.b.currentLink()
	if l == nil {
		return tls.ConnectionState{}, false
	}
	tc, ok := l.conn.(*tls.Conn)
	if !ok {
		return tls.ConnectionState{}, false
	}
	return tc.ConnectionState(), true
}

func TestControlTLSCrossHostPinnedSameHostUnixAndRestart(t *testing.T) {
	w := newWorld(t)
	h1, h2 := w.newHost(1), w.newHost(2)
	a := startNode(t, tlsOptions(w, h1, instanceID(1))).waitReady()
	b := startNode(t, tlsOptions(w, h1, instanceID(2))).waitReady()
	c := startNode(t, tlsOptions(w, h2, instanceID(3))).waitReady()
	coord := a.b.coordPub.Load()
	if !a.b.leading.Load() || coord == nil || !isHex64(coord.TLSFingerprint()) {
		t.Fatal("A is not a TLS coordinator")
	}
	fp1 := coord.TLSFingerprint()
	eventually(t, 5*time.Second, "members linked", func() bool {
		rb, rc := status(t, b), status(t, c)
		return *rb.Connected && *rc.Connected
	})
	if l, _ := b.b.currentLink(); l.conn.RemoteAddr().Network() != "unix" {
		t.Fatal("same-host member did not use Unix IPC")
	}
	st, ok := tlsLinkState(c)
	if !ok || st.Version != tls.VersionTLS13 || st.NegotiatedProtocol != controlALPN ||
		len(st.PeerCertificates) == 0 || certFingerprint(st.PeerCertificates[0].Raw) != fp1 {
		t.Fatalf("cross-host link is not TLS 1.3 pinned to the advertised certificate: ok=%v", ok)
	}
	for _, n := range []*node{a, b, c} {
		if r := status(t, n); r.ControlTLS == nil || !*r.ControlTLS {
			t.Fatalf("status does not report control TLS: %+v", r)
		}
	}
	// A NEX stream from the cross-host member reaches the real handler through the
	// TLS control connection.
	if got, err := httpsVia(t, c.ready.PortRedirects[0].To); err != nil || !strings.HasPrefix(got, "404") {
		t.Fatalf("NEX stream over control TLS: %q %v", got, err)
	}
	if coord.tlsHandshakeFailures.Load() != 0 || coord.tcpPrefaces.Load() < 2 {
		t.Fatalf("handshake failures=%d tcp prefaces=%d", coord.tlsHandshakeFailures.Load(), coord.tcpPrefaces.Load())
	}
	// Gameplay is unchanged: same host over AF_UNIX, cross host over UDP.
	ga, gb, gc := a.guest("ga", 50600), b.guest("gb", 50600), c.guest("gc", 50600)
	for _, pair := range [][2]*guest{{ga, gb}, {ga, gc}} {
		eventually(t, 5*time.Second, "route", func() bool {
			pair[0].send(pair[1].ep, []byte("probe"))
			_, _, ok := pair[1].recv(100 * time.Millisecond)
			return ok
		})
	}
	if cnt := status(t, a).Counters; cnt.UnixPeerFrames == 0 || cnt.UDPPeerFrames == 0 {
		t.Fatalf("datagram paths changed: %+v", cnt)
	}
	// A restarted coordinator advertises a new certificate; members follow it.
	epoch1 := coord.epoch
	coord.cancel()
	coord = nil
	eventually(t, 10*time.Second, "TLS coordinator restart", func() bool {
		c2 := a.b.coordPub.Load()
		st, ok := tlsLinkState(c)
		return c2 != nil && c2.epoch != epoch1 && a.b.leading.Load() && ok && len(st.PeerCertificates) > 0 &&
			certFingerprint(st.PeerCertificates[0].Raw) == c2.TLSFingerprint()
	})
	if fp2 := a.b.coordPub.Load().TLSFingerprint(); fp2 == fp1 || !isHex64(fp2) {
		t.Fatal("restarted coordinator reused its certificate")
	}
}

func TestControlTLSModesNeverMix(t *testing.T) {
	w := newWorld(t)
	h1, h2 := w.newHost(1), w.newHost(2)
	a := startNode(t, tlsOptions(w, h1, instanceID(1))).waitReady()   // TLS
	b := startNode(t, w.options(h2, instanceID(2))).waitReady()       // plaintext, other host
	c := startNode(t, w.options(h1, instanceID(3))).waitReady()       // plaintext, A's host
	eventually(t, 5*time.Second, "two separate domains", func() bool {
		rc := status(t, c)
		return a.b.leading.Load() && b.b.leading.Load() && !c.b.leading.Load() && *rc.Connected
	})
	// The other mode is invisible to discovery: no broker even attempted (and
	// failed) a join across modes. Coordinator- and dial-side mode checks back
	// this up, so it is asserted separately.
	time.Sleep(time.Second)
	for _, n := range []*node{a, b, c} {
		if r := status(t, n); r.CoordinatorUnreachable == nil || *r.CoordinatorUnreachable {
			t.Fatalf("a broker tried to join across control-TLS modes: %+v", r)
		}
	}
	ca, cb := a.b.coordPub.Load(), b.b.coordPub.Load()
	if ca.memberCount() != 1 || cb.memberCount() != 2 {
		t.Fatalf("modes mixed: TLS members=%d plaintext members=%d", ca.memberCount(), cb.memberCount())
	}
	if l, _ := c.b.currentLink(); l.conn.RemoteAddr().Network() != "tcp" {
		t.Fatal("plaintext broker joined the same-host TLS coordinator")
	}
	if _, isTLS := tlsLinkState(c); isTLS {
		t.Fatal("plaintext broker used TLS")
	}
	if r := status(t, a); r.Peers == nil || *r.Peers != 0 {
		t.Fatalf("TLS broker learned plaintext peers: %+v", r)
	}
	// Protocol level: a plaintext preface on the TLS port gets no JSON reply.
	raw, err := dialCoordinator("tcp4", ca.TCPAddr().String(), h1.shared, netip.Addr{}, true, nil, time.Now().Add(time.Second)) // plaintext on purpose
	if err != nil {
		t.Fatal(err)
	}
	before := ca.tlsHandshakeFailures.Load()
	_, _ = raw.Write([]byte(`{"v":3,"kind":"member","title":"01006A800016E000","instance":"` + instanceID(9) + `","lan_ip":"127.0.0.1","udp_port":1}` + "\n"))
	_ = raw.SetReadDeadline(time.Now().Add(5 * time.Second))
	reply, _ := io.ReadAll(raw)
	_ = raw.Close()
	if strings.Contains(string(reply), "{") {
		t.Fatalf("TLS coordinator answered a plaintext preface: %q", reply)
	}
	eventually(t, 3*time.Second, "handshake failure counted", func() bool { return ca.tlsHandshakeFailures.Load() > before })
	if ca.memberCount() != 1 {
		t.Fatal("plaintext preface registered a member")
	}
	// A preface declaring the other mode is refused even over Unix.
	u, err := dialCoordinator("unix", cb.unixPath, h2.shared, netip.Addr{}, false, nil, time.Now().Add(time.Second))
	if err != nil {
		t.Fatal(err)
	}
	defer u.Close()
	_, _ = u.Write([]byte(`{"v":3,"kind":"member","title":"01006A800016E000","instance":"` + instanceID(9) + `","tls":true}` + "\n"))
	_ = u.SetReadDeadline(time.Now().Add(5 * time.Second))
	if line, _ := bufio.NewReader(u).ReadString('\n'); !strings.Contains(line, `"bad_preface"`) {
		t.Fatalf("mode-mismatched preface: %q", line)
	}
}

func TestControlTLSWrongPinRefusedBeforePreface(t *testing.T) {
	w := newWorld(t)
	a := startNode(t, tlsOptions(w, w.newHost(1), instanceID(5))).waitReady()
	ca := a.b.coordPub.Load()
	wrong := strings.Repeat("ab", 32)
	if wrong == ca.TLSFingerprint() {
		t.Fatal("test fingerprint collision")
	}
	// A lower-instance leader beacon that points at A's real TLS endpoint but
	// advertises another certificate.
	w.hub.announcer(instanceID(1)).Publish(&Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000",
		Instance: instanceID(1), Role: roleLeader, TCP: ca.TCPAddr().String(), TLS: true, TLSFP: wrong})
	b := startNode(t, tlsOptions(w, w.newHost(2), instanceID(6))).waitReady()
	eventually(t, 5*time.Second, "fallback to the genuine coordinator", func() bool {
		r := status(t, b)
		return *r.Connected && r.CoordinatorUnreachable != nil && *r.CoordinatorUnreachable
	})
	st, ok := tlsLinkState(b)
	if !ok || certFingerprint(st.PeerCertificates[0].Raw) != ca.TLSFingerprint() {
		t.Fatal("member is not pinned to the genuine coordinator")
	}
	if ca.tlsHandshakeFailures.Load() == 0 {
		t.Fatal("the wrong-pin attempts never reached the TLS listener")
	}
	// Exactly one preface crossed TCP: the member registration after the correct
	// pin. The refused handshakes carried no preface or token.
	if got := ca.tcpPrefaces.Load(); got != 1 {
		t.Fatalf("prefaces over TCP = %d, want 1", got)
	}
}

// tlsTestServer runs the coordinator-side handshake on loopback and records the
// application bytes received after a successful handshake.
type tlsTestServer struct {
	ln       net.Listener
	accepts  atomic.Int64
	mu       sync.Mutex
	received []byte
}

func newTLSTestServer(t *testing.T, cfg *tls.Config) *tlsTestServer {
	t.Helper()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s := &tlsTestServer{ln: ln}
	t.Cleanup(func() { _ = ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			s.accepts.Add(1)
			go func() {
				defer conn.Close()
				_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
				tc := tls.Server(conn, cfg)
				if tc.Handshake() != nil {
					return
				}
				buf := make([]byte, 64)
				n, _ := tc.Read(buf)
				s.mu.Lock()
				s.received = append(s.received, buf[:n]...)
				s.mu.Unlock()
			}()
		}
	}()
	return s
}

func (s *tlsTestServer) got() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return string(s.received)
}

func TestDialCoordinatorTLSPinsBoundsAndNeverDowngrades(t *testing.T) {
	cfg, fp, err := newControlTLS()
	if err != nil {
		t.Fatal(err)
	}
	srv := newTLSTestServer(t, cfg)
	addr := srv.ln.Addr().String()
	dial := func(pin string, timeout time.Duration, to string) (net.Conn, error) {
		return dialCoordinator("tcp4", to, "/nonexistent", netip.Addr{}, true, &controlTLSClient{pin: pin}, time.Now().Add(timeout))
	}
	// Correct pin: TLS 1.3, control ALPN, bytes delivered.
	conn, err := dial(fp, time.Second, addr)
	if err != nil {
		t.Fatalf("pinned dial: %v", err)
	}
	if st := conn.(*tls.Conn).ConnectionState(); st.Version != tls.VersionTLS13 || st.NegotiatedProtocol != controlALPN {
		t.Fatalf("negotiated %x %q", st.Version, st.NegotiatedProtocol)
	}
	_, _ = conn.Write([]byte("preface\n"))
	_ = conn.Close()
	eventually(t, 2*time.Second, "server received", func() bool { return srv.got() == "preface\n" })
	// Wrong pin: refused during the handshake; the server sees no application byte.
	wrong := make([]byte, 32)
	if _, err := dial(hex.EncodeToString(wrong), time.Second, addr); !errors.Is(err, errPinMismatch) {
		t.Fatalf("wrong pin: %v", err)
	}
	time.Sleep(200 * time.Millisecond)
	if srv.got() != "preface\n" {
		t.Fatalf("bytes crossed after a wrong pin: %q", srv.got())
	}
	// Absent or malformed pin: refused before any connection is opened.
	n0 := srv.accepts.Load()
	for _, pin := range []string{"", "not-hex", strings.ToUpper(fp)} {
		if _, err := dial(pin, time.Second, addr); !errors.Is(err, errNoPin) {
			t.Fatalf("pin %q: %v", pin, err)
		}
	}
	if srv.accepts.Load() != n0 {
		t.Fatal("a pin-less TLS dial opened a connection")
	}
	// Destination policy still comes first.
	if _, err := dial(fp, time.Second, "8.8.8.8:443"); !errors.Is(err, errDestinationRefused) {
		t.Fatalf("public destination: %v", err)
	}
	// A silent peer: the handshake is bounded.
	hole, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer hole.Close()
	go func() {
		for {
			c, err := hole.Accept()
			if err != nil {
				return
			}
			defer c.Close()
		}
	}()
	start := time.Now()
	if _, err := dial(fp, 300*time.Millisecond, hole.Addr().String()); err == nil || time.Since(start) > 2*time.Second {
		t.Fatalf("silent peer: err=%v after %v", err, time.Since(start))
	}
	// A plaintext coordinator: the client never falls back to plaintext.
	plain, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer plain.Close()
	go func() {
		for {
			c, err := plain.Accept()
			if err != nil {
				return
			}
			_, _ = c.Write([]byte(`{"ok":false,"error":"bad_preface"}` + "\n"))
			_ = c.Close()
		}
	}()
	if c, err := dial(fp, time.Second, plain.Addr().String()); err == nil {
		_ = c.Close()
		t.Fatal("TLS dial succeeded against a plaintext endpoint")
	}
}
