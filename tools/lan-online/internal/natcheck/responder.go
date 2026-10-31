// Package natcheck implements the minimal UDP NAT-check responder contract that
// two independent client implementations (the citron-nextendo probe and the
// Ryujinx-Nextendo network check, both reading the console's own protocol)
// agree on:
//
//	request:  >= 16 bytes, big-endian u32 test id at offset 0 (the Switch sends
//	          101/102/103; the clients send 101 and zero padding)
//	reply:    exactly 16 bytes, four big-endian u32 fields:
//	          [echoed test id][observed source UDP port][observed source IPv4][responder IPv4]
//	          sent from the same socket the probe arrived on
//	ports:    10025 and 10125 on each of two distinguishable responder addresses
//
// Nothing beyond that is implemented: semantics of test ids other than the echo
// (for example alternate-port replies) are not documented in the available
// sources and are deliberately not guessed. Unknown ids are still echoed, which
// is what the clients above expect, and logged once per id.
package natcheck

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"sort"
	"strings"
	"sync"
)

// ProbeLen is the minimum request length and the exact reply length.
const ProbeLen = 16

// Observation is the last endpoint seen from a source IP.
type Observation struct {
	IP   string
	Port int
}

// Responder serves one or more UDP sockets.
type Responder struct {
	Addrs []string // IPv4 literals to bind
	Ports []int
	// ObservationFile receives "<ip> <port>" lines (upstream natbridge shape); optional.
	ObservationFile string
	Logf            func(format string, args ...any)

	mu       sync.Mutex
	observed map[string]int
	seenIDs  map[uint32]bool
	conns    []net.PacketConn
	// Probes counts answered probes.
	Probes uint64
}

// Reply encodes the 16-byte response.
func Reply(testID uint32, src netip.AddrPort, server netip.Addr) []byte {
	out := make([]byte, ProbeLen)
	binary.BigEndian.PutUint32(out[0:4], testID)
	binary.BigEndian.PutUint32(out[4:8], uint32(src.Port()))
	a4 := src.Addr().Unmap().As4()
	copy(out[8:12], a4[:])
	s4 := server.Unmap().As4()
	copy(out[12:16], s4[:])
	return out
}

// Listen binds every (addr, port) pair; it fails if any bind fails (fail closed).
func (r *Responder) Listen() error {
	if len(r.Addrs) == 0 || len(r.Ports) == 0 {
		return errors.New("natcheck: no addresses or ports")
	}
	r.observed = map[string]int{}
	r.seenIDs = map[uint32]bool{}
	for _, a := range r.Addrs {
		if ip, err := netip.ParseAddr(a); err != nil || !ip.Is4() {
			r.closeAll()
			return fmt.Errorf("natcheck: %q is not an IPv4 literal", a)
		}
		for _, p := range r.Ports {
			pc, err := net.ListenPacket("udp4", net.JoinHostPort(a, fmt.Sprint(p)))
			if err != nil {
				r.closeAll()
				return fmt.Errorf("natcheck: bind %s:%d: %w", a, p, err)
			}
			r.conns = append(r.conns, pc)
		}
	}
	return nil
}

// LocalAddrs lists the bound socket addresses (for logs and tests).
func (r *Responder) LocalAddrs() []string {
	out := make([]string, 0, len(r.conns))
	for _, c := range r.conns {
		out = append(out, c.LocalAddr().String())
	}
	return out
}

// Serve answers probes until ctx is done, then closes the sockets.
func (r *Responder) Serve(ctx context.Context) {
	var wg sync.WaitGroup
	for _, pc := range r.conns {
		wg.Add(1)
		go func(pc net.PacketConn) {
			defer wg.Done()
			r.serveOne(pc)
		}(pc)
	}
	<-ctx.Done()
	r.closeAll()
	wg.Wait()
}

func (r *Responder) serveOne(pc net.PacketConn) {
	local, _ := netip.ParseAddrPort(pc.LocalAddr().String())
	buf := make([]byte, 512)
	for {
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			return // closed
		}
		if n < ProbeLen {
			continue
		}
		src, ok := from.(*net.UDPAddr)
		if !ok || src.IP.To4() == nil {
			continue
		}
		srcAP := netip.AddrPortFrom(netip.AddrFrom4([4]byte(src.IP.To4())), uint16(src.Port))
		testID := binary.BigEndian.Uint32(buf[0:4])
		// Record (and persist the observation file) BEFORE replying, so anything
		// that reads the observation after seeing the reply finds it present.
		r.record(testID, srcAP)
		_, _ = pc.WriteTo(Reply(testID, srcAP, local.Addr()), from)
	}
}

// ProbeCount returns how many probes were answered.
func (r *Responder) ProbeCount() uint64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.Probes
}

func (r *Responder) record(testID uint32, src netip.AddrPort) {
	r.mu.Lock()
	r.Probes++
	first := !r.seenIDs[testID]
	r.seenIDs[testID] = true
	r.observed[src.Addr().String()] = int(src.Port())
	snapshot := r.snapshotLocked()
	r.mu.Unlock()
	if first && r.Logf != nil {
		r.Logf("[natcheck] first probe with test id %d from %s", testID, src)
	}
	if r.ObservationFile != "" {
		_ = writeObservations(r.ObservationFile, snapshot)
	}
}

func (r *Responder) snapshotLocked() []Observation {
	out := make([]Observation, 0, len(r.observed))
	for ip, port := range r.observed {
		out = append(out, Observation{IP: ip, Port: port})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].IP < out[j].IP })
	return out
}

// Observations returns the last port seen per source IP.
func (r *Responder) Observations() []Observation {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.snapshotLocked()
}

func writeObservations(path string, obs []Observation) error {
	var b strings.Builder
	for _, o := range obs {
		fmt.Fprintf(&b, "%s %d\n", o.IP, o.Port)
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, []byte(b.String()), 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func (r *Responder) closeAll() {
	for _, c := range r.conns {
		_ = c.Close()
	}
}
