package nsoserver

import (
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// Exercise the real authenticated endpoint wiring, not a separately constructed
// matchmaking handler. This proves service continuation, not the game's UI.
func TestRematchDepartureAndNextPlayer(t *testing.T) {
	for _, tc := range []struct {
		name                     string
		hostLeaves, migrateOwner bool
	}{
		{"joiner_leaves", false, false},
		{"host_leaves_and_survivor_acquires", true, true},
		{"host_only_preserves_departed_owner", true, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := newDynamicServer(t)
			for name, pid := range map[string]uint64{"49153": 49153, "49154": 49154, "49155": 49155} {
				if err := s.Resolver.Add(name, pid); err != nil {
					t.Fatal(err)
				}
			}
			a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
			b := connectPlayer(t, s, "49154", "192.168.50.12:40002")
			register(t, a, s, "192.168.50.11", 49821)
			register(t, b, s, "192.168.50.12", 49822)
			callID := uint32(100)
			call := func(c *liteClient, proto uint16, method uint32, body []byte) *nex.RMCMessage {
				t.Helper()
				callID++
				r, err := c.call(proto, method, callID, body)
				if err != nil || r == nil || r.IsError {
					t.Fatalf("protocol %x method %d: err=%v reply=%+v", proto, method, err, r)
				}
				return r
			}
			match := func(c *liteClient) nex.MatchmakeSession {
				return sessionOf(t, s, call(c, nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, autoMatchmakeBody(s.SecureSettings, false)))
			}
			initial := match(a)
			if joined := match(b); joined.ID != initial.ID || joined.NumParticipants != 2 {
				t.Fatal("initial pair did not join")
			}
			leaver, survivor, survivorPort := b, a, 49821
			if tc.hostLeaves {
				leaver, survivor, survivorPort = a, b, 49822
			}
			gid := nex.NewStreamOut(s.SecureSettings)
			gid.U32(initial.ID)
			leave := nex.NewStreamOut(s.SecureSettings)
			leave.U32(initial.ID)
			leave.String("")
			call(leaver, nex.ProtocolMatchMakingExt, nex.MethodEndParticipation, leave.Bytes())
			// Live Smash 13.0.5 asks for the existing host's station between the
			// host's EndParticipation and its own UpdateSessionHost. The host's
			// authenticated connection is still alive during this handover.
			// An empty success here stalled the guest before it could acquire.
			beforeAcquire := call(survivor, nex.ProtocolMatchMaking, nex.MethodGetSessionURLs, gid.Bytes())
			previousURLs := nex.ReadList(nex.NewStreamIn(beforeAcquire.Body, s.SecureSettings), func(in *nex.StreamIn) *nex.StationURL { return in.StationURLValue() })
			if len(previousURLs) == 0 || previousURLs[0].GetInt("port") != 49821 || previousURLs[0].GetInt("RVCID") != int(a.conn.ID) {
				t.Fatalf("handover lost the still-connected recorded host route: %v", previousURLs)
			}
			// Repeat departure and the old connection's eventual disconnect cannot
			// tear down the survivor's gathering.
			call(leaver, nex.ProtocolMatchMakingExt, nex.MethodEndParticipation, leave.Bytes())
			if tc.hostLeaves {
				host := nex.NewStreamOut(s.SecureSettings)
				host.U32(initial.ID)
				host.Bool(tc.migrateOwner) // e91 observed true; false remains a service-only case
				call(survivor, nex.ProtocolMatchMaking, nex.MethodUpdateSessionHost, host.Bytes())
			}
			leaver.conn.Close()
			if parts := s.MM.SessionByPID(survivor.conn.PID); len(parts) != 1 || parts[0] != survivor.conn.PID {
				t.Fatalf("surviving membership: %v", parts)
			}
			urlsReply := call(survivor, nex.ProtocolMatchMaking, nex.MethodGetSessionURLs, gid.Bytes())
			urls := nex.ReadList(nex.NewStreamIn(urlsReply.Body, s.SecureSettings), func(in *nex.StreamIn) *nex.StationURL { return in.StationURLValue() })
			if len(urls) == 0 || urls[0].GetInt("port") != survivorPort || urls[0].GetInt("RVCID") != int(survivor.conn.ID) {
				t.Fatalf("surviving host route: %v", urls)
			}
			// Explicit reopening models the client's action. Do not claim that the
			// real game issues it solely because this service test does.
			call(survivor, nex.ProtocolMatchmakeExtension, nex.MethodOpenParticipation, gid.Bytes())
			returning := connectPlayer(t, s, "49155", "192.168.50.13:40003")
			joined := match(returning)
			wantOwner := survivor.conn.PID
			if tc.hostLeaves && !tc.migrateOwner {
				// Host-only acquisition does not grant ownership. This known policy
				// leaves owner-only writes unavailable until explicit migration;
				// it is not evidence of graceful game/UI continuation for this case.
				wantOwner = leaver.conn.PID
			}
			if joined.ID != initial.ID || joined.HostPID != survivor.conn.PID || joined.OwnerPID != wantOwner || joined.NumParticipants != 2 {
				t.Fatalf("continuation lost room/host/owner: %+v", joined)
			}
		})
	}
}
