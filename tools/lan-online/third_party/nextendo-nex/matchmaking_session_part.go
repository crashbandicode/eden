package nex

import (
	"fmt"
	"strings"
)

// UpdateMatchmakeSessionParam.ModificationFlags bits, in the order of the fields they
// select (the same order as the structure): bit 0 attributes, bit 1 open participation,
// bit 2 application buffer, ... bit 7 game mode, bit 9 min participants, bit 10 max
// participants, bit 14 codeword. modFlagOpenParticipation and modFlagCodeword are
// declared beside the Animal Crossing handling.
const (
	modFlagAttributes        uint32 = 0x1
	modFlagApplicationBuffer uint32 = 0x4
	modFlagGameMode          uint32 = 0x80
	modFlagMinParticipants   uint32 = 0x200
	modFlagMaxParticipants   uint32 = 0x400
)

// applySessionPartFlags applies an UpdateMatchmakeSessionPart (0x6D.0x2C) the way the
// real server does: only the fields its ModificationFlags select, and only for a member
// of the gathering. It is the default path; titles that set SessionPartPersists keep
// applySessionPart and its defensive codeword handling instead.
//
// Smash's Quickplay needs this. A console that answers "Rematch? Yes" goes straight back
// to its waiting area in the same session, whose participation the host closed when the
// match started (0x6D.0x01). The console reopens it with this call (OpenParticipation,
// bit 0x2), and only then can a new player's AutoMatchmake join it. An acknowledgement
// alone left the gathering closed, so nobody could ever be matched to that console.
//
// It returns the names of the applied fields, and false when the gathering is unknown or
// the caller isn't one of its members (nothing is applied then).
func (m *Matchmaking) applySessionPartFlags(pid uint64, p *UpdateMatchmakeSessionParam) ([]string, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()

	g := m.gatherings[p.GID]
	if g == nil || g.session == nil {
		return nil, false
	}
	if g.session.OwnerPID != pid && g.session.HostPID != pid && !containsPID(g.participants, pid) {
		return nil, false
	}
	f := p.ModificationFlags
	var applied []string
	if f&modFlagAttributes != 0 {
		g.session.Attribs = append([]uint32(nil), p.Attributes...)
		applied = append(applied, "attributes")
	}
	if f&modFlagOpenParticipation != 0 {
		g.session.OpenParticipation = p.OpenParticipation
		applied = append(applied, fmt.Sprintf("open=%t", p.OpenParticipation))
	}
	if f&modFlagApplicationBuffer != 0 {
		g.session.ApplicationData = append([]byte(nil), p.ApplicationBuffer...)
		applied = append(applied, "application_buffer")
	}
	if f&modFlagGameMode != 0 && p.GameMode != 0 {
		g.session.GameMode = p.GameMode
		applied = append(applied, fmt.Sprintf("game_mode=%d", p.GameMode))
	}
	if f&modFlagMinParticipants != 0 && p.MinParticipants != 0 {
		g.session.MinParticipants = p.MinParticipants
		applied = append(applied, fmt.Sprintf("min=%d", p.MinParticipants))
	}
	if f&modFlagMaxParticipants != 0 && p.MaxParticipants != 0 {
		g.session.MaxParticipants = p.MaxParticipants
		applied = append(applied, fmt.Sprintf("max=%d", p.MaxParticipants))
	}
	return applied, true
}

func describeSessionPart(applied []string) string {
	if len(applied) == 0 {
		return "nothing flagged"
	}
	return strings.Join(applied, " ")
}
