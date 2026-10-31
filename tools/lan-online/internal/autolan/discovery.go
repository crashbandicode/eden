package autolan

import (
	"encoding/json"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	beaconProto   = "eden-nsolan-auto"
	roleCandidate = "candidate"
	roleLeader    = "leader"
	roleMember    = "member" // same-host record only; never on the network

	beaconFresh       = 3 * time.Second
	localRefreshEvery = 500 * time.Millisecond
	mcastSendEvery    = 300 * time.Millisecond
	maxBeaconBytes    = 1024
)

// beaconGroup is an organization-local (239.255/16) group; TTL 1 keeps beacons
// on the local link.
var beaconGroup = netip.MustParseAddrPort("239.255.77.61:50731")

// Beacon advertises a broker. Unix, OSPID, LocalKey, DataSocket and VIP are
// same-host fields: they appear only in the record in our private shared
// directory and are stripped from (and rejected in) network beacons.
type Beacon struct {
	Proto      string `json:"eln"`
	V          int    `json:"v"`
	Title      string `json:"title"`
	Instance   string `json:"instance"`
	Role       string `json:"role"`
	Epoch      uint64 `json:"epoch,omitempty"`
	TCP        string `json:"tcp,omitempty"`
	Unix       string `json:"unix,omitempty"`
	LocalKey   string `json:"local_key,omitempty"`
	OSPID      int    `json:"os_pid,omitempty"`
	DataSocket string `json:"data_socket,omitempty"`
	VIP        string `json:"vip,omitempty"`
	// TLS is the broker's control-TLS mode (part of compatibility). A TLS leader
	// advertises its coordinator certificate's SHA-256 fingerprint in TLSFP.
	TLS   bool   `json:"tls,omitempty"`
	TLSFP string `json:"tls_fp,omitempty"`

	Local bool `json:"-"` // heard through same-host metadata
}

// Announcer publishes this broker's beacon and reports fresh beacons of others.
type Announcer interface {
	Publish(b *Beacon) // nil withdraws
	Heard() []Beacon
	Close() error
}

func (b *Beacon) valid() bool {
	if b.Proto != beaconProto || b.V != protocolVersion || !isInstanceID(b.Instance) || len(b.Title) != 16 {
		return false
	}
	switch b.Role {
	case roleCandidate, roleMember:
		return b.TCP == "" && b.Unix == "" && b.TLSFP == ""
	case roleLeader:
		// A TLS leader must name its certificate; a plaintext leader must not.
		return b.TLS == isHex64(b.TLSFP) && (b.TLS || b.TLSFP == "")
	}
	return false
}

// networkCopy is the only form a beacon may take off this host. Members send
// no network beacon at all.
func networkCopy(b *Beacon) (Beacon, bool) {
	if b == nil || b.Role == roleMember {
		return Beacon{}, false
	}
	c := *b
	c.Unix, c.OSPID, c.LocalKey, c.DataSocket, c.VIP, c.Local = "", 0, "", "", "", false
	return c, true
}

// --- same-host metadata files ---

type localAnnouncer struct {
	dir  string
	self string
	mu   sync.Mutex
	cur  []byte
	stop chan struct{}
	wg   sync.WaitGroup
}

func newLocalAnnouncer(sharedDir, self string) *localAnnouncer {
	l := &localAnnouncer{dir: filepath.Join(sharedDir, "brokers"), self: self, stop: make(chan struct{})}
	l.wg.Add(1)
	go func() {
		defer l.wg.Done()
		t := time.NewTicker(localRefreshEvery)
		defer t.Stop()
		for {
			select {
			case <-l.stop:
				return
			case <-t.C:
				l.mu.Lock()
				cur := l.cur
				l.mu.Unlock()
				if cur != nil {
					_ = writeFileAtomic(l.path(), cur)
				}
			}
		}
	}()
	return l
}

func (l *localAnnouncer) path() string { return filepath.Join(l.dir, l.self+".json") }

func (l *localAnnouncer) Publish(b *Beacon) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if b == nil {
		l.cur = nil
		_ = os.Remove(l.path())
		return
	}
	c := *b
	c.OSPID = os.Getpid()
	c.Local = false
	data, err := json.Marshal(c)
	if err != nil {
		return
	}
	l.cur = data
	_ = writeFileAtomic(l.path(), data)
}

func (l *localAnnouncer) Heard() []Beacon {
	entries, err := os.ReadDir(l.dir)
	if err != nil {
		return nil
	}
	var out []Beacon
	for _, e := range entries {
		inst, ok := strings.CutSuffix(e.Name(), ".json")
		if !ok || !isInstanceID(inst) || inst == l.self {
			continue
		}
		if b, ok := readLocalBeacon(filepath.Join(l.dir, e.Name()), inst); ok {
			out = append(out, b)
		}
	}
	return out
}

// readLocalBeacon reads one same-host record: a regular file owned by us in the
// private shared directory, fresh, naming its own instance, of a live process.
// A crashed broker's record is reclaimed once it is clearly stale.
func readLocalBeacon(p, inst string) (Beacon, bool) {
	var b Beacon
	fi, err := os.Lstat(p)
	if err != nil || !fi.Mode().IsRegular() || fi.Size() > maxBeaconBytes {
		return Beacon{}, false
	}
	if !ownedByCurrentUser(fi, p) {
		return Beacon{}, false
	}
	data, err := os.ReadFile(p)
	if err != nil || strictUnmarshal(data, &b) != nil || !b.valid() || b.Instance != inst {
		return Beacon{}, false
	}
	age := time.Since(fi.ModTime())
	alive := processAlive(b.OSPID)
	if age > beaconFresh || !alive {
		if !alive && age > 3*beaconFresh {
			_ = os.Remove(p)
		}
		return Beacon{}, false
	}
	b.Local = true
	return b, true
}

func (l *localAnnouncer) Close() error {
	close(l.stop)
	l.wg.Wait()
	l.mu.Lock()
	l.cur = nil
	l.mu.Unlock()
	_ = os.Remove(l.path())
	return nil
}

// --- LAN multicast ---

type mcastAnnouncer struct {
	recv          *net.UDPConn
	send          *net.UDPConn
	self          string
	allowLoopback bool
	mu            sync.Mutex
	cur           []byte
	heard         map[string]heardBeacon
	stop          chan struct{}
	wg            sync.WaitGroup
}

type heardBeacon struct {
	b  Beacon
	at time.Time
}

func newMulticastAnnouncer(ifi *net.Interface, src netip.Addr, self string, allowLoopback bool) (*mcastAnnouncer, error) {
	recv, err := net.ListenMulticastUDP("udp4", ifi, net.UDPAddrFromAddrPort(beaconGroup))
	if err != nil {
		return nil, err
	}
	send, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(src, 0)))
	if err != nil {
		_ = recv.Close()
		return nil, err
	}
	if rc, err := send.SyscallConn(); err == nil {
		setMulticastSendOptions(rc, src.As4())
	}
	m := &mcastAnnouncer{recv: recv, send: send, self: self, allowLoopback: allowLoopback,
		heard: map[string]heardBeacon{}, stop: make(chan struct{})}
	m.wg.Add(2)
	go m.readLoop()
	go m.sendLoop()
	return m, nil
}

func (m *mcastAnnouncer) readLoop() {
	defer m.wg.Done()
	buf := make([]byte, maxBeaconBytes+1)
	for {
		n, from, err := m.recv.ReadFromUDPAddrPort(buf)
		if err != nil {
			return
		}
		if b, ok := parseNetworkBeacon(buf[:n], from, m.self, m.allowLoopback); ok {
			m.mu.Lock()
			m.heard[b.Instance] = heardBeacon{b: b, at: time.Now()}
			m.mu.Unlock()
		}
	}
}

// parseNetworkBeacon accepts only beacons from a private LAN source (loopback
// only under the test option) that carry no same-host field, and a leader only
// when it advertises its own packet source address.
func parseNetworkBeacon(data []byte, from netip.AddrPort, self string, allowLoopback bool) (Beacon, bool) {
	var b Beacon
	src := from.Addr().Unmap()
	if len(data) > maxBeaconBytes || !(isLANAddress(src) || (allowLoopback && src.Is4() && src.IsLoopback())) {
		return Beacon{}, false
	}
	if strictUnmarshal(data, &b) != nil || !b.valid() || b.Instance == self || b.Role == roleMember ||
		b.Unix != "" || b.OSPID != 0 || b.LocalKey != "" || b.DataSocket != "" || b.VIP != "" {
		return Beacon{}, false
	}
	if b.Role == roleLeader {
		tcp, err := netip.ParseAddrPort(b.TCP)
		if err != nil || tcp.Addr() != src || tcp.Port() == 0 {
			return Beacon{}, false
		}
	}
	return b, true
}

func (m *mcastAnnouncer) sendLoop() {
	defer m.wg.Done()
	t := time.NewTicker(mcastSendEvery)
	defer t.Stop()
	dst := net.UDPAddrFromAddrPort(beaconGroup)
	for {
		select {
		case <-m.stop:
			return
		case <-t.C:
			m.mu.Lock()
			cur := m.cur
			m.mu.Unlock()
			if cur != nil {
				_, _ = m.send.WriteToUDP(cur, dst)
			}
		}
	}
}

func (m *mcastAnnouncer) Publish(b *Beacon) {
	var data []byte
	if c, ok := networkCopy(b); ok {
		data, _ = json.Marshal(c)
	}
	m.mu.Lock()
	m.cur = data
	m.mu.Unlock()
	if data != nil {
		_, _ = m.send.WriteToUDP(data, net.UDPAddrFromAddrPort(beaconGroup))
	}
}

func (m *mcastAnnouncer) Heard() []Beacon {
	m.mu.Lock()
	defer m.mu.Unlock()
	var out []Beacon
	for k, h := range m.heard {
		if time.Since(h.at) > beaconFresh {
			delete(m.heard, k)
			continue
		}
		out = append(out, h.b)
	}
	return out
}

func (m *mcastAnnouncer) Close() error {
	close(m.stop)
	_ = m.recv.Close()
	_ = m.send.Close()
	m.wg.Wait()
	return nil
}

// --- deterministic in-process network for tests ---

// memHub stands in for the LAN in tests. It carries exactly what multicast
// would carry (networkCopy), so tests see no same-host field from the network.
type memHub struct {
	mu sync.Mutex
	m  map[string]Beacon
}

func newMemHub() *memHub { return &memHub{m: map[string]Beacon{}} }

func (h *memHub) announcer(self string) Announcer { return &memAnnouncer{h: h, self: self} }

type memAnnouncer struct {
	h    *memHub
	self string
}

func (a *memAnnouncer) Publish(b *Beacon) {
	a.h.mu.Lock()
	defer a.h.mu.Unlock()
	c, ok := networkCopy(b)
	if !ok {
		delete(a.h.m, a.self)
		return
	}
	a.h.m[a.self] = c
}

func (a *memAnnouncer) Heard() []Beacon {
	a.h.mu.Lock()
	defer a.h.mu.Unlock()
	var out []Beacon
	for k, b := range a.h.m {
		if k != a.self {
			out = append(out, b)
		}
	}
	return out
}

func (a *memAnnouncer) Close() error {
	a.Publish(nil)
	return nil
}
