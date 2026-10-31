package autolan

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"encoding/binary"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"edennsolan/backend/internal/fixture"
	"edennsolan/backend/internal/nsoserver"
)

// nexHost is the NEX fixture hosted by the elected coordinator. Production uses
// the real nsoserver; transport tests may substitute a labelled mock.
type nexHost interface {
	Serve(ctx context.Context, authLn, secureLn net.Listener, tlsCfg *tls.Config) error
	AddIdentity(pid uint32) error
	RemoveIdentity(pid uint32)
	KickPID(pid uint32)
}

type realNEX struct{ s *nsoserver.Server }

func newRealNEX(c *Coordinator) (nexHost, error) {
	cfg, err := fixture.NewDynamic(serviceIP1.String(), c.b.opt.AssetsDir)
	if err != nil {
		return nil, err
	}
	cfg.FighterExchange = c.fighterExchange
	s, err := nsoserver.New(cfg, c.b.logf)
	if err != nil {
		return nil, err
	}
	s.EndpointByPID = c.EndpointByPID
	return &realNEX{s: s}, nil
}

func (r *realNEX) Serve(ctx context.Context, a, s net.Listener, t *tls.Config) error {
	return r.s.Serve(ctx, a, s, t)
}
func (r *realNEX) AddIdentity(pid uint32) error {
	return r.s.Resolver.Add(strconv.FormatUint(uint64(pid), 10), uint64(pid))
}
func (r *realNEX) RemoveIdentity(pid uint32) {
	r.s.Resolver.Remove(strconv.FormatUint(uint64(pid), 10))
}
func (r *realNEX) KickPID(pid uint32) {
	r.s.Secure.KickPID(uint64(pid))
	r.s.Auth.KickPID(uint64(pid))
}

// Coordinator is the elected broker's shared service: lease allocation, the
// membership table, NNCS observations by PID and the NEX endpoints. It never
// carries gameplay datagrams.
type Coordinator struct {
	b        *Broker
	epoch    uint64
	host     nexHost
	authL    *chanListener
	secureL  *chanListener
	unixPath string
	unixLn   *net.UnixListener
	tcpLn    *net.TCPListener
	ctx      context.Context
	cancel   context.CancelFunc
	wg       sync.WaitGroup
	done     chan struct{} // closed when the NEX host stops

	hostStarted bool

	// Control TLS (cross-host TCP only; nil in plaintext mode). The certificate
	// is generated per coordinator start and identified by ctlFP.
	ctlTLS *tls.Config
	ctlFP  string
	// Observability for tests: failed TLS handshakes on the TCP listener, and
	// prefaces actually read from TCP connections.
	tlsHandshakeFailures atomic.Uint64
	tcpPrefaces          atomic.Uint64

	mu         sync.Mutex
	closed     bool
	members    map[string]*memberState
	byPID      map[uint32]string
	byVIP      map[netip.Addr]string
	streams    map[string]map[*vconn]struct{}
	usedPorts  map[netip.Addr]map[uint16]bool
	nextPort   map[netip.Addr]uint16
	fighterSeq atomic.Uint64
}

type memberState struct {
	info           MemberInfo
	vip            netip.Addr
	token          string
	conn           net.Conn
	w              *lineWriter
	obs            netip.AddrPort
	done           chan struct{}
	fighterWaiters map[uint64]chan coordEvent // protected by Coordinator.mu
}

func startCoordinator(b *Broker) (*Coordinator, error) {
	var eb [8]byte
	if _, err := rand.Read(eb[:]); err != nil {
		return nil, err
	}
	ctx, cancel := context.WithCancel(context.Background())
	c := &Coordinator{
		b: b, epoch: binary.BigEndian.Uint64(eb[:]) | 1, ctx: ctx, cancel: cancel,
		done:      make(chan struct{}),
		members:   map[string]*memberState{},
		byPID:     map[uint32]string{},
		byVIP:     map[netip.Addr]string{},
		streams:   map[string]map[*vconn]struct{}{},
		usedPorts: map[netip.Addr]map[uint16]bool{},
		nextPort:  map[netip.Addr]uint16{},
	}
	fail := func(err error) (*Coordinator, error) {
		c.Close()
		return nil, err
	}
	tlsCfg, err := selfSignedTLS(b.titleInfo.GameServerHost, nncs1Host, nncs2Host)
	if err != nil {
		return fail(err)
	}
	if b.opt.ControlTLS {
		if c.ctlTLS, c.ctlFP, err = newControlTLS(); err != nil {
			return fail(err)
		}
	}
	mk := b.opt.coordHost
	if mk == nil {
		mk = newRealNEX
	}
	if c.host, err = mk(c); err != nil {
		return fail(err)
	}
	c.unixPath = filepath.Join(b.sharedDir, "coord-"+b.instance+".sock")
	removeStaleSocket(c.unixPath)
	if c.unixLn, err = net.ListenUnix("unix", &net.UnixAddr{Name: c.unixPath, Net: "unix"}); err != nil {
		return fail(err)
	}
	c.unixLn.SetUnlinkOnClose(true)
	_ = os.Chmod(c.unixPath, 0o600)
	if b.lanIP.IsValid() {
		if c.tcpLn, err = net.ListenTCP("tcp4", net.TCPAddrFromAddrPort(netip.AddrPortFrom(b.lanIP, 0))); err != nil {
			return fail(err)
		}
	}
	c.authL = newChanListener(&net.TCPAddr{IP: serviceIP1.AsSlice(), Port: 443})
	c.secureL = newChanListener(&net.TCPAddr{IP: serviceIP1.AsSlice(), Port: fixture.DefaultSecurePort})
	c.hostStarted = true
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		defer close(c.done)
		if err := c.host.Serve(ctx, c.authL, c.secureL, tlsCfg); err != nil && ctx.Err() == nil {
			b.logf("[coord] NEX host stopped: %v", err)
		}
	}()
	c.acceptLoop(c.unixLn, false)
	if c.tcpLn != nil {
		c.acceptLoop(c.tcpLn, true)
	}
	b.logf("[coord] leading epoch=%d tcp=%v", c.epoch, c.TCPAddr())
	return c, nil
}

// TCPAddr is the LAN coordinator endpoint (invalid in local-only mode).
func (c *Coordinator) TCPAddr() netip.AddrPort {
	if c.tcpLn == nil {
		return netip.AddrPort{}
	}
	return c.tcpLn.Addr().(*net.TCPAddr).AddrPort()
}

// Done is closed when the hosted NEX service has stopped.
func (c *Coordinator) Done() <-chan struct{} { return c.done }

func (c *Coordinator) acceptLoop(ln net.Listener, viaTCP bool) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			c.wg.Add(1)
			go func() {
				defer c.wg.Done()
				if viaTCP && c.ctlTLS != nil {
					// TLS mode: the TCP listener speaks only TLS 1.3. Nothing is read
					// from the connection before the bounded handshake completes.
					tc := tls.Server(conn, c.ctlTLS)
					_ = conn.SetDeadline(time.Now().Add(c.b.opt.tlsHandshakeTimeout))
					if err := tc.Handshake(); err != nil {
						c.tlsHandshakeFailures.Add(1)
						_ = conn.Close()
						return
					}
					conn = tc
				}
				c.handle(conn, viaTCP)
			}()
		}
	}()
}

// TLSFingerprint is the SHA-256 fingerprint of this coordinator's control
// certificate ("" in plaintext mode).
func (c *Coordinator) TLSFingerprint() string { return c.ctlFP }

func (c *Coordinator) handle(conn net.Conn, viaTCP bool) {
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	r := bufio.NewReaderSize(conn, 4096)
	line, err := readLine(r, maxControlLine)
	if err != nil {
		_ = conn.Close()
		return
	}
	if viaTCP {
		c.tcpPrefaces.Add(1)
	}
	var p coordPreface
	if err := strictUnmarshal(line, &p); err != nil || p.V != protocolVersion || p.Title != c.b.title || !isInstanceID(p.Instance) ||
		p.TLS != c.b.opt.ControlTLS {
		writeReply(conn, coordReply{Error: "bad_preface"})
		_ = conn.Close()
		return
	}
	switch p.Kind {
	case "member":
		c.serveMember(conn, r, p, viaTCP)
	case "stream":
		c.serveStream(conn, r, p)
	case "probe":
		// An explicit, protocol-compatible answer (version, title and mode were
		// checked above) naming this coordinator lifetime.
		writeReply(conn, coordReply{OK: true, Epoch: c.epoch})
		_ = conn.Close()
	default:
		writeReply(conn, coordReply{Error: "bad_preface"})
		_ = conn.Close()
	}
}

func writeReply(conn net.Conn, r coordReply) {
	_ = (&lineWriter{w: conn}).send(r)
}

func (c *Coordinator) validMember(p coordPreface, conn net.Conn, viaTCP bool) bool {
	if p.LANIP == "" {
		return !viaTCP && p.UDPPort == 0
	}
	ip, err := netip.ParseAddr(p.LANIP)
	if err != nil || !c.b.lanAddressAllowed(ip) || p.UDPPort == 0 {
		return false
	}
	if viaTCP {
		remote, ok := conn.RemoteAddr().(*net.TCPAddr)
		if !ok || remote.AddrPort().Addr().Unmap() != ip {
			return false // a LAN member must register from its advertised address
		}
	}
	return true
}

func (c *Coordinator) serveMember(conn net.Conn, r *bufio.Reader, p coordPreface, viaTCP bool) {
	defer conn.Close()
	if !c.validMember(p, conn, viaTCP) || (p.Token != "" && !isHex32(p.Token)) {
		writeReply(conn, coordReply{Error: "bad_member"})
		return
	}
	token, err := randomHex(16)
	if err != nil {
		return
	}
	st := &memberState{token: token, conn: conn, w: &lineWriter{w: conn, conn: conn}, done: make(chan struct{}), fighterWaiters: map[uint64]chan coordEvent{}}
	defer close(st.done)

	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "closing"})
		return
	}
	// Instance IDs are public (beacons, snapshots). A live entry is replaced only
	// by a registration that presents that entry's current token: the member
	// itself reconnecting after losing its link. Anyone else is refused and the
	// live member's connection, lease and routes stay untouched. A new
	// coordinator has no entries, so re-registration after re-election is free.
	if old := c.members[p.Instance]; old != nil &&
		subtle.ConstantTimeCompare([]byte(old.token), []byte(p.Token)) != 1 {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "instance_active"})
		c.b.logf("[coord] refused a registration for an active member without its token")
		return
	}
	lease, ok := c.leaseFor(p)
	if !ok {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "lease_conflict"})
		c.b.logf("[coord] lease conflict for a re-registering member")
		return
	}
	vip := netip.MustParseAddr(lease.VIP)
	if err := c.host.AddIdentity(lease.PID); err != nil {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "lease_conflict"})
		return
	}
	if old := c.members[p.Instance]; old != nil {
		_ = old.conn.Close() // the token holder replaces its own stale link
	}
	st.vip = vip
	st.info = MemberInfo{Instance: p.Instance, PID: lease.PID, VIP: lease.VIP, LANIP: p.LANIP, UDPPort: p.UDPPort}
	c.members[p.Instance] = st
	c.byPID[lease.PID] = p.Instance
	c.byVIP[vip] = p.Instance
	_ = conn.SetDeadline(time.Time{})
	_ = st.w.send(coordReply{OK: true, PID: lease.PID, VIP: lease.VIP, Token: token, Epoch: c.epoch})
	snapshot := make([]MemberInfo, 0, len(c.members))
	for _, m := range c.members {
		snapshot = append(snapshot, m.info)
	}
	_ = st.w.send(coordEvent{Op: "members", Members: snapshot})
	for inst, m := range c.members {
		if inst != p.Instance {
			info := st.info
			_ = m.w.send(coordEvent{Op: "member", Member: &info})
		}
	}
	c.mu.Unlock()
	c.b.logf("[coord] member joined pid=%d vip=%s members=%d", lease.PID, lease.VIP, c.memberCount())

	stop := make(chan struct{})
	defer close(stop)
	go func() {
		t := time.NewTicker(time.Second)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				if st.w.send(coordEvent{Op: "ping"}) != nil {
					_ = conn.Close()
					return
				}
			}
		}
	}()
	for {
		_ = conn.SetReadDeadline(time.Now().Add(c.b.opt.MemberTimeout))
		line, err := readLine(r, maxCoordLine)
		if err != nil {
			break
		}
		var ev coordEvent
		if strictUnmarshal(line, &ev) != nil {
			break
		}
		if ev.Op == "ping" {
			continue
		}
		if ev.Op == "fighter_reply" {
			if ev.Seq == 0 || len(ev.Body) > 3129 {
				break
			}
			c.mu.Lock()
			ch := st.fighterWaiters[ev.Seq]
			delete(st.fighterWaiters, ev.Seq)
			if ch != nil {
				ch <- ev
			}
			c.mu.Unlock()
			continue
		}
		if ev.Op != "nat" || ev.Port == 0 {
			break
		}
		if !c.recordNAT(st, ev.Port) {
			break
		}
		if st.w.send(coordEvent{Op: "nat_ok", Seq: ev.Seq}) != nil {
			break
		}
	}
	c.dropMember(st)
}

// leaseFor returns the requested lease when it is free (or already this
// instance's), otherwise a fresh one derived from the instance id. Callers hold mu.
func (c *Coordinator) leaseFor(p coordPreface) (Lease, bool) {
	free := func(pid uint32, vip netip.Addr) bool {
		owner, used := c.byPID[pid]
		vowner, vused := c.byVIP[vip]
		return (!used || owner == p.Instance) && (!vused || vowner == p.Instance)
	}
	if p.Lease != nil {
		vip, err := netip.ParseAddr(p.Lease.VIP)
		if err != nil || !validVIP(vip) || !validPID(p.Lease.PID) || !free(p.Lease.PID, vip) {
			return Lease{}, false
		}
		return Lease{PID: p.Lease.PID, VIP: vip.String()}, true
	}
	h := sha256.Sum256([]byte("eden-nsolan-auto-lease:" + p.Instance))
	pidSeed := binary.BigEndian.Uint32(h[0:4])
	vipSeed := uint32(binary.BigEndian.Uint16(h[4:6]))
	const pidLo, pidSpan = 0x00010000, 0x7FFFFFFF - 0x00010000 + 1
	const vipSpan = 256 * 254
	for i := uint32(0); i < 1<<16; i++ {
		pid := pidLo + (pidSeed+i)%pidSpan
		idx := (vipSeed + i) % vipSpan
		vip := netip.AddrFrom4([4]byte{10, 253, byte(idx / 254), byte(idx%254 + 1)})
		if free(pid, vip) {
			return Lease{PID: pid, VIP: vip.String()}, true
		}
	}
	return Lease{}, false
}

func validPID(pid uint32) bool { return pid != 0 && pid != 2 && pid <= 0x7FFFFFFF }

func validVIP(a netip.Addr) bool {
	if !a.Is4() || !instanceNet.Contains(a) {
		return false
	}
	b := a.As4()
	return b[3] != 0 && b[3] != 255
}

func (c *Coordinator) memberCount() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.members)
}

func (c *Coordinator) recordNAT(st *memberState, port uint16) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.members[st.info.Instance] != st {
		return false
	}
	st.obs = netip.AddrPortFrom(st.vip, port)
	return true
}

// EndpointByPID is the explicit station table: the NNCS-observed virtual UDP
// endpoint of the instance holding pid.
func (c *Coordinator) EndpointByPID(pid uint64) (netip.AddrPort, bool) {
	if pid > 0xFFFFFFFF {
		return netip.AddrPort{}, false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	st := c.members[c.byPID[uint32(pid)]]
	if st == nil || !st.obs.IsValid() {
		return netip.AddrPort{}, false
	}
	return st.obs, true
}

func (c *Coordinator) dropMember(st *memberState) {
	c.mu.Lock()
	if c.members[st.info.Instance] != st {
		c.mu.Unlock()
		return
	}
	delete(c.members, st.info.Instance)
	delete(c.byPID, st.info.PID)
	delete(c.byVIP, st.vip)
	streams := c.streams[st.info.Instance]
	delete(c.streams, st.info.Instance)
	for _, m := range c.members {
		_ = m.w.send(coordEvent{Op: "leave", Instance: st.info.Instance})
	}
	c.mu.Unlock()
	c.host.RemoveIdentity(st.info.PID)
	for vc := range streams {
		_ = vc.Close()
	}
	c.host.KickPID(st.info.PID)
	c.b.logf("[coord] member left pid=%d vip=%s", st.info.PID, st.info.VIP)
}

func (c *Coordinator) serveStream(conn net.Conn, r *bufio.Reader, p coordPreface) {
	var target *chanListener
	switch p.Service {
	case "auth":
		target = c.authL
	case "secure":
		target = c.secureL
	default:
		writeReply(conn, coordReply{Error: "bad_service"})
		_ = conn.Close()
		return
	}
	c.mu.Lock()
	st := c.members[p.Instance]
	if st == nil || subtle.ConstantTimeCompare([]byte(st.token), []byte(p.Token)) != 1 {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "bad_lease"})
		_ = conn.Close()
		return
	}
	port, ok := c.allocStreamPort(st.vip)
	if !ok {
		c.mu.Unlock()
		writeReply(conn, coordReply{Error: "busy"})
		_ = conn.Close()
		return
	}
	vc := &vconn{Conn: conn, r: r, remote: net.TCPAddrFromAddrPort(netip.AddrPortFrom(st.vip, port))}
	inst, vip := p.Instance, st.vip
	vc.onClose = func() {
		c.mu.Lock()
		delete(c.usedPorts[vip], port)
		if set := c.streams[inst]; set != nil {
			delete(set, vc)
		}
		c.mu.Unlock()
	}
	if c.streams[inst] == nil {
		c.streams[inst] = map[*vconn]struct{}{}
	}
	c.streams[inst][vc] = struct{}{}
	c.mu.Unlock()
	writeReply(conn, coordReply{OK: true})
	_ = conn.SetDeadline(time.Time{})
	if !target.deliver(vc, 5*time.Second) {
		_ = vc.Close()
	}
}

// allocStreamPort picks a per-VIP source port unique among live streams, so
// every NEX connection keeps a distinct RemoteAddr. Callers hold mu.
func (c *Coordinator) allocStreamPort(vip netip.Addr) (uint16, bool) {
	used := c.usedPorts[vip]
	if used == nil {
		used = map[uint16]bool{}
		c.usedPorts[vip] = used
	}
	next := c.nextPort[vip]
	for i := 0; i < 64512; i++ {
		if next < 1024 {
			next = 1024
		}
		port := next
		next++
		if !used[port] {
			used[port] = true
			c.nextPort[vip] = next
			return port, true
		}
	}
	return 0, false
}

// Close stops leading: listeners, member links, streams and the NEX host.
func (c *Coordinator) Close() {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return
	}
	c.closed = true
	members := c.members
	c.members = map[string]*memberState{}
	var streams []*vconn
	for _, set := range c.streams {
		for vc := range set {
			streams = append(streams, vc)
		}
	}
	c.mu.Unlock()
	c.cancel()
	if c.unixLn != nil {
		_ = c.unixLn.Close()
	}
	if c.tcpLn != nil {
		_ = c.tcpLn.Close()
	}
	if c.authL != nil {
		_ = c.authL.Close()
		_ = c.secureL.Close()
	}
	for _, m := range members {
		_ = m.conn.Close()
	}
	for _, vc := range streams {
		_ = vc.Close()
	}
	c.wg.Wait()
	if !c.hostStarted {
		close(c.done) // startCoordinator failed before the host goroutine existed
	}
}

// chanListener hands coordinator streams to an http.Server.
type chanListener struct {
	ch   chan net.Conn
	stop chan struct{}
	once sync.Once
	addr net.Addr
}

func newChanListener(addr net.Addr) *chanListener {
	return &chanListener{ch: make(chan net.Conn), stop: make(chan struct{}), addr: addr}
}

func (l *chanListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.ch:
		return c, nil
	case <-l.stop:
		return nil, net.ErrClosed
	}
}

func (l *chanListener) Close() error {
	l.once.Do(func() { close(l.stop) })
	return nil
}

func (l *chanListener) Addr() net.Addr { return l.addr }

func (l *chanListener) deliver(c net.Conn, wait time.Duration) bool {
	t := time.NewTimer(wait)
	defer t.Stop()
	select {
	case l.ch <- c:
		return true
	case <-l.stop:
		return false
	case <-t.C:
		return false
	}
}

// vconn presents a coordinator stream with its leased virtual RemoteAddr.
type vconn struct {
	net.Conn
	r       io.Reader
	remote  net.Addr
	once    sync.Once
	onClose func()
}

func (v *vconn) Read(p []byte) (int, error) { return v.r.Read(p) }
func (v *vconn) RemoteAddr() net.Addr       { return v.remote }
func (v *vconn) Close() error {
	err := v.Conn.Close()
	v.once.Do(func() {
		if v.onClose != nil {
			v.onClose()
		}
	})
	return err
}

func removeStaleSocket(path string) {
	if fi, err := os.Lstat(path); err == nil && fi.Mode()&os.ModeSocket != 0 {
		_ = os.Remove(path)
	}
}
