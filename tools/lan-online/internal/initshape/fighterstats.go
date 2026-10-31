package initshape

import (
	"encoding/binary"
	"sync"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// These layouts come from the 13.0.5 client's serializers, not captured server
// bytes. Method 82 writes one record (0x284ee0); method 84 reads a map of the
// same records and a summary (0x28ee90, 0x298420, 0x2852f0). The game's consumer
// iterates fighter keys 0..98 (0x172cfe0). The report builder also emits 0xff
// for an unmapped fighter (0x172ce38); preserve that record without presenting
// it as a playable fighter. Field semantics beyond the fighter key
// are deliberately opaque here: the game reports its values; the service must
// not count calls as wins or invent a rating formula.
const (
	fighterCount      = 99
	fighterRecordSize = 30
	maxFighterPlayers = 256
)

type fighterRecords struct {
	mu      sync.Mutex
	players map[uint64]map[byte][fighterRecordSize]byte
	path    string
}

func validFighterRecord(b []byte) bool {
	return len(b) == fighterRecordSize && b[0] == 0 && binary.LittleEndian.Uint32(b[1:5]) == 25 && (b[5] < fighterCount || b[5] == 0xff)
}

// answerFighterStats is reached only for the opt-in stateful implementation.
// The authenticated connection supplies the principal. No request field can
// select another player's records. State is persistent when a store is loaded.
func (r *Responder) answerFighterStats(s *nex.Settings, pid uint64, req *nex.RMCMessage) (*nex.RMCMessage, Kind) {
	fail := func(code uint32) (*nex.RMCMessage, Kind) {
		return nex.NewRMCError(s, ProtocolDataStore, req.CallID, code), KindFighterStats
	}
	if pid == 0 {
		return fail(nex.ResultCoreAccessDenied)
	}
	if req.Method == 82 {
		b := req.Body
		// Only the observed v0 structure is supported. Validate every byte boundary
		// before allocating or changing a record; trailing data is not accepted.
		if !validFighterRecord(b) {
			return fail(nex.ResultCoreInvalidArgument)
		}
		var record [fighterRecordSize]byte
		copy(record[:], b)
		r.fighters.mu.Lock()
		defer r.fighters.mu.Unlock()
		player := r.fighters.players[pid]
		if player == nil {
			if len(r.fighters.players) >= maxFighterPlayers {
				return fail(nex.ResultCoreAccessDenied)
			}
			player = make(map[byte][fighterRecordSize]byte)
			r.fighters.players[pid] = player
		}
		previous, existed := player[b[5]]
		player[b[5]] = record
		if r.fighters.path != "" {
			if err := writeFighterState(r.fighters.path, r.fighters.players); err != nil {
				if existed {
					player[b[5]] = previous
				} else {
					delete(player, b[5])
				}
				if len(player) == 0 {
					delete(r.fighters.players, pid)
				}
				if r.opt.Logf != nil {
					r.opt.Logf("[fighter-stats] persistence failed: %v", err)
				}
				return fail(nex.ResultCoreUnknown)
			}
		}
		return nex.NewRMCSuccess(s, ProtocolDataStore, req.Method, req.CallID, nil), KindFighterStats
	}
	if len(req.Body) != 0 {
		return fail(nex.ResultCoreInvalidArgument)
	}
	r.fighters.mu.Lock()
	defer r.fighters.mu.Unlock()
	player := r.fighters.players[pid]
	out := nex.NewStreamOut(s)
	out.U32(uint32(len(player)))
	// Stable key order also makes independent byte-level probes reproducible.
	for key := 0; key < fighterCount; key++ {
		if record, ok := player[byte(key)]; ok {
			out.U8(byte(key))
			out.Write(record[:])
		}
	}
	if record, ok := player[0xff]; ok {
		out.U8(0xff)
		out.Write(record[:])
	}
	// The aggregate structure is required even for an empty map. Its source and
	// semantics are still being traced; this opt-in candidate returns neutral
	// values and does not claim to implement an Elite threshold.
	out.U8(0)
	out.U32(20)
	out.U32(0)
	out.U32(0)
	out.U32(0)
	out.U64(0)
	return nex.NewRMCSuccess(s, ProtocolDataStore, req.Method, req.CallID, out.Bytes()), KindFighterStats
}

// ExchangeFighterStats exposes the same validated service to a local broker.
// The durable store's principal is local to that profile, never a leased PID.
func (r *Responder) ExchangeFighterStats(pid uint64, method uint32, body []byte) ([]byte, uint32) {
	if method != 82 && method != 84 {
		return nil, nex.ResultCoreInvalidArgument
	}
	s := nex.NewSwitchSettings("9587602b", 40000)
	req := nex.NewRMCRequest(s, ProtocolDataStore, method, 1, body)
	resp, _ := r.answerFighterStats(s, pid, req)
	if resp.IsError {
		return nil, resp.Result
	}
	return resp.Body, 0
}
