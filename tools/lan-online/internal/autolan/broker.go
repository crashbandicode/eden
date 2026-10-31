package autolan

import (
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"edennsolan/backend/internal/initshape"
	"edennsolan/backend/internal/natcheck"
)

// Broker is one Eden instance's runtime: guest control and datagram sockets,
// peer routing, local service listeners and the coordinator link.
type Broker struct {
	opt           Options
	logf          func(string, ...any)
	title         string
	titleInfo     titleInfo
	instance      string
	profile       *initshape.Responder
	profileLock   *os.File
	profileMu     sync.Mutex
	profileClosed bool

	runtimeDir  string
	runtimeReal string
	sharedDir   string
	localKey    string
	lanIP       netip.Addr
	ifi         *net.Interface

	ctlPath  string
	dataPath string
	ctlLn    *net.UnixListener
	dataT    dataTransport
	parent   *parentMonitor
	udp      *net.UDPConn
	udpRaw   syscall.RawConn
	udpPort  uint16

	authLn, secureLn net.Listener
	znc              *zncServer
	authPort         uint16
	securePort       uint16
	zncPort          uint16

	local   Announcer
	network Announcer

	mu         sync.RWMutex
	pid        uint32
	vip        netip.Addr
	bindings   map[string]uint16
	byPort     map[uint16]string
	peers      map[string]*peer
	peerByVIP  map[netip.Addr]*peer
	peerByPath map[string]*peer
	peerByUDP  map[netip.AddrPort]*peer

	linkMu      sync.Mutex
	link        *memberLink
	linkChanged chan struct{}
	session     atomic.Uint64

	coord *Coordinator // owned by the run goroutine

	unreachMu   sync.Mutex
	unreachable map[string]*leaderHealth
	dgramQlen   int

	startedAt time.Time // run start: the readiness budget is measured from here

	tokenMu          sync.Mutex
	memberToken      string
	memberTokenEpoch uint64
	coordPub         atomic.Pointer[Coordinator]
	leading          atomic.Bool

	readyWritten atomic.Bool
	natSem       chan struct{}

	connMu sync.Mutex
	conns  map[net.Conn]struct{}

	ctx    context.Context
	cancel context.CancelCauseFunc
	wg     sync.WaitGroup

	stats brokerStats
}

type brokerStats struct {
	guestFrames, forged, unknownSender, malformed atomic.Uint64
	toLocalPeer, toRemotePeer, delivered          atomic.Uint64
	noRoute, noBinding, sendDrops                 atomic.Uint64
	udpIn, udpRejected, natAnswered, natDropped   atomic.Uint64
}

type peer struct {
	info       MemberInfo
	vip        netip.Addr
	local      bool
	dataSocket string
	udp        netip.AddrPort
	session    uint64
}

var errParentGone = errors.New("parent process exited")

// instanceID32Zero has the length of an instance id (for socket path limits).
const instanceID32Zero = "00000000000000000000000000000000"

// dataTransport carries ELN1 frames between this broker, its guest sockets and same-host peer
// brokers. Linux uses AF_UNIX datagrams addressed by socket path (transport_linux.go); Windows
// uses AF_UNIX streams with the frame header as the record boundary (transport_windows.go).
// Both identify a sender by the path string of the socket node it owns.
type dataTransport interface {
	listen(b *Broker) error
	serve(b *Broker)
	// hasClient reports whether a guest socket may bind (Windows: its stream said hello).
	hasClient(path string) bool
	sendClient(path string, pkt []byte) error
	sendPeer(b *Broker, path string, pkt []byte) error
	close()
}

// stopRequested reports the parent's stop marker (platforms without a usable signal).
func stopRequested(runtimeDir string) bool {
	_, err := os.Lstat(filepath.Join(runtimeDir, stopFileName))
	return err == nil
}

// Run validates the options, serves until ctx ends or the parent exits, then
// removes the broker's own files. Fatal conditions are written to error.json.
func Run(ctx context.Context, opt Options) error {
	opt.withDefaults()
	b, err := newBroker(opt)
	if err != nil {
		var se *StatusError
		if errors.As(err, &se) && se.Code != "runtime_dir_invalid" && checkPrivateDir(opt.RuntimeDir) == nil {
			_ = writeErrorStatus(opt.RuntimeDir, se)
		}
		return err
	}
	err = b.run(ctx)
	b.shutdown()
	var se *StatusError
	if errors.As(err, &se) {
		_ = writeErrorStatus(b.runtimeDir, se)
	}
	return err
}

func newBroker(opt Options) (*Broker, error) {
	if bad := forbiddenEnvironment(); len(bad) > 0 {
		return nil, statusErr("bad_environment", "core persistence variables are set: %v", bad)
	}
	if err := checkPrivateDir(opt.RuntimeDir); err != nil {
		return nil, statusErr("runtime_dir_invalid", "%v", err)
	}
	title, info, err := normalizeTitle(opt.TitleID)
	if err != nil {
		return nil, err
	}
	if err := validateAssets(opt.AssetsDir); err != nil {
		return nil, err
	}
	if os.Getppid() != opt.ParentPID || !processAlive(opt.ParentPID) {
		return nil, statusErr("parent_gone", "parent %d is not this process's parent", opt.ParentPID)
	}
	parent, err := newParentMonitor(opt.ParentPID)
	if err != nil {
		return nil, statusErr("parent_gone", "parent %d: %v", opt.ParentPID, err)
	}
	b := &Broker{
		parent: parent,
		opt:    opt, logf: opt.Logf, title: title, titleInfo: info,
		runtimeDir: opt.RuntimeDir,
		ctlPath:    filepath.Join(opt.RuntimeDir, controlSocketName),
		dataPath:   filepath.Join(opt.RuntimeDir, dataSocketName),
		bindings:   map[string]uint16{}, byPort: map[uint16]string{},
		peers: map[string]*peer{}, peerByVIP: map[netip.Addr]*peer{},
		peerByPath: map[string]*peer{}, peerByUDP: map[netip.AddrPort]*peer{},
		linkChanged: make(chan struct{}),
		natSem:      make(chan struct{}, 16),
		conns:       map[net.Conn]struct{}{},
	}
	if b.runtimeReal, err = filepath.EvalSymlinks(opt.RuntimeDir); err != nil {
		return nil, statusErr("runtime_dir_invalid", "%v", err)
	}
	if len(b.ctlPath) > maxSunPath || len(b.dataPath) > maxSunPath {
		return nil, statusErr("runtime_dir_invalid", "runtime directory path is too long for AF_UNIX sockets")
	}
	if b.sharedDir, err = sharedDirectory(opt.SharedDir); err != nil {
		return nil, statusErr("runtime_dir_invalid", "%v", err)
	}
	// Fail before electing if this broker could never bind a coordinator socket.
	if len(filepath.Join(b.sharedDir, "coord-"+instanceID32Zero+".sock")) > maxSunPath {
		return nil, statusErr("listen_failed", "shared directory path is too long for AF_UNIX sockets")
	}
	b.localKey = opt.LocalKey
	if b.localKey == "" {
		b.localKey = sameHostKey(b.sharedDir)
	}
	b.instance = opt.Instance
	if b.instance == "" {
		if b.instance, err = randomHex(16); err != nil {
			return nil, statusErr("internal", "%v", err)
		}
	} else if !isInstanceID(b.instance) {
		return nil, statusErr("bad_arguments", "invalid instance id")
	}
	switch {
	case opt.LocalOnly:
	case opt.LANIP.IsValid():
		if !b.lanAddressAllowed(opt.LANIP) {
			return nil, statusErr("bad_arguments", "the LAN address must be a private IPv4 address")
		}
		b.lanIP = opt.LANIP
	default:
		if b.lanIP, b.ifi, err = lanEndpoint(opt.Interface); err != nil {
			return nil, err
		}
	}
	b.unreachable = map[string]*leaderHealth{}
	if b.dgramQlen = unixMaxDgramQlen(); b.dgramQlen > 0 && b.dgramQlen < minUnixDgramQlen {
		b.logf("[auto] warning: net.unix.max_dgram_qlen=%d (< %d); same-host datagrams may drop under load (sysctl left unchanged)", b.dgramQlen, minUnixDgramQlen)
	}
	if err := b.listen(); err != nil {
		b.closeSockets()
		return nil, err
	}
	if err := b.openProfile(); err != nil {
		b.closeSockets()
		parent.close()
		return nil, err
	}
	b.local = newLocalAnnouncer(b.sharedDir, b.instance)
	b.network = opt.Network
	if b.network == nil && b.ifi != nil {
		if m, err := newMulticastAnnouncer(b.ifi, b.lanIP, b.instance, opt.allowLoopback); err == nil {
			b.network = m
		} else {
			b.logf("[auto] LAN discovery unavailable on %s: %v (same-host play only)", b.ifi.Name, err)
		}
	}
	return b, nil
}

func (b *Broker) listen() error {
	removeStaleSocket(b.ctlPath)
	removeStaleSocket(b.dataPath)
	var err error
	if b.ctlLn, err = net.ListenUnix("unix", &net.UnixAddr{Name: b.ctlPath, Net: "unix"}); err != nil {
		return statusErr("listen_failed", "control socket: %v", err)
	}
	b.ctlLn.SetUnlinkOnClose(true)
	_ = os.Chmod(b.ctlPath, 0o600)
	b.dataT = newDataTransport()
	if err = b.dataT.listen(b); err != nil {
		return err
	}
	if b.lanIP.IsValid() {
		if b.udp, err = net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(b.lanIP, 0))); err != nil {
			return statusErr("listen_failed", "LAN datagram socket: %v", err)
		}
		_ = b.udp.SetReadBuffer(4 << 20)
		b.udpPort = b.udp.LocalAddr().(*net.UDPAddr).AddrPort().Port()
		if b.udpRaw, err = b.udp.SyscallConn(); err != nil {
			return statusErr("listen_failed", "LAN datagram socket: %v", err)
		}
	}
	loopback := func() (net.Listener, uint16, error) {
		ln, err := net.Listen("tcp4", "127.0.0.1:0")
		if err != nil {
			return nil, 0, statusErr("listen_failed", "local listener: %v", err)
		}
		return ln, ln.Addr().(*net.TCPAddr).AddrPort().Port(), nil
	}
	if b.authLn, b.authPort, err = loopback(); err != nil {
		return err
	}
	if b.secureLn, b.securePort, err = loopback(); err != nil {
		return err
	}
	zncLn, port, err := loopback()
	if err != nil {
		return err
	}
	b.zncPort = port
	zncTLS, err := selfSignedTLS(zncHost)
	if err != nil {
		_ = zncLn.Close()
		return statusErr("internal", "%v", err)
	}
	b.znc = &zncServer{ln: zncLn, tls: zncTLS, logf: b.logf}
	return nil
}

func (b *Broker) closeSockets() {
	if b.ctlLn != nil {
		_ = b.ctlLn.Close()
	}
	if b.dataT != nil {
		b.dataT.close()
	}
	if b.udp != nil {
		_ = b.udp.Close()
	}
	if b.authLn != nil {
		_ = b.authLn.Close()
	}
	if b.secureLn != nil {
		_ = b.secureLn.Close()
	}
	if b.znc != nil {
		_ = b.znc.ln.Close()
	}
}

func (b *Broker) run(parent context.Context) error {
	b.ctx, b.cancel = context.WithCancelCause(parent)
	b.startedAt = time.Now()
	ctx := b.ctx
	b.goRun(b.watchParent)
	b.goRun(b.serveControl)
	b.goRun(func() { b.dataT.serve(b) })
	if b.udp != nil {
		b.goRun(b.udpLoop)
	}
	b.goRun(func() { b.serveProxy(b.authLn, "auth") })
	b.goRun(func() { b.serveProxy(b.secureLn, "secure") })
	b.goRun(func() { b.znc.serve(ctx) })
	b.logf("[auto] broker started title=%s lan=%v local_only=%v", b.title, b.lanIP, !b.lanIP.IsValid())

	for ctx.Err() == nil {
		var leader Beacon
		if b.coord != nil {
			select {
			case <-b.coord.Done():
				b.stopLeading("NEX host stopped")
				continue
			default:
			}
			leader = b.selfLeaderBeacon()
		} else {
			l, self, err := b.elect(ctx)
			if err != nil {
				break
			}
			if self {
				coord, err := startCoordinator(b)
				if err != nil {
					b.fail(statusErr("listen_failed", "coordinator: %v", err))
					break
				}
				b.coord = coord
				b.coordPub.Store(coord)
				b.leading.Store(true)
				b.announce(roleLeader)
				leader = b.selfLeaderBeacon()
			} else {
				b.announce(roleMember)
				leader = l
			}
		}
		link, err := b.join(leader)
		if err != nil {
			var se *StatusError
			if errors.As(err, &se) {
				b.fail(se)
				break
			}
			b.logf("[auto] join failed: %v", err)
			if b.coord == nil {
				// A leader we hear but cannot join (for example behind a firewall)
				// is excluded for a cooldown after repeated failures, so the
				// election falls back to reachable candidates instead of looping.
				b.noteJoinFailure(leaderKey(leader))
				sleepCtx(ctx, 200*time.Millisecond)
			} else {
				b.stopLeading("own coordinator unreachable")
			}
			continue
		}
		if b.coord == nil {
			b.noteJoinSuccess(leaderKey(leader))
		}
		b.announce(b.currentRole()) // republish the local record with the leased VIP
		b.setLink(link)
		if b.readyWritten.CompareAndSwap(false, true) {
			if err := b.writeReady(); err != nil {
				b.fail(statusErr("internal", "ready file: %v", err))
				break
			}
			b.logf("[auto] ready pid=%d vip=%s", b.pid, b.vip)
		}
		b.waitLink(ctx, link)
		b.setLink(nil)
		link.close()
	}
	cause := context.Cause(ctx)
	if errors.Is(cause, errParentGone) || errors.Is(cause, context.Canceled) {
		return nil
	}
	return cause
}

func (b *Broker) goRun(f func()) {
	b.wg.Add(1)
	go func() {
		defer b.wg.Done()
		f()
	}()
}

func (b *Broker) fail(se *StatusError) {
	b.logf("[auto] fatal %s: %s", se.Code, se.Message)
	b.cancel(se)
}

func (b *Broker) watchParent() {
	t := time.NewTicker(250 * time.Millisecond)
	defer t.Stop()
	for {
		select {
		case <-b.ctx.Done():
			return
		case <-t.C:
			if !b.parent.alive() {
				b.logf("[auto] parent exited; shutting down")
				b.cancel(errParentGone)
				return
			}
			if useStopFile && stopRequested(b.runtimeDir) {
				b.logf("[auto] parent requested a stop; shutting down")
				b.cancel(errParentGone)
				return
			}
		}
	}
}

func sleepCtx(ctx context.Context, d time.Duration) {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
	case <-t.C:
	}
}

// --- election ---

func (b *Broker) selfLeaderBeacon() Beacon {
	l := Beacon{Proto: beaconProto, V: protocolVersion, Title: b.title, Instance: b.instance, Role: roleLeader,
		Unix: b.coord.unixPath, LocalKey: b.localKey, Local: true, Epoch: b.coord.epoch,
		TLS: b.opt.ControlTLS, TLSFP: b.coord.TLSFingerprint()}
	if tcp := b.coord.TCPAddr(); tcp.IsValid() {
		l.TCP = tcp.String()
	}
	return l
}

func (b *Broker) currentRole() string {
	if b.coord != nil {
		return roleLeader
	}
	return roleMember
}

// announce publishes this broker's record. The same-host record (private shared
// directory) always exists while the broker runs: it carries the data socket
// and, once leased, the VIP, which is how same-host peers recognise each other.
// The network beacon exists only for candidates and leaders; the network
// announcers strip every same-host field before it leaves the host.
func (b *Broker) announce(role string) {
	l := Beacon{Proto: beaconProto, V: protocolVersion, Title: b.title, Instance: b.instance, Role: role,
		LocalKey: b.localKey, DataSocket: b.dataPath, TLS: b.opt.ControlTLS}
	if lease := b.currentLease(); lease != nil {
		l.VIP = lease.VIP
	}
	if role == roleLeader && b.coord != nil {
		l.Unix, l.Epoch, l.TLSFP = b.coord.unixPath, b.coord.epoch, b.coord.TLSFingerprint()
		if tcp := b.coord.TCPAddr(); tcp.IsValid() {
			l.TCP = tcp.String()
		}
	}
	b.local.Publish(&l)
	if b.network != nil {
		if role == roleMember {
			b.network.Publish(nil)
		} else {
			b.network.Publish(&l)
		}
	}
}

func (b *Broker) heard() []Beacon {
	out := b.local.Heard()
	if b.network != nil {
		out = append(out, b.network.Heard()...)
	}
	return out
}

// findLeader returns the lowest-instance compatible leader that is not in a
// join-failure cooldown and that we could reach: through our own shared
// directory on this host, or over TCP to a permitted LAN address.
func (b *Broker) findLeader() (Beacon, bool) {
	var best Beacon
	found := false
	expired := b.startupExpired()
	for _, h := range b.heard() {
		if !b.compatible(h) || h.Role != roleLeader || h.Instance == b.instance || b.leaderExcluded(leaderKey(h)) {
			continue
		}
		local := h.Local && h.LocalKey == b.localKey && h.Unix != ""
		reachable := local || (!expired && !h.Local && h.TCP != "" && b.lanIP.IsValid() && b.tcpEndpointAllowed(h.TCP))
		if !reachable {
			continue
		}
		if !found || h.Instance < best.Instance || (h.Instance == best.Instance && h.Local) {
			best, found = h, true
		}
	}
	return best, found
}

// startupExpired reports that the readiness budget is spent without a ready
// file. From then on only same-host coordinators are considered and otherwise
// this broker leads itself, so no unreachable or pretending network peer can
// hold the frontend's readiness deadline. Afterwards the normal election rules
// (including stepping down for a reachable lower leader) apply again.
func (b *Broker) startupExpired() bool {
	return !b.readyWritten.Load() && !b.startedAt.IsZero() && time.Since(b.startedAt) > b.opt.startupBudget
}
func (b *Broker) tcpEndpointAllowed(s string) bool {
	ap, err := netip.ParseAddrPort(s)
	return err == nil && ap.Port() != 0 && b.lanAddressAllowed(ap.Addr())
}

// lanAddressAllowed accepts RFC 1918 LAN addresses; loopback only under the
// unexported test option.
func (b *Broker) lanAddressAllowed(a netip.Addr) bool {
	a = a.Unmap()
	return isLANAddress(a) || (b.opt.allowLoopback && a.Is4() && a.IsLoopback())
}

// elect waits up to DiscoveryWindow for an existing leader. Without one, the
// lowest instance among the heard candidates and ourselves leads; the others
// wait LeaderWait for it and otherwise start a new round. A broker that finds a
// leader never competes with it, so late joiners do not steal leadership.
func (b *Broker) elect(ctx context.Context) (Beacon, bool, error) {
	b.announce(roleCandidate)
	for {
		deadline := time.Now().Add(b.opt.DiscoveryWindow)
		for {
			if l, ok := b.findLeader(); ok {
				return l, false, nil
			}
			if ctx.Err() != nil {
				return Beacon{}, false, ctx.Err()
			}
			if time.Now().After(deadline) || b.startupExpired() {
				break
			}
			sleepCtx(ctx, 50*time.Millisecond)
		}
		winner := b.instance
		for _, h := range b.heard() {
			if !b.startupExpired() && b.compatible(h) && h.Role == roleCandidate && h.Instance < winner {
				winner = h.Instance
			}
		}
		if winner == b.instance {
			return Beacon{}, true, nil
		}
		wait := time.Now().Add(b.opt.LeaderWait)
		for time.Now().Before(wait) && !b.startupExpired() {
			if l, ok := b.findLeader(); ok {
				return l, false, nil
			}
			if ctx.Err() != nil {
				return Beacon{}, false, ctx.Err()
			}
			sleepCtx(ctx, 50*time.Millisecond)
		}
		b.announce(roleCandidate)
	}
}

// waitLink returns when the link ends, ctx ends, or while leading: our NEX host
// stops (we stop leading and re-elect), or a lower-instance leader appears that
// we can actually reach (we step down so its members are not split). A lower
// leader that fails the reachability probe is counted like a failed join and
// ignored during its cooldown, so an unreachable host cannot capture the LAN.
func (b *Broker) waitLink(ctx context.Context, link *memberLink) {
	t := time.NewTicker(250 * time.Millisecond)
	defer t.Stop()
	var hostDone <-chan struct{} // nil (never ready) unless leading
	if b.coord != nil {
		hostDone = b.coord.Done()
	}
	for {
		select {
		case <-ctx.Done():
			return
		case <-link.done:
			return
		case <-hostDone:
			b.stopLeading("NEX host stopped")
			return
		case <-t.C:
			if b.coord != nil {
				if l, ok := b.findLeader(); ok && l.Instance < b.instance {
					if b.probeLeader(l) {
						b.stopLeading("a reachable lower-instance coordinator exists")
						return
					}
					b.noteJoinFailure(leaderKey(l))
				}
			}
		}
	}
}

func (b *Broker) stopLeading(reason string) {
	if b.coord == nil {
		return
	}
	b.logf("[auto] stepping down: %s", reason)
	b.coord.Close()
	b.coord = nil
	b.coordPub.Store(nil)
	b.leading.Store(false)
	b.announce(roleMember)
}

// --- leader reachability ---

// leaderHealth tracks consecutive failed joins (or probes) of one leader.
type leaderHealth struct {
	failures int
	until    time.Time // excluded from election until then
}

const joinFailuresBeforeCooldown = 2

// compatible: same title and same control-TLS mode. Brokers of different modes
// never elect or join each other, so a mixed LAN never falls back to plaintext.
func (b *Broker) compatible(h Beacon) bool {
	return h.Title == b.title && h.TLS == b.opt.ControlTLS
}

// leaderKey identifies one coordinator lifetime (instance, epoch, certificate),
// so a restarted coordinator is not penalised for its predecessor's failures.
func leaderKey(h Beacon) string {
	return h.Instance + "/" + strconv.FormatUint(h.Epoch, 10) + "/" + h.TLSFP
}

func (b *Broker) noteJoinFailure(key string) {
	b.unreachMu.Lock()
	defer b.unreachMu.Unlock()
	h := b.unreachable[key]
	if h == nil {
		h = &leaderHealth{}
		b.unreachable[key] = h
	}
	h.failures++
	threshold := joinFailuresBeforeCooldown
	if !b.readyWritten.Load() {
		threshold = 1 // still starting: a failed join already costs readiness budget
	}
	if h.failures >= threshold {
		h.failures = 0
		h.until = time.Now().Add(b.opt.LeaderCooldown)
		b.logf("[auto] coordinator unreachable; excluded for %s", b.opt.LeaderCooldown)
	}
}

func (b *Broker) noteJoinSuccess(key string) {
	b.unreachMu.Lock()
	delete(b.unreachable, key)
	b.unreachMu.Unlock()
}

func (b *Broker) leaderExcluded(key string) bool {
	b.unreachMu.Lock()
	defer b.unreachMu.Unlock()
	h := b.unreachable[key]
	return h != nil && time.Now().Before(h.until)
}

// coordinatorUnreachable reports whether any heard leader is currently excluded.
func (b *Broker) coordinatorUnreachable() bool {
	b.unreachMu.Lock()
	defer b.unreachMu.Unlock()
	now := time.Now()
	for _, h := range b.unreachable {
		if now.Before(h.until) {
			return true
		}
	}
	return false
}

// --- coordinator link state ---

func (b *Broker) setLink(l *memberLink) {
	b.linkMu.Lock()
	b.link = l
	close(b.linkChanged)
	b.linkChanged = make(chan struct{})
	b.linkMu.Unlock()
	if l != nil {
		session := l.session
		time.AfterFunc(b.opt.PruneAfter, func() { b.prunePeers(session) })
	}
}

func (b *Broker) currentLink() (*memberLink, <-chan struct{}) {
	b.linkMu.Lock()
	defer b.linkMu.Unlock()
	return b.link, b.linkChanged
}

func (b *Broker) waitForLink(d time.Duration) *memberLink {
	deadline := time.NewTimer(d)
	defer deadline.Stop()
	for {
		l, changed := b.currentLink()
		if l != nil {
			return l
		}
		select {
		case <-changed:
		case <-deadline.C:
			return nil
		case <-b.ctx.Done():
			return nil
		}
	}
}

// setLease records the first lease and rejects any different later one.
func (b *Broker) setLease(pid uint32, vip netip.Addr) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pid == 0 {
		b.pid, b.vip = pid, vip
		return nil
	}
	if b.pid != pid || b.vip != vip {
		return statusErr("lease_conflict", "coordinator assigned a different identity")
	}
	return nil
}

func (b *Broker) currentLease() *Lease {
	b.mu.RLock()
	defer b.mu.RUnlock()
	if b.pid == 0 {
		return nil
	}
	return &Lease{PID: b.pid, VIP: b.vip.String()}
}

// --- peer table ---

func (b *Broker) upsertPeer(m MemberInfo, session uint64) {
	if m.Instance == b.instance || !isInstanceID(m.Instance) {
		return
	}
	vip, err := netip.ParseAddr(m.VIP)
	if err != nil || !validVIP(vip) {
		return
	}
	p := &peer{info: m, vip: vip, session: session}
	// Locality is never taken from the coordinator: a peer is on this host only
	// if our own private shared directory holds its live record, and the data
	// socket that record names is a socket owned by us.
	if path, ok := b.localPeerSocket(m.Instance, vip); ok {
		p.local, p.dataSocket = true, path
	} else {
		ip, err := netip.ParseAddr(m.LANIP)
		if err != nil || m.UDPPort == 0 || !b.lanAddressAllowed(ip) {
			return
		}
		p.udp = netip.AddrPortFrom(ip.Unmap(), m.UDPPort)
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if old := b.peers[m.Instance]; old != nil {
		b.unindexPeer(old)
	}
	if other := b.peerByVIP[vip]; other != nil {
		b.unindexPeer(other)
		delete(b.peers, other.info.Instance)
	}
	b.peers[m.Instance] = p
	b.peerByVIP[vip] = p
	if p.local {
		b.peerByPath[p.dataSocket] = p
	} else {
		b.peerByUDP[p.udp] = p
	}
}

// localPeerSocket returns the data socket of a same-host peer, derived only
// from its live record in our shared directory (same local key, live process,
// matching VIP once leased) and a socket inode owned by our uid.
func (b *Broker) localPeerSocket(instance string, vip netip.Addr) (string, bool) {
	rec, ok := b.localPeerRecord(instance)
	if !ok {
		return "", false
	}
	if rec.VIP != "" && rec.VIP != vip.String() {
		return "", false
	}
	if !ownedSocket(rec.DataSocket) {
		return "", false
	}
	return rec.DataSocket, true
}

// localPeerRecord is the live same-host record of instance in our private shared directory:
// same local key, compatible mode, naming a valid data socket path.
func (b *Broker) localPeerRecord(instance string) (Beacon, bool) {
	rec, ok := readLocalBeacon(filepath.Join(b.sharedDir, "brokers", instance+".json"), instance)
	if !ok || !b.compatible(rec) || rec.LocalKey != b.localKey || !validSocketPath(rec.DataSocket) {
		return Beacon{}, false
	}
	return rec, true
}

func (b *Broker) unindexPeer(p *peer) {
	if b.peerByVIP[p.vip] == p {
		delete(b.peerByVIP, p.vip)
	}
	if p.local && b.peerByPath[p.dataSocket] == p {
		delete(b.peerByPath, p.dataSocket)
	}
	if !p.local && b.peerByUDP[p.udp] == p {
		delete(b.peerByUDP, p.udp)
	}
}

func (b *Broker) removePeer(instance string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if p := b.peers[instance]; p != nil {
		b.unindexPeer(p)
		delete(b.peers, instance)
	}
}

// prunePeers drops peers the coordinator of session never listed. Peers kept
// from an earlier coordinator keep routing until then (no leader dependency).
func (b *Broker) prunePeers(session uint64) {
	l, _ := b.currentLink()
	if l == nil || l.session != session {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	for inst, p := range b.peers {
		if p.session != session {
			b.unindexPeer(p)
			delete(b.peers, inst)
		}
	}
}

// --- datagram routing ---

// handleUnix routes one frame received from the same-host transport; sender is the path of the
// socket node the transport attributed it to (a bound guest socket or a peer broker's data socket).
func (b *Broker) handleUnix(sender string, pkt []byte) {
	f, err := DecodeFrame(pkt)
	if err != nil {
		b.stats.malformed.Add(1)
		return
	}
	b.mu.RLock()
	port, isClient := b.bindings[sender]
	p := b.peerByPath[sender]
	vip := b.vip
	b.mu.RUnlock()
	switch {
	case isClient:
		b.stats.guestFrames.Add(1)
		if f.Src.Addr() != vip || f.Src.Port() != port {
			b.stats.forged.Add(1)
			return
		}
		b.route(f, pkt)
	case p != nil:
		if f.Src.Addr() != p.vip {
			b.stats.forged.Add(1)
			return
		}
		b.deliverLocal(f, pkt)
	default:
		b.stats.unknownSender.Add(1)
	}
}

func (b *Broker) route(f Frame, pkt []byte) {
	dst := f.Dst.Addr()
	b.mu.RLock()
	vip := b.vip
	p := b.peerByVIP[dst]
	b.mu.RUnlock()
	switch {
	case dst == vip:
		b.deliverLocal(f, pkt)
	case (dst == serviceIP1 || dst == serviceIP2) && (f.Dst.Port() == natPort1 || f.Dst.Port() == natPort2):
		b.handleNAT(f)
	case p != nil && p.local:
		if b.sendToPeer(p.dataSocket, pkt) == nil {
			b.stats.toLocalPeer.Add(1)
		}
	case p != nil && b.udpRaw != nil:
		if b.sendUDP(p.udp, pkt) == nil {
			b.stats.toRemotePeer.Add(1)
		}
	default:
		b.stats.noRoute.Add(1)
	}
}

// deliverLocal hands a frame addressed to this instance to the bound client.
func (b *Broker) deliverLocal(f Frame, pkt []byte) {
	b.mu.RLock()
	vip := b.vip
	path, ok := b.byPort[f.Dst.Port()]
	b.mu.RUnlock()
	if f.Dst.Addr() != vip || !ok {
		b.stats.noBinding.Add(1)
		return
	}
	err := b.sendToClient(path, pkt)
	if err == nil {
		b.stats.delivered.Add(1)
		return
	}
	if clientGone(err) {
		// The client socket is gone: release its binding like a closed UDP socket.
		b.releaseBinding(path, f.Dst.Port())
	}
}

func (b *Broker) handleNAT(f Frame) {
	if len(f.Payload) < natcheck.ProbeLen {
		b.stats.natDropped.Add(1)
		return
	}
	select {
	case b.natSem <- struct{}{}:
	default:
		b.stats.natDropped.Add(1)
		return
	}
	testID := binary.BigEndian.Uint32(f.Payload[0:4])
	src, server := f.Src, f.Dst
	go func() {
		defer func() { <-b.natSem }()
		link, _ := b.currentLink()
		if link == nil || link.recordNAT(src.Port(), time.Second) != nil {
			b.stats.natDropped.Add(1)
			return
		}
		reply, err := AppendFrame(nil, server, src, natcheck.Reply(testID, src, server.Addr()))
		if err != nil {
			return
		}
		rf, _ := DecodeFrame(reply)
		b.stats.natAnswered.Add(1)
		b.deliverLocal(rf, reply)
	}()
}

func (b *Broker) udpLoop() {
	buf := make([]byte, MaxFrame+1)
	for {
		n, from, err := b.udp.ReadFromUDPAddrPort(buf)
		if err != nil {
			if b.ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		b.stats.udpIn.Add(1)
		f, err := DecodeFrame(buf[:n])
		if err != nil {
			b.stats.udpRejected.Add(1)
			continue
		}
		from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
		b.mu.RLock()
		p := b.peerByUDP[from]
		b.mu.RUnlock()
		if p == nil || f.Src.Addr() != p.vip {
			b.stats.udpRejected.Add(1)
			continue
		}
		b.deliverLocal(f, buf[:n])
	}
}

// sendToClient hands a frame to a registered guest socket; a full or missing receiver drops it.
func (b *Broker) sendToClient(path string, pkt []byte) error {
	err := b.dataT.sendClient(path, pkt)
	if err != nil {
		b.stats.sendDrops.Add(1)
	}
	return err
}

// sendToPeer hands a frame to a same-host peer broker's data endpoint.
func (b *Broker) sendToPeer(path string, pkt []byte) error {
	err := b.dataT.sendPeer(b, path, pkt)
	if err != nil {
		b.stats.sendDrops.Add(1)
	}
	return err
}

func (b *Broker) sendUDP(to netip.AddrPort, pkt []byte) error {
	err := udpSend(b.udp, b.udpRaw, &syscall.SockaddrInet4{Port: int(to.Port()), Addr: to.Addr().As4()}, pkt)
	if err != nil {
		b.stats.sendDrops.Add(1)
	}
	return err
}

// --- readiness and shutdown ---

type readyRedirect struct {
	Address       string `json:"address"`
	From          uint16 `json:"from"`
	To            uint16 `json:"to"`
	TargetAddress string `json:"target_address"`
}

type readyTransport struct {
	ControlSocket  string `json:"control_socket"`
	DatagramSocket string `json:"datagram_socket"`
	InstanceIP     string `json:"instance_ip"`
}

type readyFile struct {
	AccountID     uint32            `json:"account_id"`
	DNS           map[string]string `json:"dns"`
	AllowedIPs    []string          `json:"allowed_ips"`
	PortRedirects []readyRedirect   `json:"port_redirects"`
	AutoTransport readyTransport    `json:"auto_transport"`
}

func (b *Broker) readyContents() readyFile {
	s1, s2 := serviceIP1.String(), serviceIP2.String()
	b.mu.RLock()
	defer b.mu.RUnlock()
	return readyFile{
		AccountID: b.pid,
		DNS: map[string]string{
			b.titleInfo.GameServerHost: s1, nncs1Host: s1, nncs2Host: s2, zncHost: s2,
		},
		// Eden's parser requires the redirect target and this instance's own VIP to
		// be allowlisted. Peer VIPs are deliberately absent: they are reachable
		// only through the datagram transport, which drops unknown destinations.
		AllowedIPs: []string{s1, s2, "127.0.0.1", b.vip.String()},
		PortRedirects: []readyRedirect{
			{Address: s1, From: 443, To: b.authPort, TargetAddress: "127.0.0.1"},
			{Address: s1, From: 60005, To: b.securePort, TargetAddress: "127.0.0.1"},
			{Address: s2, From: 443, To: b.zncPort, TargetAddress: "127.0.0.1"},
		},
		AutoTransport: readyTransport{ControlSocket: b.ctlPath, DatagramSocket: b.dataPath, InstanceIP: b.vip.String()},
	}
}

func (b *Broker) writeReady() error {
	return writeJSONAtomic(filepath.Join(b.runtimeDir, readyFileName), b.readyContents())
}

func (b *Broker) shutdown() {
	b.cancel(context.Canceled)
	if b.network != nil {
		b.network.Publish(nil) // the local record is removed when its announcer closes
	}
	if b.coord != nil {
		b.coord.Close()
		b.coord = nil
		b.coordPub.Store(nil)
		b.leading.Store(false)
	}
	if l, _ := b.currentLink(); l != nil {
		l.close()
	}
	b.closeSockets()
	b.connMu.Lock()
	for c := range b.conns {
		_ = c.Close()
	}
	b.connMu.Unlock()
	done := make(chan struct{})
	go func() {
		b.wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		b.logf("[auto] shutdown timed out waiting for workers")
	}
	_ = b.local.Close()
	if b.network != nil {
		_ = b.network.Close()
	}
	removeStaleSocket(b.ctlPath)
	removeStaleSocket(b.dataPath)
	if b.readyWritten.Load() {
		_ = os.Remove(filepath.Join(b.runtimeDir, readyFileName))
	}
	if b.parent != nil {
		b.parent.close()
	}
	b.profileMu.Lock()
	b.profileClosed = true
	if b.profileLock != nil {
		_ = b.profileLock.Close()
	}
	b.profileMu.Unlock()
	b.logf("[auto] broker stopped")
}

// --- small validators ---

func isHex32(s string) bool {
	if len(s) != 32 {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}

func validSocketPath(p string) bool {
	return filepath.IsAbs(p) && filepath.Clean(p) == p && len(p) <= maxSunPath
}
