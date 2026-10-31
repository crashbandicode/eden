package autolan

import (
	"bufio"
	"context"
	"crypto/tls"
	"errors"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestLateJoinerDoesNotStealLeadership(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	high := startNode(t, w.options(h, instanceID(0xff))).waitReady()
	if !high.b.leading.Load() {
		t.Fatal("first broker did not lead")
	}
	low := startNode(t, w.options(h, instanceID(0x01))).waitReady()
	time.Sleep(time.Second) // several step-down check intervals
	if low.b.leading.Load() || !high.b.leading.Load() {
		t.Fatal("a late lower-instance broker took over a healthy coordinator")
	}
	if r := low.control().call(t, `{"op":"status"}`); r.Connected == nil || !*r.Connected {
		t.Fatalf("late joiner not connected: %+v", r)
	}
}

func TestSimultaneousStartElectsLowestInstance(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	opts := []Options{w.options(h, instanceID(0x0c)), w.options(h, instanceID(0x0a)), w.options(h, instanceID(0x0b))}
	nodes := make([]*node, len(opts))
	var wg sync.WaitGroup
	for i := range opts {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			nodes[i] = startNode(t, opts[i])
		}(i)
	}
	wg.Wait()
	seenPID, seenVIP := map[uint32]bool{}, map[netip.Addr]bool{}
	for _, n := range nodes {
		n.waitReady()
		if seenPID[n.ready.AccountID] || seenVIP[n.vip()] {
			t.Fatal("duplicate lease")
		}
		seenPID[n.ready.AccountID], seenVIP[n.vip()] = true, true
	}
	eventually(t, 5*time.Second, "single leader", func() bool {
		l := leaders(nodes...)
		return len(l) == 1 && l[0].b.instance == instanceID(0x0a)
	})
}

func TestCoordinatorLossReelectsWithStableIdentityPortsAndData(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	a := startNode(t, w.options(h, instanceID(1))).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	c := startNode(t, w.options(h, instanceID(3))).waitReady()
	readyB, _ := os.ReadFile(filepath.Join(b.rd, readyFileName))
	readyC, _ := os.ReadFile(filepath.Join(c.rd, readyFileName))
	statB, _ := os.Stat(filepath.Join(b.rd, readyFileName))
	gb := b.guest("gb", 50200)
	gc := c.guest("gc", 50201)
	eventually(t, 5*time.Second, "B->C route", func() bool {
		gb.send(gc.ep, []byte("probe"))
		_, _, ok := gc.recv(100 * time.Millisecond)
		return ok
	})
	gc.drain()
	if !a.b.leading.Load() {
		t.Fatal("lowest instance is not leading")
	}
	a.stop()
	// Gameplay keeps flowing from the cached peer table during the election.
	gb.send(gc.ep, []byte("during-election"))
	if f, _ := gc.mustRecv(time.Second); string(f.Payload) != "during-election" {
		t.Fatalf("payload %q", f.Payload)
	}
	eventually(t, 10*time.Second, "re-election", func() bool {
		r := c.control().call(t, `{"op":"status"}`)
		return b.b.leading.Load() && !c.b.leading.Load() && r.Connected != nil && *r.Connected
	})
	for _, n := range []*node{b, c} {
		r := n.control().call(t, `{"op":"status"}`)
		if r.AccountID != n.ready.AccountID || r.InstanceIP != n.ready.AutoTransport.InstanceIP {
			t.Fatalf("identity changed across re-election: %+v", r)
		}
	}
	nowB, _ := os.ReadFile(filepath.Join(b.rd, readyFileName))
	nowC, _ := os.ReadFile(filepath.Join(c.rd, readyFileName))
	statB2, _ := os.Stat(filepath.Join(b.rd, readyFileName))
	if string(nowB) != string(readyB) || string(nowC) != string(readyC) || !statB2.ModTime().Equal(statB.ModTime()) {
		t.Fatal("ready.json was rewritten or changed (ports/identity must stay stable)")
	}
	gc.send(gb.ep, []byte("after-election"))
	if f, _ := gb.mustRecv(2 * time.Second); string(f.Payload) != "after-election" {
		t.Fatalf("payload %q", f.Payload)
	}
	// The departed instance is pruned once the new coordinator never lists it.
	eventually(t, 6*time.Second, "prune of the old coordinator's instance", func() bool {
		r := c.control().call(t, `{"op":"status"}`)
		return r.Peers != nil && *r.Peers == 1
	})
}

// mockNEX is a labelled transport-level mock: it serves plain HTTPS on the
// coordinator's NEX listeners to observe RemoteAddr and identity calls. It
// does not implement or claim NEX behaviour.
type mockNEX struct {
	mu      sync.Mutex
	remotes map[string][]string
	ids     map[uint32]bool
	kicked  []uint32
}

func (m *mockNEX) Serve(ctx context.Context, a, s net.Listener, tlsCfg *tls.Config) error {
	mk := func(service string) *http.Server {
		return &http.Server{TLSConfig: tlsCfg, TLSNextProto: map[string]func(*http.Server, *tls.Conn, http.Handler){},
			Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				m.mu.Lock()
				m.remotes[service] = append(m.remotes[service], r.RemoteAddr)
				m.mu.Unlock()
				_, _ = io.WriteString(w, service)
			})}
	}
	sa, ss := mk("auth"), mk("secure")
	go func() { _ = sa.ServeTLS(a, "", "") }()
	go func() { _ = ss.ServeTLS(s, "", "") }()
	<-ctx.Done()
	_ = sa.Close()
	_ = ss.Close()
	return nil
}
func (m *mockNEX) AddIdentity(pid uint32) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.ids[pid] = true
	return nil
}
func (m *mockNEX) RemoveIdentity(pid uint32) {
	m.mu.Lock()
	defer m.mu.Unlock()
	delete(m.ids, pid)
}
func (m *mockNEX) KickPID(pid uint32) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.kicked = append(m.kicked, pid)
}

// httpsVia performs one HTTPS request through a broker's loopback listener, as
// the guest would after Eden's port redirect (guests do not verify the cert).
func httpsVia(t *testing.T, port uint16) (string, error) {
	t.Helper()
	conn, err := tls.Dial("tcp4", "127.0.0.1:"+strconv.Itoa(int(port)), &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		return "", err
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := conn.Write([]byte("GET / HTTP/1.1\r\nHost: 10.254.0.1\r\nConnection: close\r\n\r\n")); err != nil {
		return "", err
	}
	resp, err := http.ReadResponse(bufio.NewReader(conn), nil)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return strconv.Itoa(resp.StatusCode) + " " + string(b), nil
}

func TestStreamsCarryLeasedVirtualRemoteAddr(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	mock := &mockNEX{remotes: map[string][]string{}, ids: map[uint32]bool{}}
	optA := w.options(h, instanceID(1))
	optA.coordHost = func(*Coordinator) (nexHost, error) { return mock, nil }
	a := startNode(t, optA).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	if got, err := httpsVia(t, b.ready.PortRedirects[0].To); err != nil || got != "200 auth" {
		t.Fatalf("auth via B: %q %v", got, err)
	}
	if got, err := httpsVia(t, b.ready.PortRedirects[0].To); err != nil || got != "200 auth" {
		t.Fatalf("second auth via B: %q %v", got, err)
	}
	if got, err := httpsVia(t, a.ready.PortRedirects[1].To); err != nil || got != "200 secure" {
		t.Fatalf("secure via A: %q %v", got, err)
	}
	mock.mu.Lock()
	auth, secure := append([]string(nil), mock.remotes["auth"]...), append([]string(nil), mock.remotes["secure"]...)
	ids := len(mock.ids)
	mock.mu.Unlock()
	if len(auth) != 2 || len(secure) != 1 || ids != 2 {
		t.Fatalf("observed auth=%v secure=%v ids=%d", auth, secure, ids)
	}
	for _, ra := range auth {
		if ap := netip.MustParseAddrPort(ra); ap.Addr() != b.vip() || ap.Port() < 1024 {
			t.Fatalf("B stream RemoteAddr %s, want %s:<unique>", ra, b.vip())
		}
	}
	if auth[0] == auth[1] {
		t.Fatal("concurrent-lifetime streams must get distinct virtual ports")
	}
	if ap := netip.MustParseAddrPort(secure[0]); ap.Addr() != a.vip() {
		t.Fatalf("A stream RemoteAddr %s", secure[0])
	}
	b.stop()
	eventually(t, 5*time.Second, "identity removal on leave", func() bool {
		mock.mu.Lock()
		defer mock.mu.Unlock()
		return len(mock.ids) == 1 && !mock.ids[b.ready.AccountID] && len(mock.kicked) == 1 && mock.kicked[0] == b.ready.AccountID
	})
}

func TestRealNEXHostServesTLSThroughLocalListener(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	// Real nsoserver handler: a non-upgrade request is answered 404 by the WSS handler.
	if got, err := httpsVia(t, n.ready.PortRedirects[0].To); err != nil || !strings.HasPrefix(got, "404") {
		t.Fatalf("auth listener: %q %v", got, err)
	}
	if got, err := httpsVia(t, n.ready.PortRedirects[1].To); err != nil || !strings.HasPrefix(got, "404") {
		t.Fatalf("secure listener: %q %v", got, err)
	}
	host, ok := n.b.coordPub.Load().host.(*realNEX)
	if !ok {
		t.Fatal("production coordinator is not hosting the real NEX server")
	}
	if pid, _, res := host.s.Resolver.Resolve(strconv.FormatUint(uint64(n.ready.AccountID), 10), nil); pid != uint64(n.ready.AccountID) || res != 0 {
		t.Fatalf("leased identity not registered: pid=%d res=%#x", pid, res)
	}
	if pid, _, _ := host.s.Resolver.Resolve("49153", nil); pid != 0 {
		t.Fatal("unregistered fixture username accepted in dynamic mode")
	}
}

func TestZNCAnswers503Locally(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	if n.ready.PortRedirects[2].Address != "10.254.0.2" || n.ready.PortRedirects[2].From != 443 {
		t.Fatalf("ZNC redirect %+v", n.ready.PortRedirects[2])
	}
	conn, err := tls.Dial("tcp4", "127.0.0.1:"+strconv.Itoa(int(n.ready.PortRedirects[2].To)), &tls.Config{InsecureSkipVerify: true})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_, _ = conn.Write([]byte("GET /v1/secret?q=1 HTTP/1.1\r\nHost: " + zncHost + "\r\n\r\n"))
	line, err := bufio.NewReader(conn).ReadString('\n')
	if err != nil || !strings.HasPrefix(line, "HTTP/1.1 503 ") {
		t.Fatalf("ZNC reply %q %v", line, err)
	}
}

func TestReadyFileSchema(t *testing.T) {
	w := newWorld(t)
	n := startNode(t, w.options(w.newHost(1), instanceID(1))).waitReady()
	data, _ := os.ReadFile(filepath.Join(n.rd, readyFileName))
	var rf readyFile
	if err := strictUnmarshal(data[:len(data)-1], &rf); err != nil {
		t.Fatalf("ready.json does not match the contract exactly: %v", err)
	}
	if rf.DNS[supportedTitles["01006A800016E000"].GameServerHost] != "10.254.0.1" || rf.DNS[nncs1Host] != "10.254.0.1" ||
		rf.DNS[nncs2Host] != "10.254.0.2" || rf.DNS[zncHost] != "10.254.0.2" || len(rf.DNS) != 4 {
		t.Fatalf("dns %v", rf.DNS)
	}
	// Exactly the two services, the redirect target and this instance's own VIP;
	// no peer VIP is ever listed.
	if len(rf.AllowedIPs) != 4 || rf.AllowedIPs[0] != "10.254.0.1" || rf.AllowedIPs[1] != "10.254.0.2" ||
		rf.AllowedIPs[2] != "127.0.0.1" || rf.AllowedIPs[3] != rf.AutoTransport.InstanceIP {
		t.Fatalf("allowed_ips %v (instance %s)", rf.AllowedIPs, rf.AutoTransport.InstanceIP)
	}
	want := []struct {
		addr string
		from uint16
	}{{"10.254.0.1", 443}, {"10.254.0.1", 60005}, {"10.254.0.2", 443}}
	for i, r := range rf.PortRedirects {
		if r.Address != want[i].addr || r.From != want[i].from || r.TargetAddress != "127.0.0.1" || r.To == 0 {
			t.Fatalf("redirect %d %+v", i, r)
		}
	}
	if rf.AutoTransport.ControlSocket != filepath.Join(n.rd, "control.sock") || rf.AutoTransport.DatagramSocket != filepath.Join(n.rd, "data.sock") {
		t.Fatalf("transport %+v", rf.AutoTransport)
	}
	assertPrivateFile(t, filepath.Join(n.rd, readyFileName))
}

func TestRunReportsFatalStatus(t *testing.T) {
	w := newWorld(t)
	run := func(opt Options) (*StatusError, string) {
		t.Helper()
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		err := Run(ctx, opt)
		var se *StatusError
		if !errors.As(err, &se) {
			t.Fatalf("expected a status error, got %v", err)
		}
		data, _ := os.ReadFile(filepath.Join(opt.RuntimeDir, errorFileName))
		if _, err := os.Stat(filepath.Join(opt.RuntimeDir, readyFileName)); err == nil {
			t.Fatal("ready.json published despite a fatal error")
		}
		return se, string(data)
	}
	h := w.newHost(1)
	opt := w.options(h, instanceID(1))
	opt.AssetsDir = shortDir(t, "elm")
	if se, file := run(opt); se.Code != "missing_assets" || !strings.Contains(file, `"code":"missing_assets"`) {
		t.Fatalf("missing assets: %v %s", se, file)
	}
	opt = w.options(h, instanceID(1))
	opt.TitleID = "0100000000000000"
	if se, file := run(opt); se.Code != "unsupported_title" || !strings.Contains(file, `"unsupported_title"`) {
		t.Fatalf("unsupported title: %v %s", se, file)
	}
	opt = w.options(h, instanceID(1))
	makeDirUntrusted(t, opt.RuntimeDir)
	if err := Run(context.Background(), opt); err == nil || !strings.HasPrefix(err.Error(), "runtime_dir_invalid") {
		t.Fatalf("0755 runtime dir: %v", err)
	}
	if _, err := os.Stat(filepath.Join(opt.RuntimeDir, errorFileName)); err == nil {
		t.Fatal("status written into an untrusted runtime directory")
	}
	// A shared directory too deep for the coordinator's AF_UNIX path fails before
	// any election, with a status Eden can show (observed in a CLI smoke run).
	opt = w.options(h, instanceID(1))
	deep := filepath.Join(shortDir(t, "eld"), strings.Repeat("d", 60))
	if err := os.Mkdir(deep, 0o700); err != nil {
		t.Fatal(err)
	}
	opt.SharedDir = filepath.Join(deep, "shared")
	if se, file := run(opt); se.Code != "listen_failed" || !strings.Contains(file, `"listen_failed"`) {
		t.Fatalf("deep shared directory: %v %s", se, file)
	}
	opt = w.options(h, instanceID(1))
	t.Setenv("NEXTENDO_AUTH_RECALL_FILE", "/tmp/should-not-be-used")
	if se, _ := run(opt); se.Code != "bad_environment" {
		t.Fatalf("persistence env: %v", se)
	}
}

func TestParseArgs(t *testing.T) {
	good := []string{"--parent-pid", "4242", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle, "--interface", "eth0"}
	o, err := ParseArgs(good)
	if err != nil || o.ParentPID != 4242 || o.RuntimeDir != "/r" || o.AssetsDir != "/a" || o.Interface != "eth0" || o.LocalOnly {
		t.Fatalf("%+v %v", o, err)
	}
	local := []string{"--parent-pid", "4242", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle, "--local-only"}
	if o, err := ParseArgs(local); err != nil || !o.LocalOnly || o.Interface != "" {
		t.Fatalf("--local-only: %+v %v", o, err)
	}
	if _, err := ParseArgs(append(local, "--interface", "eth0")); err == nil {
		t.Fatal("--local-only together with --interface accepted")
	}
	if _, err := ParseArgs(append(local[:len(local)-1], "--local-only=maybe")); err == nil {
		t.Fatal("non-boolean --local-only accepted")
	}
	for _, bad := range [][]string{
		{"--parent-pid", "x", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle},
		{"--parent-pid", "1", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle},
		{"--parent-pid", "4242", "--assets", "/a", "--title-id", testTitle},
		{"--parent-pid", "4242", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle, "extra"},
		{"--parent-pid", "4242", "--runtime-dir", "/r", "--assets", "/a", "--title-id", testTitle, "--unknown", "1"},
	} {
		if _, err := ParseArgs(bad); err == nil {
			t.Fatalf("accepted %v", bad)
		}
	}
}

func TestNetworkBeaconValidation(t *testing.T) {
	self := instanceID(9)
	leader := `{"eln":"eden-nsolan-auto","v":4,"title":"01006A800016E000","instance":"` + instanceID(1) + `","role":"leader","tcp":"192.168.10.20:41000"}`
	from := netip.MustParseAddrPort("192.168.10.20:50731")
	if _, ok := parseNetworkBeacon([]byte(leader), from, self, false); !ok {
		t.Fatal("valid leader beacon rejected")
	}
	loopLeader := strings.Replace(leader, "192.168.10.20:41000", "127.0.0.1:41000", 1)
	loopFrom := netip.MustParseAddrPort("127.0.0.1:50731")
	if _, ok := parseNetworkBeacon([]byte(loopLeader), loopFrom, self, true); !ok {
		t.Fatal("loopback beacon rejected under the test option")
	}
	for name, c := range map[string]struct {
		data string
		from netip.AddrPort
	}{
		"public source":      {leader, netip.MustParseAddrPort("8.8.8.8:50731")},
		"loopback (prod)":    {loopLeader, loopFrom},
		"spoofed tcp":        {leader, netip.MustParseAddrPort("192.168.10.21:50731")},
		"unix path":          {strings.Replace(leader, `"role"`, `"unix":"/tmp/x","role"`, 1), from},
		"host key":           {strings.Replace(leader, `"role"`, `"local_key":"` + instanceID(7) + `","role"`, 1), from},
		"data socket":        {strings.Replace(leader, `"role"`, `"data_socket":"/tmp/x.sock","role"`, 1), from},
		"vip":                {strings.Replace(leader, `"role"`, `"vip":"10.253.1.2","role"`, 1), from},
		"member role":        {strings.Replace(strings.Replace(leader, `"leader"`, `"member"`, 1), `,"tcp":"192.168.10.20:41000"`, "", 1), from},
		"self":               {strings.Replace(leader, instanceID(1), self, 1), from},
		"old version":        {strings.Replace(leader, `"v":4`, `"v":3`, 1), from},
		"unknown field":      {strings.Replace(leader, `"role"`, `"x":1,"role"`, 1), from},
		"candidate with tcp": {strings.Replace(leader, `"leader"`, `"candidate"`, 1), from},
	} {
		if _, ok := parseNetworkBeacon([]byte(c.data), c.from, self, false); ok {
			t.Errorf("%s accepted", name)
		}
	}
	// What leaves the host: networkCopy strips every same-host field.
	full := &Beacon{Proto: beaconProto, V: protocolVersion, Title: "01006A800016E000", Instance: instanceID(1), Role: roleLeader,
		TCP: "192.168.10.20:41000", Unix: "/x/coord.sock", LocalKey: instanceID(7), OSPID: 9, DataSocket: "/x/data.sock", VIP: "10.253.1.2"}
	c, ok := networkCopy(full)
	if !ok || c.Unix != "" || c.LocalKey != "" || c.OSPID != 0 || c.DataSocket != "" || c.VIP != "" || c.TCP == "" {
		t.Fatalf("network copy leaks same-host data: %+v", c)
	}
	if _, ok := networkCopy(&Beacon{Role: roleMember}); ok {
		t.Fatal("a member record was given a network form")
	}
}
