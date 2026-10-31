package nex

import (
	"bytes"
	"encoding/json"
	"reflect"
	"strconv"
	"sync"
	"testing"
	"time"
)

// Original, synthetic fixtures. The independent oracle is the documented wire
// shape and explicit membership/identity facts, not a second migration algorithm.
const rematchGID uint32 = 731

type rematchFixture struct {
	s         *Settings
	ep        *Endpoint
	mm        *Matchmaking
	old, next *Connection
	noticeMu  sync.Mutex
	notices   map[uint64][]NotificationEvent
}

func newRematchFixture(t *testing.T) *rematchFixture {
	t.Helper()
	f := &rematchFixture{s: testSettings(), mm: NewMatchmaking(), notices: make(map[uint64][]NotificationEvent)}
	f.ep = NewEndpoint(f.s)
	f.mm.Endpoint = f.ep // same wiring as companion/internal/nsoserver/server.go
	f.old = f.connection(t, 71)
	f.next = f.connection(t, 72)
	f.mm.gatherings[rematchGID] = &gathering{
		session: &MatchmakeSession{
			Gathering:       Gathering{ID: rematchGID, OwnerPID: 71, HostPID: 71, MaxParticipants: 2},
			NumParticipants: 2, OpenParticipation: false, GameMode: 5,
			SessionKey: []byte("synthetic-test-key"), ApplicationData: []byte{9, 8}, Attribs: []uint32{7, 6},
		},
		participants: []uint64{71, 72}, hostConnID: f.old.ID, code: "TEST1",
	}
	f.mm.byCode["TEST1"] = rematchGID
	// Exercise the production callback boundary: it must receive the new host
	// connection, not an IP/old-connection-derived replacement.
	f.mm.SessionStations = func(joiner, host *Connection) []*StationURL { return host.Stations() }
	return f
}

func (f *rematchFixture) connection(t *testing.T, pid uint64) *Connection {
	t.Helper()
	c := NewConnection(f.ep, "192.0.2.1:40000", func(frame []byte) {
		// A socket write cannot retain the store lock, even when the write calls
		// back into matchmaking. TryLock makes a regression fail, not hang.
		if !f.mm.mu.TryLock() {
			t.Error("notification sent while holding matchmaking mutex")
			return
		}
		f.mm.mu.Unlock()
		packets, rest, err := DecodePackets(frame)
		if err != nil || len(rest) != 0 || len(packets) != 1 {
			t.Error("invalid notification packet")
			return
		}
		rmc, err := ParseRMC(f.s, packets[0].Payload)
		if err != nil || rmc.Protocol != ProtocolNotifications || rmc.Method != MethodProcessNotificationEvent {
			t.Error("unexpected outbound RMC")
			return
		}
		in := NewStreamIn(rmc.Body, f.s)
		var event NotificationEvent
		in.Extract(&event)
		if in.Err() != nil || !in.EOF() {
			t.Error("invalid notification body")
			return
		}
		f.noticeMu.Lock()
		f.notices[pid] = append(f.notices[pid], event)
		f.noticeMu.Unlock()
	})
	c.PID = pid
	f.ep.registerConnection(c)
	station := NewStationURL("prudp")
	station.Set("address", "192.0.2."+strconv.FormatUint(pid, 10))
	station.SetInt("RVCID", int(c.ID))
	c.SetStations([]*StationURL{station})
	return c
}

func (f *rematchFixture) host(c *Connection, method uint32, migrate bool) *RMCMessage {
	body := NewStreamOut(f.s)
	body.U32(rematchGID)
	if method == MethodUpdateSessionHost {
		body.Bool(migrate)
	}
	return f.mm.MatchMakingHandler()(c, NewRMCRequest(f.s, ProtocolMatchMaking, method, 42, body.Bytes()))
}

func (f *rematchFixture) leave(c *Connection) *RMCMessage {
	body := NewStreamOut(f.s)
	body.U32(rematchGID)
	body.String("bye")
	return f.mm.MatchMakingExtHandler()(c, NewRMCRequest(f.s, ProtocolMatchMakingExt, MethodEndParticipation, 1, body.Bytes()))
}

func requireRematchSuccess(t *testing.T, response *RMCMessage, body []byte) {
	t.Helper()
	if response == nil || response.IsError || !bytes.Equal(response.Body, body) {
		t.Fatal("expected successful response with documented body")
	}
}

func (f *rematchFixture) state(t *testing.T) []byte {
	t.Helper()
	f.mm.mu.Lock()
	defer f.mm.mu.Unlock()
	g := f.mm.gatherings[rematchGID]
	data, err := json.Marshal(struct {
		Session *MatchmakeSession
		Parts   []uint64
		Conn    uint32
		Codes   map[string]uint32
	}{g.session, g.participants, g.hostConnID, f.mm.byCode})
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func (f *rematchFixture) urls(t *testing.T, c *Connection) []*StationURL {
	t.Helper()
	body := NewStreamOut(f.s)
	body.U32(rematchGID)
	r := f.mm.MatchMakingHandler()(c, NewRMCRequest(f.s, ProtocolMatchMaking, MethodGetSessionURLs, 41, body.Bytes()))
	if r == nil || r.IsError {
		t.Fatal("GetSessionURLs failed")
	}
	in := NewStreamIn(r.Body, f.s)
	urls := ReadList(in, func(i *StreamIn) *StationURL { return i.StationURLValue() })
	if in.Err() != nil || !in.EOF() {
		t.Fatal("invalid URL response")
	}
	return urls
}

func TestEdenRematchHandoverRouteLifetime(t *testing.T) {
	f := newRematchFixture(t)
	requireRematchSuccess(t, f.leave(f.old), []byte{1})
	before := f.state(t)
	if urls := f.urls(t, f.next); len(urls) != 1 || urls[0].String() != f.old.Stations()[0].String() {
		t.Fatal("remaining participant lost the connected previous host")
	}
	outsider := f.connection(t, 73)
	if len(f.urls(t, outsider)) != 0 || len(f.urls(t, f.old)) != 0 {
		t.Fatal("handover route exposed to a nonparticipant")
	}
	if !bytes.Equal(before, f.state(t)) {
		t.Fatal("route lookup changed membership or ownership")
	}
	f.ep.unregisterConnection(f.old)
	if len(f.urls(t, f.next)) != 0 {
		t.Fatal("disconnected old host remains routable")
	}
	requireRematchSuccess(t, f.host(f.next, MethodUpdateSessionHost, true), nil)
	if urls := f.urls(t, f.next); len(urls) != 1 || urls[0].String() != f.next.Stations()[0].String() {
		t.Fatal("new host not routed after acquisition")
	}
}

func TestEdenRematchHostAcquisitionContinuation(t *testing.T) {
	for _, order := range []string{"host-leaves-first", "acquire-before-host-leaves", "joiner-leaves"} {
		t.Run(order, func(t *testing.T) {
			f := newRematchFixture(t)
			remaining, departing := f.next, f.old
			if order == "joiner-leaves" {
				remaining, departing = f.old, f.next
			}
			if order == "acquire-before-host-leaves" {
				requireRematchSuccess(t, f.host(remaining, MethodUpdateSessionHost, true), nil)
			}
			requireRematchSuccess(t, f.leave(departing), []byte{1})
			if order == "host-leaves-first" {
				urls := f.urls(t, remaining)
				if len(urls) != 1 || urls[0].String() != departing.Stations()[0].String() {
					t.Fatal("handover lost the still-connected recorded host")
				}
			}
			requireRematchSuccess(t, f.host(remaining, MethodUpdateSessionHost, true), nil)
			g := f.mm.gatherings[rematchGID]
			if g == nil || g.session.HostPID != remaining.PID || g.session.OwnerPID != remaining.PID || g.hostConnID != remaining.ID {
				t.Fatal("host acquisition did not update authoritative identity/connection")
			}
			if !reflect.DeepEqual(g.participants, []uint64{remaining.PID}) || g.session.NumParticipants != 1 || g.session.OpenParticipation {
				t.Fatal("wrong membership/count or unsolicited reopen")
			}
			if !bytes.Equal(g.session.SessionKey, []byte("synthetic-test-key")) || !bytes.Equal(g.session.ApplicationData, []byte{9, 8}) || f.mm.byCode["TEST1"] != rematchGID {
				t.Fatal("acquisition changed session data or room code")
			}
			before := f.state(t)
			requireRematchSuccess(t, f.leave(departing), []byte{1})
			f.mm.RemovePlayer(departing.PID) // later transport disconnect is harmless
			if !bytes.Equal(before, f.state(t)) {
				t.Fatal("duplicate leave/disconnect altered remaining session")
			}
			urls := f.urls(t, remaining)
			if len(urls) != 1 || urls[0].String() != remaining.Stations()[0].String() {
				t.Fatal("URL lookup did not use acquired host connection")
			}

			// Existing OpenParticipation and Join dispatch continue on the same
			// gathering. Join's complete synthetic struct follows NintendoClients.
			body := NewStreamOut(f.s)
			body.U32(rematchGID)
			requireRematchSuccess(t, f.mm.ExtensionHandler()(remaining, NewRMCRequest(f.s, ProtocolMatchmakeExtension, MethodOpenParticipation, 2, body.Bytes())), nil)
			newPeer := f.connection(t, 73)
			param := NewStreamOut(f.s)
			param.U32(rematchGID)
			param.U32(0)     // participants
			param.U32(0)     // participation-check gid
			param.U32(0)     // options
			param.U8(0)      // behavior
			param.String("") // user password
			param.String("") // system password
			param.String("") // join message
			param.U16(1)
			param.U16(0)
			param.U8(0) // block-list struct version, size, options
			param.U32(4)
			param.U32(0)
			body = NewStreamOut(f.s)
			body.U8(0)
			body.Buffer(param.Bytes())
			joined := f.mm.ExtensionHandler()(newPeer, NewRMCRequest(f.s, ProtocolMatchmakeExtension, MethodJoinMatchmakeSessionWithParam, 39, body.Bytes()))
			if joined == nil || joined.IsError {
				t.Fatal("new peer could not join retained gathering")
			}
			in := NewStreamIn(joined.Body, f.s)
			var session MatchmakeSession
			in.Extract(&session)
			if in.Err() != nil || !in.EOF() || session.ID != rematchGID || session.OwnerPID != remaining.PID || session.HostPID != remaining.PID || session.NumParticipants != 2 {
				t.Fatal("new peer received stale gathering state")
			}
			urls = f.urls(t, newPeer)
			if len(urls) != 1 || urls[0].String() != remaining.Stations()[0].String() {
				t.Fatal("new peer routed to departed host")
			}
		})
	}
}

func TestEdenRematchHostAndOwnerAreDistinct(t *testing.T) {
	for _, method := range []uint32{MethodUpdateSessionHost, MethodUpdateSessionHostV1} {
		for _, departed := range []bool{false, true} {
			t.Run(strconv.Itoa(int(method))+"/owner-departed="+strconv.FormatBool(departed), func(t *testing.T) {
				f := newRematchFixture(t)
				if departed {
					requireRematchSuccess(t, f.leave(f.old), []byte{1})
				}
				requireRematchSuccess(t, f.host(f.next, method, false), nil)
				g := f.mm.gatherings[rematchGID]
				if g.session.HostPID != 72 || g.hostConnID != f.next.ID || g.session.OwnerPID != 71 {
					t.Fatal("host-only acquisition implicitly migrated ownership")
				}
				if len(f.notices) != 0 {
					t.Fatal("host-only update emitted unsubstantiated ownership/host notification")
				}
				// Even if removal moved the new host to index0, it cannot write
				// owner-only application data or attributes before migrating owner.
				before := f.state(t)
				body := NewStreamOut(f.s)
				body.U32(rematchGID)
				body.Buffer([]byte{1, 2, 3})
				r := f.mm.ExtensionHandler()(f.next, NewRMCRequest(f.s, ProtocolMatchmakeExtension, MethodUpdateApplicationBuffer, 11, body.Bytes()))
				if r == nil || !r.IsError || r.Result != ResultCoreAccessDenied|ResultErrorMask {
					t.Fatal("hosting granted application-buffer ownership")
				}
				for _, attrMethod := range []uint32{MethodModifyCurrentGameAttribute, MethodUpdateMatchmakeSessionAttribute} {
					body = NewStreamOut(f.s)
					body.U32(rematchGID)
					if attrMethod == MethodModifyCurrentGameAttribute {
						body.U32(0)
					} else {
						body.U32(1)
					}
					body.U32(55)
					f.mm.ExtensionHandler()(f.next, NewRMCRequest(f.s, ProtocolMatchmakeExtension, attrMethod, attrMethod, body.Bytes()))
				}
				if !bytes.Equal(before, f.state(t)) {
					t.Fatal("host-only caller changed owner-controlled state")
				}
				requireRematchSuccess(t, f.host(f.next, MethodUpdateSessionHost, true), nil)
				body = NewStreamOut(f.s)
				body.U32(rematchGID)
				body.Buffer([]byte{1, 2, 3})
				requireRematchSuccess(t, f.mm.ExtensionHandler()(f.next, NewRMCRequest(f.s, ProtocolMatchmakeExtension, MethodUpdateApplicationBuffer, 11, body.Bytes())), nil)
				if g.session.OwnerPID != 72 || g.participants[0] != 72 || !bytes.Equal(g.session.ApplicationData, []byte{1, 2, 3}) {
					t.Fatal("explicit ownership acquisition did not allow continuation")
				}
			})
		}
	}
}

func TestEdenRematchHostRejectsInvalidRequestsAtomically(t *testing.T) {
	for _, name := range []string{"empty", "short-gid", "missing-boolean", "trailing-byte", "v1-extra-boolean", "missing-gathering", "nonmember", "departed", "unregistered", "foreign-endpoint", "zero-pid"} {
		t.Run(name, func(t *testing.T) {
			f := newRematchFixture(t)
			// Independently specified LE gid731 + true, the exact five-byte shape.
			body := []byte{0xdb, 0x02, 0, 0, 1}
			method, code, caller := MethodUpdateSessionHost, ResultCoreInvalidArgument, f.next
			switch name {
			case "empty":
				body = nil
			case "short-gid":
				body = body[:3]
			case "missing-boolean":
				body = body[:4]
			case "trailing-byte":
				body = append(body, 0)
			case "v1-extra-boolean":
				method = MethodUpdateSessionHostV1
			case "missing-gathering":
				body[0]++
				code = ResultRendezVousSessionVoid
			case "nonmember":
				caller = f.connection(t, 73)
				code = ResultCoreAccessDenied
			case "departed":
				requireRematchSuccess(t, f.leave(f.next), []byte{1})
				code = ResultCoreAccessDenied
			case "unregistered":
				f.ep.unregisterConnection(f.next)
				code = ResultCoreAccessDenied
			case "foreign-endpoint":
				caller = newTestConn(NewEndpoint(f.s), 72, "192.0.2.72:40000")
				code = ResultCoreAccessDenied
			case "zero-pid":
				caller = f.connection(t, 0)
				code = ResultCoreAccessDenied
			}
			before := f.state(t)
			f.notices = make(map[uint64][]NotificationEvent)
			r := f.mm.MatchMakingHandler()(caller, NewRMCRequest(f.s, ProtocolMatchMaking, method, 42, body))
			if r == nil || !r.IsError || r.Result != code|ResultErrorMask {
				t.Fatal("invalid host acquisition was not rejected with the expected error")
			}
			if !bytes.Equal(before, f.state(t)) || len(f.notices) != 0 {
				t.Fatal("rejected update mutated state or sent a notification")
			}
		})
	}
}

func TestEdenRematchNotificationsAndIdempotentLeave(t *testing.T) {
	f := newRematchFixture(t)
	foreign := f.connection(t, 73)
	before := f.state(t)
	requireRematchSuccess(t, f.leave(foreign), []byte{1})
	if !bytes.Equal(before, f.state(t)) || len(f.notices) != 0 {
		t.Fatal("nonmember leave changed gathering")
	}
	start := time.Now().UnixMicro()
	requireRematchSuccess(t, f.host(f.next, MethodUpdateSessionHost, true), nil)
	end := time.Now().UnixMicro()
	for _, pid := range []uint64{71, 72} {
		n := f.notices[pid]
		if len(n) != 1 || n[0].Type != 4000 || n[0].PIDSource != 71 || n[0].Param1 != 731 || n[0].Param2 != 72 {
			t.Fatal("ownership notification disagrees with documented fields/recipients")
		}
		stamp, err := strconv.ParseInt(n[0].StrParam, 10, 64)
		if err != nil || stamp < start || stamp > end {
			t.Fatal("ownership timestamp is not transition microseconds")
		}
	}
	if len(f.notices[73]) != 0 {
		t.Fatal("nonmember received ownership notification")
	}
	requireRematchSuccess(t, f.host(f.next, MethodUpdateSessionHost, true), nil)
	if len(f.notices[71]) != 1 || len(f.notices[72]) != 1 {
		t.Fatal("duplicate update repeated ownership event")
	}
	requireRematchSuccess(t, f.leave(f.old), []byte{1})
	requireRematchSuccess(t, f.leave(f.old), []byte{1})
	n := f.notices[72]
	if len(n) != 2 || n[1].Type != 3008 || n[1].PIDSource != 71 || n[1].Param1 != 731 || n[1].Param2 != 71 || n[1].StrParam != "bye" {
		t.Fatal("actual departure did not notify only the current owner once")
	}
	if len(f.notices[71]) != 1 || len(f.notices[73]) != 0 {
		t.Fatal("departure broadcast outside owner")
	}
	requireRematchSuccess(t, f.leave(f.next), []byte{1})
	requireRematchSuccess(t, f.leave(f.next), []byte{1})
	if len(f.mm.gatherings) != 0 || len(f.mm.byCode) != 0 {
		t.Fatal("last departure retained gathering/code")
	}
	if len(f.notices[72]) != 2 {
		t.Fatal("empty/missing gathering departure notified")
	}
}

func TestEdenRematchMalformedLeavePreservesState(t *testing.T) {
	for _, body := range [][]byte{nil, {0xdb, 2, 0}, {0xdb, 2, 0, 0}, {0xdb, 2, 0, 0, 4, 0, 'b'}, {0xdb, 2, 0, 0, 0, 0, 9}} {
		f := newRematchFixture(t)
		before := f.state(t)
		r := f.mm.MatchMakingExtHandler()(f.old, NewRMCRequest(f.s, ProtocolMatchMakingExt, MethodEndParticipation, 1, body))
		if r == nil || !r.IsError || r.Result != ResultCoreInvalidArgument|ResultErrorMask || !bytes.Equal(before, f.state(t)) || len(f.notices) != 0 {
			t.Fatal("malformed departure was acknowledged or mutated state")
		}
	}
}
