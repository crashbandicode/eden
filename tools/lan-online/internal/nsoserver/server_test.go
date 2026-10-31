package nsoserver

import (
	"crypto/tls"
	"encoding/hex"
	"fmt"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	nex "github.com/NextendoNetwork/nextendo-nex"
	"github.com/gorilla/websocket"

	"edennsolan/backend/internal/fixture"
)

func testConfig(t *testing.T) *fixture.Config {
	t.Helper()
	cfg, err := fixture.FromEnv(func(k string) string {
		switch k {
		case "NSOLAN_ADVERTISE_HOST":
			return "192.168.50.10"
		case "NSOLAN_NATCHECK":
			return "0"
		}
		return ""
	})
	if err != nil {
		t.Fatal(err)
	}
	return cfg
}

type logSink struct {
	mu    sync.Mutex
	lines []string
}

func (l *logSink) logf(format string, args ...any) {
	l.mu.Lock()
	l.lines = append(l.lines, fmt.Sprintf(format, args...))
	l.mu.Unlock()
}

func (l *logSink) contains(sub string) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	for _, s := range l.lines {
		if strings.Contains(s, sub) {
			return true
		}
	}
	return false
}

func newServer(t *testing.T) (*Server, *logSink) {
	t.Helper()
	sink := &logSink{}
	s, err := New(testConfig(t), sink.logf)
	if err != nil {
		t.Fatal(err)
	}
	return s, sink
}

// loginEx performs TicketGranting LoginEx (method 2) on the auth endpoint and
// returns the PID the server minted, the decrypted client ticket and the in-band
// QResult (LoginEx reports a refusal as retval inside a success-mode reply).
func loginEx(t *testing.T, s *Server, username, addr string) (uint64, *nex.ClientTicket, uint32) {
	t.Helper()
	c := newLiteClient(s.Auth, addr)
	if err := c.handshake(nil); err != nil {
		t.Fatal(err)
	}
	body := nex.NewStreamOut(s.AuthSettings)
	body.String(username)
	resp, err := c.call(nex.ProtocolTicketGranting, nex.MethodLoginEx, 1, body.Bytes())
	if err != nil {
		t.Fatal(err)
	}
	if resp.IsError {
		t.Fatalf("LoginEx must answer in-band, got RMC error %#x", resp.Result)
	}
	in := nex.NewStreamIn(resp.Body, s.AuthSettings)
	retval := in.U32()
	pid := in.PID()
	if retval&nex.ResultErrorMask != 0 {
		if pid != 0 {
			t.Fatalf("refused login must carry pid 0, got %d", pid)
		}
		return 0, nil, retval
	}
	ticket := in.Buffer()
	var rv nex.RVConnectionData
	in.Extract(&rv)
	_ = in.String() // return message
	srcKeyHex := in.String()
	if in.Err() != nil || retval != nex.ResultCoreUnknown {
		t.Fatalf("login reply parse: err=%v retval=%#x", in.Err(), retval)
	}
	if rv.MainStation == nil || rv.MainStation.Scheme != "prudps" || rv.MainStation.Get("address") != "192.168.50.10" || rv.MainStation.GetInt("port") != 60005 {
		t.Fatalf("advertised secure station: %v", rv.MainStation)
	}
	key, err := hex.DecodeString(srcKeyHex)
	if err != nil || len(key) != 32 {
		t.Fatalf("source key: %v len=%d", err, len(key))
	}
	ct, err := nex.DecryptClientTicket(ticket, key, s.AuthSettings)
	if err != nil {
		t.Fatalf("decrypt client ticket: %v", err)
	}
	return pid, ct, retval
}

// secureConnect presents the ticket on the secure endpoint exactly as a console
// does: CONNECT payload = Buffer(internal ticket) + Buffer(seal(sessionKey, PID|CID|check)).
func secureConnect(t *testing.T, s *Server, pid uint64, ct *nex.ClientTicket, addr string) *liteClient {
	t.Helper()
	req := nex.NewStreamOut(s.SecureSettings)
	req.PID(pid)
	req.U32(0x1234)   // CID
	req.U32(0x00C0FFE) // check value
	payload := nex.NewStreamOut(s.SecureSettings)
	payload.Buffer(ct.Internal)
	payload.Buffer(kerberosSeal(ct.SessionKey, req.Bytes()))
	c := newLiteClient(s.Secure, addr)
	if err := c.handshake(payload.Bytes()); err != nil {
		t.Fatal(err)
	}
	if c.conn.PID != pid || c.conn.ID == 0 {
		t.Fatalf("secure identity: pid=%d rvcid=%d", c.conn.PID, c.conn.ID)
	}
	return c
}

func autoMatchmakeBody(s *nex.Settings, open bool) []byte {
	p := &nex.AutoMatchmakeParam{Session: nex.MatchmakeSession{
		Gathering:         nex.Gathering{MinParticipants: 2, MaxParticipants: 2},
		GameMode:          1,
		OpenParticipation: open,
		Attribs:           []uint32{0, 0, 0, 0, 0, 0},
	}}
	out := nex.NewStreamOut(s)
	out.Add(p)
	return out.Bytes()
}

func sessionOf(t *testing.T, s *Server, resp *nex.RMCMessage) nex.MatchmakeSession {
	t.Helper()
	if resp.IsError {
		t.Fatalf("matchmake error %#x", resp.Result)
	}
	var ms nex.MatchmakeSession
	in := nex.NewStreamIn(resp.Body, s.SecureSettings)
	in.Extract(&ms)
	if in.Err() != nil {
		t.Fatalf("session parse: %v", in.Err())
	}
	return ms
}

func participateEvents(ms []*nex.RMCMessage, s *nex.Settings) []nex.NotificationEvent {
	var out []nex.NotificationEvent
	for _, m := range ms {
		if m.Protocol != nex.ProtocolNotifications || m.Method != nex.MethodProcessNotificationEvent {
			continue
		}
		var ev nex.NotificationEvent
		nex.NewStreamIn(m.Body, s).Extract(&ev)
		out = append(out, ev)
	}
	return out
}

func register(t *testing.T, c *liteClient, s *Server, lanIP string, udpPort int) {
	t.Helper()
	st := nex.NewStationURL("prudp")
	st.Set("address", lanIP)
	st.SetInt("port", udpPort)
	st.SetInt("sid", 15)
	st.SetInt("stream", 3)
	st.SetInt("type", 0)
	body := nex.NewStreamOut(s.SecureSettings)
	nex.WriteList(body, []*nex.StationURL{st}, func(o *nex.StreamOut, u *nex.StationURL) { o.StationURL(u) })
	resp, err := c.call(nex.ProtocolSecureConnection, nex.MethodRegister, 10, body.Bytes())
	if err != nil || resp.IsError {
		t.Fatalf("register: %v %+v", err, resp)
	}
}

// --- tests ---

// The identity path end to end: both fixture usernames log in through the real
// auth handler and get back their own number as PID; an unknown username is
// refused; the ticket each one gets is accepted by the secure endpoint and
// yields the same PID there.
func TestLoginAndSecureConnectForBothIdentities(t *testing.T) {
	s, _ := newServer(t)
	for i, id := range s.Cfg.SortedIdentities() {
		addr := fmt.Sprintf("192.168.50.1%d:4000%d", i+1, i)
		pid, ct, _ := loginEx(t, s, id.Username, addr)
		if pid != id.PID {
			t.Fatalf("%s: login pid %d want %d", id.Username, pid, id.PID)
		}
		if ct.Target != s.Cfg.SecurePID || len(ct.SessionKey) != 32 {
			t.Fatalf("%s: ticket target=%d keyLen=%d", id.Username, ct.Target, len(ct.SessionKey))
		}
		sc := secureConnect(t, s, pid, ct, addr)
		if s.Secure.FindConnectionByPID(id.PID) != sc.conn {
			t.Fatalf("%s: secure connection not registered under pid", id.Username)
		}
	}
	pid, ct, retval := loginEx(t, s, "49155", "192.168.50.13:40002")
	if retval != (nex.ResultAuthTokenParseError|nex.ResultErrorMask) || pid != 0 || ct != nil {
		t.Fatalf("unknown username must be refused in-band: retval=%#x pid=%d", retval, pid)
	}
	if s.Resolver.Accepted.Load() != 2 || s.Resolver.Rejected.Load() != 1 {
		t.Fatalf("resolver counters %d/%d", s.Resolver.Accepted.Load(), s.Resolver.Rejected.Load())
	}
}

// SSBU logs in through TicketGranting method 6 (ValidateAndRequestTicketWithParam).
func TestLoginWithContextMethod6(t *testing.T) {
	s, _ := newServer(t)
	c := newLiteClient(s.Auth, "192.168.50.11:40100")
	if err := c.handshake(nil); err != nil {
		t.Fatal(err)
	}
	fields := nex.NewStreamOut(s.AuthSettings)
	fields.U32(2) // platform type
	fields.String("49154")
	body := nex.NewStreamOut(s.AuthSettings)
	body.U8(1)
	body.U32(uint32(len(fields.Bytes())))
	body.Write(fields.Bytes())
	resp, err := c.call(nex.ProtocolTicketGranting, nex.MethodLoginWithContext, 3, body.Bytes())
	if err != nil || resp.IsError {
		t.Fatalf("method 6: %v %+v", err, resp)
	}
	in := nex.NewStreamIn(resp.Body, s.AuthSettings)
	_ = in.U8()
	_ = in.U32()
	if pid := in.PID(); pid != 49154 {
		t.Fatalf("method 6 pid %d", pid)
	}
	ticket := in.Buffer()
	station := in.StationURLValue()
	_ = in.DateTime()
	_ = in.String()
	key, _ := hex.DecodeString(in.String())
	if in.Err() != nil || station.Scheme != "prudps" || station.GetInt("port") != 60005 {
		t.Fatalf("method 6 reply: err=%v station=%v", in.Err(), station)
	}
	if _, err := nex.DecryptClientTicket(ticket, key, s.AuthSettings); err != nil {
		t.Fatalf("method 6 ticket: %v", err)
	}
}

// A ticket-less secure CONNECT from an address that never authenticated is refused.
func TestTicketlessSecureConnectRefused(t *testing.T) {
	s, _ := newServer(t)
	c := newLiteClient(s.Secure, "192.168.50.99:5000")
	err := c.handshake(nil)
	if err == nil || !strings.Contains(err.Error(), "timeout") {
		t.Fatalf("ticketless connect should get no CONNECT-ACK, got %v", err)
	}
	if c.conn.ID != 0 {
		t.Fatal("ticketless connection must not be registered")
	}
}

// Quickplay pairing through the real dispatch path: the first player opens a
// gathering and waits (one participant, self Participate event); the second
// player lands in the SAME gathering (two participants) even with the client
// flag OpenParticipation clear; both receive the Participate notifications Pia
// waits on; the joiner can fetch the host's LAN station endpoint.
func TestFirstWaitsSecondJoinsSameGathering(t *testing.T) {
	s, sink := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
	b := connectPlayer(t, s, "49154", "192.168.50.12:40002")
	register(t, a, s, "192.168.50.11", 49821)
	register(t, b, s, "192.168.50.12", 49822)

	respA, err := a.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 20, autoMatchmakeBody(s.SecureSettings, false))
	if err != nil {
		t.Fatal(err)
	}
	sessA := sessionOf(t, s, respA)
	if sessA.ID == 0 || sessA.OwnerPID != 49153 || sessA.HostPID != 49153 || sessA.NumParticipants != 1 || !sessA.OpenParticipation {
		t.Fatalf("A must wait as host of an open gathering: %+v", sessA)
	}
	if parts := s.MM.SessionByPID(49153); len(parts) != 1 || parts[0] != 49153 {
		t.Fatalf("A participants: %v", parts)
	}
	evA := participateEvents(a.takeNotifications(), s.SecureSettings)
	if len(evA) != 1 || evA[0].Type != nex.NotificationParticipate || evA[0].Param2 != 49153 || evA[0].Param1 != uint64(sessA.ID) {
		t.Fatalf("A self Participate event: %+v", evA)
	}
	if got := len(participateEvents(b.takeNotifications(), s.SecureSettings)); got != 0 {
		t.Fatalf("B must not be notified before joining: %d", got)
	}

	respB, err := b.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 21, autoMatchmakeBody(s.SecureSettings, false))
	if err != nil {
		t.Fatal(err)
	}
	sessB := sessionOf(t, s, respB)
	if sessB.ID != sessA.ID || sessB.OwnerPID != 49153 || sessB.HostPID != 49153 || sessB.NumParticipants != 2 {
		t.Fatalf("B must join A's gathering: A=%+v B=%+v", sessA, sessB)
	}
	if string(sessB.SessionKey) != string(sessA.SessionKey) || len(sessA.SessionKey) != 32 {
		t.Fatal("both players must share the gathering session key")
	}
	if parts := s.MM.SessionByPID(49154); len(parts) != 2 || parts[0] != 49153 || parts[1] != 49154 {
		t.Fatalf("participants after join: %v", parts)
	}
	evA = participateEvents(a.takeNotifications(), s.SecureSettings)
	if len(evA) != 1 || evA[0].Param2 != 49154 || evA[0].Param3 != 2 || evA[0].PIDSource != 49154 {
		t.Fatalf("host must learn of the joiner: %+v", evA)
	}
	evB := participateEvents(b.takeNotifications(), s.SecureSettings)
	if len(evB) != 2 || evB[0].Param2 != 49154 || evB[1].Param2 != 49153 {
		t.Fatalf("joiner must get its own event and the recap of the host: %+v", evB)
	}

	// Joiner fetches the host's station URLs: LAN two-candidate shape pointing at A.
	gidBody := nex.NewStreamOut(s.SecureSettings)
	gidBody.U32(sessA.ID)
	urlResp, err := b.call(nex.ProtocolMatchMaking, nex.MethodGetSessionURLs, 22, gidBody.Bytes())
	if err != nil || urlResp.IsError {
		t.Fatalf("GetSessionURLs: %v %+v", err, urlResp)
	}
	urls := nex.ReadList(nex.NewStreamIn(urlResp.Body, s.SecureSettings), func(i *nex.StreamIn) *nex.StationURL { return i.StationURLValue() })
	if len(urls) != 2 || urls[0].Get("address") != "192.168.50.11" || urls[0].GetInt("port") != 49821 ||
		urls[0].GetInt("RVCID") != int(a.conn.ID) || urls[0].Has("type") || urls[1].GetInt("type") != 0x0B {
		t.Fatalf("session urls: %v", urls)
	}
	if !sink.contains("[lanstations] mode=lan") {
		t.Fatal("station builder did not log")
	}
}

// Cancellation paths clean the gathering so a later search starts fresh:
// host UnregisterGathering while alone, lone host EndParticipation, and a
// dropped connection. A non-owner cannot unregister someone else's gathering.
func TestCancelWhileWaitingCleansUp(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
	b := connectPlayer(t, s, "49154", "192.168.50.12:40002")

	open := func(c *liteClient, call uint32) nex.MatchmakeSession {
		resp, err := c.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, call, autoMatchmakeBody(s.SecureSettings, false))
		if err != nil {
			t.Fatal(err)
		}
		return sessionOf(t, s, resp)
	}
	u32 := func(v uint32) []byte { o := nex.NewStreamOut(s.SecureSettings); o.U32(v); return o.Bytes() }

	// 1. Host cancels with UnregisterGathering.
	g1 := open(a, 30)
	if _, err := a.call(nex.ProtocolMatchMaking, nex.MethodUnregisterGathering, 31, u32(g1.ID)); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.MM.GatheringIDByPID(49153); ok {
		t.Fatal("unregister by owner must remove the gathering")
	}
	// B now searches: must get a NEW gathering where it waits as host.
	g2 := open(b, 32)
	if g2.ID == g1.ID || g2.OwnerPID != 49154 || g2.NumParticipants != 1 {
		t.Fatalf("after cancel the next search must open fresh: %+v", g2)
	}
	// A non-owner cannot unregister B's gathering.
	if _, err := a.call(nex.ProtocolMatchMaking, nex.MethodUnregisterGathering, 33, u32(g2.ID)); err != nil {
		t.Fatal(err)
	}
	if gid, ok := s.MM.GatheringIDByPID(49154); !ok || gid != g2.ID {
		t.Fatal("non-owner unregister must not remove the gathering")
	}

	// 2. Lone host leaves with EndParticipation.
	leave := nex.NewStreamOut(s.SecureSettings)
	leave.U32(g2.ID)
	leave.String("")
	if resp, err := b.call(nex.ProtocolMatchMakingExt, nex.MethodEndParticipation, 34, leave.Bytes()); err != nil {
		t.Fatal(err)
	} else if resp.IsError {
		t.Fatalf("end participation rejected: %#x", resp.Result)
	}
	if _, ok := s.MM.GatheringIDByPID(49154); ok {
		t.Fatal("end participation by the lone host must remove the gathering")
	}

	// 3. Host drops the connection while waiting; disconnect hook must clean up.
	g3 := open(a, 35)
	a.conn.Close()
	if _, ok := s.MM.GatheringIDByPID(49153); ok {
		t.Fatal("disconnect must remove the waiting host's gathering")
	}
	if s.Secure.FindConnectionByPID(49153) != nil {
		t.Fatal("closed connection must be unregistered")
	}
	// A fresh connection for A searches again and pairs with B's new gathering.
	g4 := open(b, 36)
	if g4.ID == g3.ID {
		t.Fatal("stale gathering id must not be reused for the new host")
	}
	a2 := connectPlayer(t, s, "49153", "192.168.50.11:40003")
	g5 := open(a2, 37)
	if g5.ID != g4.ID || g5.NumParticipants != 2 {
		t.Fatalf("reconnected player must join the waiting one: %+v vs %+v", g4, g5)
	}
}

// Two different game modes never merge (compatibility guard the core applies).
func TestIncompatibleModesDoNotPair(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
	b := connectPlayer(t, s, "49154", "192.168.50.12:40002")
	mk := func(mode uint32) []byte {
		p := &nex.AutoMatchmakeParam{Session: nex.MatchmakeSession{Gathering: nex.Gathering{MaxParticipants: 2}, GameMode: mode, Attribs: []uint32{0, 0, 0, 0, 0, 0}}}
		o := nex.NewStreamOut(s.SecureSettings)
		o.Add(p)
		return o.Bytes()
	}
	ra, _ := a.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 40, mk(1))
	rb, _ := b.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 41, mk(2))
	if sessionOf(t, s, ra).ID == sessionOf(t, s, rb).ID {
		t.Fatal("different game modes must not share a gathering")
	}
}

// Init getters and the Pia type-8 keepalive are served on the secure endpoint.
func TestInitShapeAndKeepaliveOnSecureEndpoint(t *testing.T) {
	s, _ := newServer(t)
	a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
	resp, err := a.call(0x6E, nex.MethodGetIntegerSettings, 50, []byte{0, 0, 0, 0})
	if err != nil || resp.IsError || len(resp.Body) != 4+431*6 {
		t.Fatalf("GetIntegerSettings over the wire: %v %+v", err, resp)
	}
	resp, err = a.call(0x73, 60, 51, nil)
	if err != nil || resp.IsError || len(resp.Body) != 4 {
		t.Fatalf("DataStore 60 fallback over the wire: %v %+v", err, resp)
	}
	if s.Init.Calls(0x73, 60) != 1 {
		t.Fatalf("init counter %d", s.Init.Calls(0x73, 60))
	}
	// Type-8 keepalive with NeedAck must be acknowledged with a type-8 ACK.
	before := len(a.frames)
	a.conn.Feed(nex.EncodePacket(&nex.Packet{Type: 8, Flags: nex.FlagNeedACK, SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort, PacketID: 77}))
	deadline := time.Now().Add(time.Second)
	for len(a.frames) == before && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	a.mu.Lock()
	last := a.frames[len(a.frames)-1]
	a.mu.Unlock()
	pks, _, _ := nex.DecodePackets(last)
	if len(pks) != 1 || pks[0].Type != 8 || !pks[0].HasFlag(nex.FlagACK) || pks[0].PacketID != 77 {
		t.Fatalf("type-8 keepalive not acked: %+v", pks)
	}
}

// The real WebSocket transport over TLS accepts the console's "NEX" subprotocol
// offer and answers a SYN with a SYN+ACK through our handler/listener wrapper.
func TestWSSTransportSmoke(t *testing.T) {
	s, _ := newServer(t)
	ts := httptest.NewTLSServer(s.AuthHandler())
	defer ts.Close()
	dialer := websocket.Dialer{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, Subprotocols: []string{"NEX"}}
	ws, _, err := dialer.Dial("wss"+strings.TrimPrefix(ts.URL, "https"), nil)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	syn := &nex.Packet{Type: nex.PacketSYN, Flags: nex.FlagNeedACK, SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort, MinorVersion: 5}
	if err := ws.WriteMessage(websocket.BinaryMessage, nex.EncodePacket(syn)); err != nil {
		t.Fatal(err)
	}
	_ = ws.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, data, err := ws.ReadMessage()
	if err != nil {
		t.Fatal(err)
	}
	pks, _, err := nex.DecodePackets(data)
	if err != nil || len(pks) != 1 || pks[0].Type != nex.PacketSYN || !pks[0].HasFlag(nex.FlagACK) || len(pks[0].ConnectionSig) != 16 {
		t.Fatalf("SYN+ACK over WSS: err=%v pks=%+v", err, pks)
	}
	// A plain HTTPS GET is not a NEX client and gets 404, nothing else.
	resp, err := ts.Client().Get(ts.URL + "/")
	if err != nil || resp.StatusCode != 404 {
		t.Fatalf("plain GET: %v %v", err, resp)
	}
}

// connectPlayer logs the identity in on the auth endpoint and connects it on
// the secure endpoint with its ticket.
func connectPlayer(t *testing.T, s *Server, username, addr string) *liteClient {
	t.Helper()
	pid, ct, _ := loginEx(t, s, username, addr)
	return secureConnect(t, s, pid, ct, addr)
}
