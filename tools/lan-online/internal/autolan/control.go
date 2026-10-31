package autolan

import (
	"bufio"
	"encoding/json"
	"errors"
	"io"
	"math/rand/v2"
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type controlRequest struct {
	ID     json.RawMessage `json:"id,omitempty"`
	Op     string          `json:"op"`
	Socket *string         `json:"socket,omitempty"`
	Port   *int            `json:"port,omitempty"`
}

type controlResponse struct {
	ID    json.RawMessage `json:"id,omitempty"`
	OK    bool            `json:"ok"`
	Error string          `json:"error,omitempty"`
	Port  uint16          `json:"port,omitempty"`
	// status
	Instance   string `json:"instance,omitempty"`
	AccountID  uint32 `json:"account_id,omitempty"`
	InstanceIP string `json:"instance_ip,omitempty"`
	Leader     *bool  `json:"leader,omitempty"`
	Connected  *bool  `json:"coordinator_connected,omitempty"`
	Peers      *int   `json:"peers,omitempty"`
	Bindings   *int   `json:"bindings,omitempty"`
	LocalOnly  *bool  `json:"local_only,omitempty"`
	ControlTLS *bool  `json:"control_tls,omitempty"`
	// CoordinatorUnreachable: a heard coordinator is excluded after failed joins.
	CoordinatorUnreachable *bool           `json:"coordinator_unreachable,omitempty"`
	UnixMaxDgramQlen       *int            `json:"unix_max_dgram_qlen,omitempty"`
	Counters               *statusCounters `json:"counters,omitempty"`
}

// statusCounters are cumulative datagram counters since broker start.
type statusCounters struct {
	GuestFrames    uint64 `json:"guest_frames"`
	UnixPeerFrames uint64 `json:"unix_peer_frames"` // sent to same-host peers over AF_UNIX
	UDPPeerFrames  uint64 `json:"udp_peer_frames"`  // sent to remote peers over UDP
	UDPFramesIn    uint64 `json:"udp_frames_in"`
	UDPRejected    uint64 `json:"udp_rejected"`
	Delivered      uint64 `json:"delivered"`
	Forged         uint64 `json:"forged"`
	Malformed      uint64 `json:"malformed"`
	UnknownSender  uint64 `json:"unknown_sender"`
	NoRoute        uint64 `json:"no_route"`
	NoBinding      uint64 `json:"no_binding"`
	SendDrops      uint64 `json:"send_drops"`
	NATAnswered    uint64 `json:"nat_answered"`
	NATDropped     uint64 `json:"nat_dropped"`
}

func (s *brokerStats) snapshot() *statusCounters {
	return &statusCounters{
		GuestFrames: s.guestFrames.Load(), UnixPeerFrames: s.toLocalPeer.Load(), UDPPeerFrames: s.toRemotePeer.Load(),
		UDPFramesIn: s.udpIn.Load(), UDPRejected: s.udpRejected.Load(), Delivered: s.delivered.Load(),
		Forged: s.forged.Load(), Malformed: s.malformed.Load(), UnknownSender: s.unknownSender.Load(),
		NoRoute: s.noRoute.Load(), NoBinding: s.noBinding.Load(), SendDrops: s.sendDrops.Load(),
		NATAnswered: s.natAnswered.Load(), NATDropped: s.natDropped.Load(),
	}
}

func (b *Broker) serveControl() {
	for {
		c, err := b.ctlLn.AcceptUnix()
		if err != nil {
			return
		}
		if !b.track(c) {
			_ = c.Close()
			return
		}
		b.goRun(func() {
			defer b.untrack(c)
			defer c.Close()
			b.handleControl(c)
		})
	}
}

func (b *Broker) track(c net.Conn) bool {
	b.connMu.Lock()
	defer b.connMu.Unlock()
	if b.ctx.Err() != nil {
		return false
	}
	b.conns[c] = struct{}{}
	return true
}

func (b *Broker) untrack(c net.Conn) {
	b.connMu.Lock()
	delete(b.conns, c)
	b.connMu.Unlock()
}

func (b *Broker) handleControl(c *net.UnixConn) {
	w := &lineWriter{w: c, conn: c}
	// Linux: SO_PEERCRED (uid and pid). Windows: SIO_AF_UNIX_GETPEERPID (pid) with the private
	// directory's DACL supplying the account check. Both admit only the configured process.
	if !authenticateControl(c, b.opt.ControlPeerPID) {
		_ = w.send(controlResponse{Error: "forbidden"})
		return
	}
	r := bufio.NewReaderSize(c, maxControlLine+1)
	for {
		line, err := readLine(r, maxControlLine)
		if err != nil {
			if errors.Is(err, errLineTooLong) {
				_ = w.send(controlResponse{Error: "too_large"})
			}
			return
		}
		var req controlRequest
		if err := strictUnmarshal(line, &req); err != nil {
			if w.send(controlResponse{Error: "bad_request"}) != nil {
				return
			}
			continue
		}
		resp := b.control(req)
		resp.ID = req.ID
		if w.send(resp) != nil {
			return
		}
	}
}

func (b *Broker) control(req controlRequest) controlResponse {
	switch req.Op {
	case "bind":
		if req.Socket == nil || req.Port == nil || *req.Port < 0 || *req.Port > 65535 {
			return controlResponse{Error: "bad_request"}
		}
		port, code := b.bind(*req.Socket, uint16(*req.Port))
		if code != "" {
			return controlResponse{Error: code}
		}
		return controlResponse{OK: true, Port: port}
	case "unbind":
		if req.Socket == nil || req.Port != nil {
			return controlResponse{Error: "bad_request"}
		}
		if code := b.unbind(*req.Socket); code != "" {
			return controlResponse{Error: code}
		}
		return controlResponse{OK: true}
	case "status":
		return b.status()
	}
	return controlResponse{Error: "unknown_op"}
}

// clientPath accepts only a socket node owned by us inside the runtime
// directory, reached without symlinks, that is not one of the broker's files.
func (b *Broker) clientPath(p string) string {
	if !validSocketPath(p) {
		return "bad_path"
	}
	rel, err := filepath.Rel(b.runtimeDir, p)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return "bad_path"
	}
	if p == b.ctlPath || p == b.dataPath {
		return "bad_path"
	}
	real, err := filepath.EvalSymlinks(p)
	if err != nil {
		return "not_socket"
	}
	if real != filepath.Join(b.runtimeReal, rel) {
		return "bad_path"
	}
	fi, err := os.Lstat(p)
	if err != nil || fi.Mode()&os.ModeSocket == 0 {
		return "not_socket"
	}
	if !ownedByCurrentUser(fi, p) {
		return "bad_path"
	}
	return ""
}

func (b *Broker) bind(path string, port uint16) (uint16, string) {
	if code := b.clientPath(path); code != "" {
		return 0, code
	}
	if !b.dataT.hasClient(path) {
		return 0, "not_connected" // Windows: the socket's stream has not introduced itself yet
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pid == 0 {
		return 0, "not_ready"
	}
	if _, ok := b.bindings[path]; ok {
		return 0, "already_bound"
	}
	if port == 0 {
		const lo, span = 49152, 65536 - 49152
		start := rand.IntN(span)
		for i := 0; i < span; i++ {
			cand := uint16(lo + (start+i)%span)
			if _, used := b.byPort[cand]; !used {
				port = cand
				break
			}
		}
		if port == 0 {
			return 0, "port_in_use"
		}
	} else if _, used := b.byPort[port]; used {
		return 0, "port_in_use"
	}
	b.bindings[path] = port
	b.byPort[port] = path
	return port, ""
}

func (b *Broker) unbind(path string) string {
	b.mu.Lock()
	defer b.mu.Unlock()
	port, ok := b.bindings[path]
	if !ok {
		return "not_bound"
	}
	delete(b.bindings, path)
	if b.byPort[port] == path {
		delete(b.byPort, port)
	}
	return ""
}

// releaseBinding drops the registration of a guest socket that proved to be gone, like a
// closed UDP socket releasing its port. Only the exact publication is removed.
func (b *Broker) releaseBinding(path string, port uint16) {
	b.mu.Lock()
	if b.bindings[path] == port {
		delete(b.bindings, path)
		delete(b.byPort, port)
	}
	b.mu.Unlock()
}

// releaseClient drops whatever binding a departed guest socket held (Windows: its stream closed).
func (b *Broker) releaseClient(path string) {
	b.mu.Lock()
	if port, ok := b.bindings[path]; ok {
		delete(b.bindings, path)
		if b.byPort[port] == path {
			delete(b.byPort, port)
		}
	}
	b.mu.Unlock()
}

func (b *Broker) status() controlResponse {
	link, _ := b.currentLink()
	connected := link != nil
	leader := b.leading.Load()
	localOnly := !b.lanIP.IsValid()
	unreachable := b.coordinatorUnreachable()
	controlTLS := b.opt.ControlTLS
	qlen := b.dgramQlen
	b.mu.RLock()
	defer b.mu.RUnlock()
	peers, bindings := len(b.peers), len(b.bindings)
	resp := controlResponse{OK: true, Instance: b.instance, AccountID: b.pid, Leader: &leader,
		Connected: &connected, Peers: &peers, Bindings: &bindings, LocalOnly: &localOnly, ControlTLS: &controlTLS,
		CoordinatorUnreachable: &unreachable, UnixMaxDgramQlen: &qlen, Counters: b.stats.snapshot()}
	if b.vip.IsValid() {
		resp.InstanceIP = b.vip.String()
	}
	return resp
}

// --- local TCP service listeners ---

func (b *Broker) serveProxy(ln net.Listener, service string) {
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		if !b.track(c) {
			_ = c.Close()
			return
		}
		b.goRun(func() {
			defer b.untrack(c)
			defer c.Close()
			link := b.waitForLink(5 * time.Second)
			if link == nil {
				return
			}
			up, err := link.openStream(service)
			if err != nil {
				return
			}
			if !b.track(up) {
				_ = up.Close()
				return
			}
			defer b.untrack(up)
			defer up.Close()
			stop := make(chan struct{})
			defer close(stop)
			go func() {
				// A coordinator change resets its streams; the game reconnects.
				select {
				case <-link.done:
					_ = up.Close()
					_ = c.Close()
				case <-stop:
				}
			}()
			pipe(c, up)
		})
	}
}

// pipe copies both directions. A clean EOF half-closes the other side; an
// error tears down both.
func pipe(a, b net.Conn) {
	errc := make(chan error, 2)
	cp := func(dst, src net.Conn) {
		_, err := io.Copy(dst, src)
		if err == nil {
			if cw, ok := dst.(interface{ CloseWrite() error }); ok {
				_ = cw.CloseWrite()
			}
		} else {
			_ = dst.Close()
			_ = src.Close()
		}
		errc <- err
	}
	go cp(a, b)
	go cp(b, a)
	<-errc
	<-errc
}
