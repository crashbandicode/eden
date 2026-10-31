// Package initshape answers the DataStore (0x73) and Utility (0x6E) calls Smash
// Ultimate makes while entering online mode, using protocol-shaped values only.
//
// No bytes recorded from Nintendo's or Nextendo's servers are embedded here. Where
// the public protocol documentation gives a shape (Utility GetIntegerSettings is a
// Map<Uint16, Sint32>; DataStore PostMetaBinary returns a Uint64 data id), that
// shape is produced with neutral values. Where a method is undocumented and only
// known from upstream capture lengths (DataStore 60/61/84/95, Utility 9/10/11),
// the conservative fallback is an empty List (four zero bytes), which upstream
// reports was enough for the online menu but NOT for Quickplay (DataStore 60).
// Root can supply operator-provided override files through ReplayDir to measure
// whether a given method is the blocker; this repository never ships such files.
package initshape

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

const (
	ProtocolDataStore = uint16(0x73)
	ProtocolUtility   = uint16(0x6E)

	// DataStore::NotFound (documented DataStore result 0x0069 0004 with the error bit).
	resultDataStoreNotFound = uint32(0x00690004)
)

// Options tunes the responder.
type Options struct {
	// FighterExchange routes automatic-mode requests to the authenticated
	// principal's owning broker; paths and durable identities never cross LAN.
	FighterExchange func(uint64, string, uint32, []byte) ([]byte, uint32)
	// FighterStats enables the candidate per-principal report/read service for
	// DataStore 82/84. These methods then supersede global replay files. Disabled
	// until the aggregate/Elite contract and live game acceptance are established.
	FighterStats bool
	// UtilityIntSettings is the entry count for GetIntegerSettings (positions 0..n-1).
	UtilityIntSettings int
	// UtilityIntValues overrides individual positions; absent positions are zero.
	UtilityIntValues map[int]int32
	// ReplayDir optionally overrides responses from files "0x73_60.bin" etc.
	ReplayDir string
	// Logf receives one line per answered call; nil disables logging.
	Logf func(format string, args ...any)
}

// Responder serves both protocols.
type Responder struct {
	opt     Options
	utility nex.RMCHandler
	dataID  atomic.Uint64
	// Calls counts answered requests per (proto, method) for tests/diagnostics.
	callsMu  sync.Mutex
	calls    map[key]uint64
	fighters fighterRecords
}

type key struct {
	proto  uint16
	method uint32
}

// Kind classifies how a response was produced (reported in logs and tests).
type Kind string

const (
	KindDocumented   Kind = "documented-shape"
	KindEmptyList    Kind = "empty-list-fallback"
	KindEmptyBody    Kind = "empty-body"
	KindNotFound     Kind = "not-found"
	KindDelegated    Kind = "delegated-utility"
	KindOverride     Kind = "operator-override-file"
	KindFighterStats Kind = "fighter-stats"
)

// New builds a responder.
func New(opt Options) *Responder {
	return &Responder{opt: opt, utility: nex.UtilityHandler(), calls: map[key]uint64{}, fighters: fighterRecords{players: map[uint64]map[byte][fighterRecordSize]byte{}}}
}

// Register installs the responder for both protocols on an endpoint.
func (r *Responder) Register(ep *nex.Endpoint) {
	ep.Register(ProtocolDataStore, r.Handler(ProtocolDataStore))
	ep.Register(ProtocolUtility, r.Handler(ProtocolUtility))
}

// Handler returns the RMC handler for one protocol.
func (r *Responder) Handler(proto uint16) nex.RMCHandler {
	return func(conn *nex.Connection, req *nex.RMCMessage) *nex.RMCMessage {
		resp, kind := r.answer(conn.Settings, conn.PID, conn.RemoteAddr, proto, req)
		r.count(proto, req.Method)
		if r.opt.Logf != nil {
			n := 0
			if resp != nil {
				n = len(resp.Body)
			}
			r.opt.Logf("[initshape] pid=%d 0x%02x.%d call=%d -> %s (%d bytes)", conn.PID, proto, req.Method, req.CallID, kind, n)
		}
		return resp
	}
}

// Answer computes the response for a request without needing a live connection.
func (r *Responder) Answer(s *nex.Settings, proto uint16, req *nex.RMCMessage) (*nex.RMCMessage, Kind) {
	return r.answer(s, 0, "", proto, req)
}

func (r *Responder) answer(s *nex.Settings, pid uint64, remote string, proto uint16, req *nex.RMCMessage) (*nex.RMCMessage, Kind) {
	if r.opt.FighterExchange != nil && proto == ProtocolDataStore && (req.Method == 82 || req.Method == 84) {
		if pid == 0 || (req.Method == 82 && !validFighterRecord(req.Body)) || (req.Method == 84 && len(req.Body) != 0) {
			return nex.NewRMCError(s, proto, req.CallID, nex.ResultCoreInvalidArgument), KindFighterStats
		}
		body, code := r.opt.FighterExchange(pid, remote, req.Method, req.Body)
		if code != 0 {
			return nex.NewRMCError(s, proto, req.CallID, code), KindFighterStats
		}
		return nex.NewRMCSuccess(s, proto, req.Method, req.CallID, body), KindFighterStats
	}
	if r.opt.FighterStats && proto == ProtocolDataStore && (req.Method == 82 || req.Method == 84) {
		return r.answerFighterStats(s, pid, req)
	}
	if b, ok := r.overrideBytes(proto, req.Method); ok {
		return nex.NewRMCSuccess(s, proto, req.Method, req.CallID, b), KindOverride
	}
	switch proto {
	case ProtocolUtility:
		return r.answerUtility(s, req)
	case ProtocolDataStore:
		return r.answerDataStore(s, req)
	}
	return nex.NewRMCError(s, proto, req.CallID, nex.ResultCoreNotImplemented), KindNotFound
}

func (r *Responder) answerUtility(s *nex.Settings, req *nex.RMCMessage) (*nex.RMCMessage, Kind) {
	switch req.Method {
	case nex.MethodGetIntegerSettings:
		// Map<Uint16, Sint32>: u32 count, then (u16 key, s32 value) pairs. Keys are
		// positions 0..n-1 because clients read positions (upstream bisection).
		out := nex.NewStreamOut(s)
		n := r.opt.UtilityIntSettings
		out.U32(uint32(n))
		for k := 0; k < n; k++ {
			out.U16(uint16(k))
			out.S32(r.opt.UtilityIntValues[k])
		}
		return nex.NewRMCSuccess(s, ProtocolUtility, req.Method, req.CallID, out.Bytes()), KindDocumented
	case nex.MethodGetStringSettings:
		out := nex.NewStreamOut(s)
		out.U32(0) // empty Map<Uint16, String>
		return nex.NewRMCSuccess(s, ProtocolUtility, req.Method, req.CallID, out.Bytes()), KindDocumented
	case nex.MethodAcquireNexUniqueID, nex.MethodAcquireNexUniqueIDWithPassword,
		nex.MethodAssociateNexUniqueIDWithMyPrincipalID, nex.MethodGetAssociatedNexUniqueIDWithMyPrincipalID:
		// Documented methods already implemented by the core (PID-derived ids).
		return r.utility(&nex.Connection{Settings: s}, req), KindDelegated
	}
	return emptyList(s, ProtocolUtility, req), KindEmptyList
}

func (r *Responder) answerDataStore(s *nex.Settings, req *nex.RMCMessage) (*nex.RMCMessage, Kind) {
	switch req.Method {
	case 8: // GetMeta: nothing is stored, so NotFound is the truthful documented result.
		return nex.NewRMCError(s, ProtocolDataStore, req.CallID, resultDataStoreNotFound), KindNotFound
	case 21: // PostMetaBinary -> Uint64 dataId
		out := nex.NewStreamOut(s)
		out.U64(r.dataID.Add(1))
		return nex.NewRMCSuccess(s, ProtocolDataStore, req.Method, req.CallID, out.Bytes()), KindDocumented
	case 38, 82, 83: // 13.0.5 method38 reply parser 0x28d7e0 is void, as are 82/83
		return nex.NewRMCSuccess(s, ProtocolDataStore, req.Method, req.CallID, nil), KindEmptyBody
	}
	return emptyList(s, ProtocolDataStore, req), KindEmptyList
}

func emptyList(s *nex.Settings, proto uint16, req *nex.RMCMessage) *nex.RMCMessage {
	out := nex.NewStreamOut(s)
	out.U32(0)
	return nex.NewRMCSuccess(s, proto, req.Method, req.CallID, out.Bytes())
}

// OverrideFileName is the file name an operator override uses for a method.
func OverrideFileName(proto uint16, method uint32) string {
	return fmt.Sprintf("0x%02x_%d.bin", proto, method)
}

// Overrides lists the override files present in ReplayDir (names only), so the
// server can log at startup which methods an operator has overridden.
func (r *Responder) Overrides() []string {
	if r.opt.ReplayDir == "" {
		return nil
	}
	entries, err := os.ReadDir(r.opt.ReplayDir)
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		var proto uint16
		var method uint32
		if n, _ := fmt.Sscanf(e.Name(), "0x%02x_%d.bin", &proto, &method); n == 2 && OverrideFileName(proto, method) == e.Name() {
			out = append(out, e.Name())
		}
	}
	return out
}

func (r *Responder) overrideBytes(proto uint16, method uint32) ([]byte, bool) {
	if r.opt.ReplayDir == "" {
		return nil, false
	}
	b, err := os.ReadFile(filepath.Join(r.opt.ReplayDir, OverrideFileName(proto, method)))
	if err != nil {
		return nil, false
	}
	return b, true
}

func (r *Responder) count(proto uint16, method uint32) {
	r.callsMu.Lock()
	defer r.callsMu.Unlock()
	k := key{proto, method}
	r.calls[k]++
}

// Calls returns how many times a method was answered.
func (r *Responder) Calls(proto uint16, method uint32) uint64 {
	r.callsMu.Lock()
	defer r.callsMu.Unlock()
	return r.calls[key{proto, method}]
}
