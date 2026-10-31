package nsoserver

import (
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// Yoga's 13.0.5 trace shows VIP mode3 and Quickplay mode1 both serialize
// public GameMode1. The deployed matcher ignores attribute/search differences,
// so it already allows mixed public searches without a new server-side timer.
// These synthetic probes establish that service policy, not the game's UI or
// an exact captured VIP parameter encoding.
func TestPublicMatchmakingAllowsDifferentAttributesButKeepsModesSeparate(t *testing.T) {
	for _, mode := range []uint32{1, 2} {
		s := newDynamicServer(t)
		for name, pid := range map[string]uint64{"49153": 49153, "49154": 49154} {
			if err := s.Resolver.Add(name, pid); err != nil {
				t.Fatal(err)
			}
		}
		a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
		b := connectPlayer(t, s, "49154", "192.168.50.12:40002")
		register(t, a, s, "192.168.50.11", 49821)
		register(t, b, s, "192.168.50.12", 49822)
		match := func(c *liteClient, mode, attr uint32) nex.MatchmakeSession {
			t.Helper()
			p := &nex.AutoMatchmakeParam{Session: nex.MatchmakeSession{
				Gathering: nex.Gathering{MinParticipants: 2, MaxParticipants: 2}, GameMode: mode,
				OpenParticipation: true, Attribs: []uint32{attr, attr, attr, attr, attr, attr},
			}}
			out := nex.NewStreamOut(s.SecureSettings)
			out.Add(p)
			r, err := c.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 100, out.Bytes())
			if err != nil || r == nil {
				t.Fatalf("match: %v", err)
			}
			return sessionOf(t, s, r)
		}
		first := match(a, 1, 0)
		second := match(b, mode, 1)
		if (first.ID == second.ID) != (mode == 1) {
			t.Fatalf("public mode %d: first=%d second=%d", mode, first.ID, second.ID)
		}
		a.conn.Close()
		b.conn.Close()
	}
}
