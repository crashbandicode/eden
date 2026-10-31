package autolan

import (
	"bufio"
	"errors"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"
)

// memberLink is this broker's registration with the current coordinator.
type memberLink struct {
	b       *Broker
	leader  Beacon
	conn    net.Conn
	r       *bufio.Reader
	w       *lineWriter
	token   string
	epoch   uint64
	session uint64
	done    chan struct{}
	once    sync.Once
	seq     atomic.Uint64

	mu      sync.Mutex
	waiters map[uint64]chan struct{}
}

var errLinkClosed = errors.New("coordinator link closed")

// dialLeader connects to a coordinator. The whole exchange the caller performs
// on the connection shares the absolute deadline: LAN round trips take
// milliseconds, and a peer that only pretends to be online must not hold a
// starting broker past its readiness budget.
func (b *Broker) dialLeader(l Beacon, deadline time.Time) (net.Conn, error) {
	var conn net.Conn
	var err error
	if l.Local && l.LocalKey == b.localKey && l.Unix != "" {
		conn, err = dialCoordinator("unix", l.Unix, b.sharedDir, netip.Addr{}, false, nil, deadline) // same host: Unix
	} else {
		if l.TCP == "" || !b.lanIP.IsValid() || l.TLS != b.opt.ControlTLS {
			return nil, errors.New("coordinator unreachable from this broker")
		}
		var tlsc *controlTLSClient
		if b.opt.ControlTLS {
			// Pinned to the certificate this leader advertised (the leader selected
			// for this link; a restarted coordinator advertises a new one).
			tlsc = &controlTLSClient{pin: l.TLSFP}
		}
		conn, err = dialCoordinator("tcp4", l.TCP, b.sharedDir, b.lanIP, b.opt.allowLoopback, tlsc, deadline)
	}
	if err != nil {
		return nil, err
	}
	_ = conn.SetDeadline(deadline)
	return conn, nil
}

// join registers with the leader, requesting the existing lease if we have one
// and presenting our current member token when reconnecting to the same
// coordinator lifetime (a live entry is only replaced by its token holder).
func (b *Broker) join(l Beacon) (*memberLink, error) {
	conn, err := b.dialLeader(l, time.Now().Add(b.opt.joinAttemptTimeout))
	if err != nil {
		return nil, err
	}
	p := coordPreface{V: protocolVersion, Kind: "member", Title: b.title, Instance: b.instance,
		Lease: b.currentLease(), TLS: b.opt.ControlTLS, Token: b.memberTokenFor(l.Epoch)}
	if b.lanIP.IsValid() {
		p.LANIP, p.UDPPort = b.lanIP.String(), b.udpPort
	}
	w := &lineWriter{w: conn, conn: conn}
	if err := w.send(p); err != nil {
		_ = conn.Close()
		return nil, err
	}
	r := bufio.NewReaderSize(conn, 64<<10)
	line, err := readLine(r, maxControlLine)
	if err != nil {
		_ = conn.Close()
		return nil, err
	}
	var rep coordReply
	if err := strictUnmarshal(line, &rep); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if !rep.OK {
		_ = conn.Close()
		if rep.Error == "lease_conflict" {
			return nil, statusErr("lease_conflict", "the coordinator refused this instance's identity")
		}
		return nil, errors.New("coordinator refused registration: " + rep.Error)
	}
	vip, err := netip.ParseAddr(rep.VIP)
	if err != nil || !validVIP(vip) || !validPID(rep.PID) || !isHex32(rep.Token) {
		_ = conn.Close()
		return nil, errors.New("coordinator sent an invalid lease")
	}
	if err := b.setLease(rep.PID, vip); err != nil {
		_ = conn.Close()
		return nil, err
	}
	b.setMemberToken(rep.Token, rep.Epoch)
	_ = conn.SetDeadline(time.Time{})
	m := &memberLink{b: b, leader: l, conn: conn, r: r, w: w, token: rep.Token, epoch: rep.Epoch,
		session: b.session.Add(1), done: make(chan struct{}), waiters: map[uint64]chan struct{}{}}
	go m.readLoop()
	go m.pingLoop()
	return m, nil
}

func (m *memberLink) close() {
	m.once.Do(func() {
		close(m.done)
		_ = m.conn.Close()
	})
}

func (m *memberLink) readLoop() {
	defer m.close()
	for {
		_ = m.conn.SetReadDeadline(time.Now().Add(m.b.opt.LeaderTimeout))
		line, err := readLine(m.r, maxCoordLine)
		if err != nil {
			return
		}
		var ev coordEvent
		if strictUnmarshal(line, &ev) != nil {
			return
		}
		switch ev.Op {
		case "fighter_request":
			if ev.Seq == 0 || len(ev.Body) > 30 {
				return
			}
			if m.session != m.b.session.Load() {
				return
			}
			body, code := m.b.exchangeProfile(ev.Method, ev.Body)
			if m.w.send(coordEvent{Op: "fighter_reply", Seq: ev.Seq, Body: body, Result: code}) != nil {
				return
			}
		case "ping":
		case "members":
			for _, mi := range ev.Members {
				m.b.upsertPeer(mi, m.session)
			}
		case "member":
			if ev.Member != nil {
				m.b.upsertPeer(*ev.Member, m.session)
			}
		case "leave":
			m.b.removePeer(ev.Instance)
		case "nat_ok":
			m.mu.Lock()
			if ch := m.waiters[ev.Seq]; ch != nil {
				close(ch)
				delete(m.waiters, ev.Seq)
			}
			m.mu.Unlock()
		default:
			return
		}
	}
}

func (m *memberLink) pingLoop() {
	t := time.NewTicker(time.Second)
	defer t.Stop()
	for {
		select {
		case <-m.done:
			return
		case <-t.C:
			if m.w.send(coordEvent{Op: "ping"}) != nil {
				m.close()
				return
			}
		}
	}
}

// recordNAT stores the guest's observed virtual endpoint at the coordinator
// and returns only after the coordinator acknowledged it.
func (m *memberLink) recordNAT(port uint16, timeout time.Duration) error {
	seq := m.seq.Add(1)
	ch := make(chan struct{})
	m.mu.Lock()
	m.waiters[seq] = ch
	m.mu.Unlock()
	defer func() {
		m.mu.Lock()
		delete(m.waiters, seq)
		m.mu.Unlock()
	}()
	if err := m.w.send(coordEvent{Op: "nat", Port: port, Seq: seq}); err != nil {
		return err
	}
	t := time.NewTimer(timeout)
	defer t.Stop()
	select {
	case <-ch:
		return nil
	case <-m.done:
		return errLinkClosed
	case <-t.C:
		return errors.New("coordinator did not acknowledge the NAT observation")
	}
}

// openStream opens one auth/secure stream to the coordinator. The reply is
// read byte by byte so the returned connection starts exactly at the TLS bytes.
func (m *memberLink) openStream(service string) (net.Conn, error) {
	conn, err := m.b.dialLeader(m.leader, time.Now().Add(m.b.opt.joinAttemptTimeout))
	if err != nil {
		return nil, err
	}
	p := coordPreface{V: protocolVersion, Kind: "stream", Title: m.b.title, Instance: m.b.instance, Token: m.token, Service: service, TLS: m.b.opt.ControlTLS}
	if err := (&lineWriter{w: conn}).send(p); err != nil {
		_ = conn.Close()
		return nil, err
	}
	line, err := readLineUnbuffered(conn, 256)
	if err != nil {
		_ = conn.Close()
		return nil, err
	}
	var rep coordReply
	if err := strictUnmarshal(line, &rep); err != nil || !rep.OK {
		_ = conn.Close()
		return nil, errors.New("coordinator refused the stream")
	}
	_ = conn.SetDeadline(time.Time{})
	return conn, nil
}

// probeLeader asks a coordinator for an explicit, protocol-compatible probe
// reply naming the epoch it advertised. Connecting (even completing pinned TLS)
// is not enough: an endpoint that rejects our protocol is not a leader to step
// down for. The whole exchange shares one bounded deadline.
func (b *Broker) probeLeader(l Beacon) bool {
	if l.Epoch == 0 {
		return false
	}
	conn, err := b.dialLeader(l, time.Now().Add(b.opt.joinAttemptTimeout))
	if err != nil {
		return false
	}
	defer conn.Close()
	p := coordPreface{V: protocolVersion, Kind: "probe", Title: b.title, Instance: b.instance, TLS: b.opt.ControlTLS}
	if (&lineWriter{w: conn}).send(p) != nil {
		return false
	}
	line, err := readLineUnbuffered(conn, 256)
	if err != nil {
		return false
	}
	var rep coordReply
	return strictUnmarshal(line, &rep) == nil && rep.OK && rep.Epoch == l.Epoch
}

// The member token proves ownership of a live registration. It is kept per
// coordinator lifetime (epoch) and presented only when reconnecting to that
// same coordinator; a new coordinator never sees an old token.
func (b *Broker) setMemberToken(token string, epoch uint64) {
	b.tokenMu.Lock()
	b.memberToken, b.memberTokenEpoch = token, epoch
	b.tokenMu.Unlock()
}

func (b *Broker) memberTokenFor(epoch uint64) string {
	b.tokenMu.Lock()
	defer b.tokenMu.Unlock()
	if epoch == 0 || epoch != b.memberTokenEpoch {
		return ""
	}
	return b.memberToken
}
