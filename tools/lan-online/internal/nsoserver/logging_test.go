package nsoserver

import (
	"bytes"
	"encoding/hex"
	"io"
	"log"
	"os"
	"strings"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"

	"edennsolan/backend/internal/identity"
	"edennsolan/backend/internal/quiet"
)

// capture swaps *target (os.Stdout or os.Stderr style *os.File variable) for a
// pipe and returns a function that restores it and yields everything written.
func capture(t *testing.T, target **os.File) func() string {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	prev := *target
	*target = w
	done := make(chan string, 1)
	go func() {
		var buf bytes.Buffer
		_, _ = io.Copy(&buf, r)
		done <- buf.String()
	}()
	return func() string {
		*target = prev
		_ = w.Close()
		out := <-done
		_ = r.Close()
		return out
	}
}

// Both Eden slots run on one host, so both consoles reach the backend from the
// same source IP (192.168.0.137) with different source ports. The Kerberos ticket
// path must keep the two identities apart end to end; the core's IP-keyed
// fallback must never be what the fixture relies on.
func TestSameSourceIPTwoIdentitiesPairViaTickets(t *testing.T) {
	s, _ := newServer(t)
	const sharedIP = "192.168.0.137"
	a := connectPlayer(t, s, "49153", sharedIP+":40001")
	b := connectPlayer(t, s, "49154", sharedIP+":40002")
	if a.conn.PID != 49153 || b.conn.PID != 49154 || a.conn.ID == b.conn.ID {
		t.Fatalf("identities from one IP: a=%d/%d b=%d/%d", a.conn.PID, a.conn.ID, b.conn.PID, b.conn.ID)
	}
	if s.Secure.FindConnectionByPID(49153) != a.conn || s.Secure.FindConnectionByPID(49154) != b.conn {
		t.Fatal("PID lookup must resolve each identity to its own connection")
	}
	register(t, a, s, sharedIP, 49821)
	register(t, b, s, sharedIP, 49822)

	ra, err := a.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 60, autoMatchmakeBody(s.SecureSettings, false))
	if err != nil {
		t.Fatal(err)
	}
	rb, err := b.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 61, autoMatchmakeBody(s.SecureSettings, false))
	if err != nil {
		t.Fatal(err)
	}
	sa, sb := sessionOf(t, s, ra), sessionOf(t, s, rb)
	if sa.ID != sb.ID || sa.OwnerPID != 49153 || sb.NumParticipants != 2 {
		t.Fatalf("same-IP pairing: a=%+v b=%+v", sa, sb)
	}
	evA := participateEvents(a.takeNotifications(), s.SecureSettings)
	if len(evA) != 2 || evA[1].Param2 != 49154 {
		t.Fatalf("host notifications from a shared IP: %+v", evA)
	}
	gid := nex.NewStreamOut(s.SecureSettings)
	gid.U32(sa.ID)
	urlResp, err := b.call(nex.ProtocolMatchMaking, nex.MethodGetSessionURLs, 62, gid.Bytes())
	if err != nil || urlResp.IsError {
		t.Fatalf("GetSessionURLs: %v %+v", err, urlResp)
	}
	urls := nex.ReadList(nex.NewStreamIn(urlResp.Body, s.SecureSettings), func(i *nex.StreamIn) *nex.StationURL { return i.StationURLValue() })
	if len(urls) != 2 || urls[0].Get("address") != sharedIP || urls[0].GetInt("port") != 49821 || urls[0].GetInt("RVCID") != int(a.conn.ID) {
		t.Fatalf("joiner must get the HOST's UDP endpoint, not its own: %v", urls)
	}

	// Documented hazard (why tickets are mandatory): a ticket-less CONNECT from the
	// shared IP inherits whichever identity authenticated last from that IP.
	ghost := newLiteClient(s.Secure, sharedIP+":40003")
	if err := ghost.handshake(nil); err != nil {
		t.Fatalf("ticketless connect from an IP that authenticated is accepted by the core: %v", err)
	}
	if ghost.conn.PID != 49154 {
		t.Fatalf("ticketless CONNECT inherited pid %d (core IP recall); expected the last login 49154", ghost.conn.PID)
	}
	// It did not disturb the ticketed sessions' identities.
	if a.conn.PID != 49153 || b.conn.PID != 49154 {
		t.Fatal("ticketed sessions must keep their PIDs")
	}
}

// Proof of the logging contract:
//  1. the core really writes the per-login source key to standard output (so the
//     suppression is load-bearing, not theoretical);
//  2. after quiet.SuppressStdout nothing reaches the previous standard output;
//  3. the fixture's own log (log.Printf, standard error) never contains the
//     source key, session key or ticket material, only metadata.
func TestCoreStdoutSuppressedAndOwnLogHasNoCredentials(t *testing.T) {
	keyA := hex.EncodeToString(identity.SourceKey("49153"))
	keyB := hex.EncodeToString(identity.SourceKey("49154"))

	// 1. Unsuppressed: the leak is observable.
	stopStdout := capture(t, &os.Stdout)
	s, _ := newServer(t)
	_, ctA, _ := loginEx(t, s, "49153", "192.168.0.137:40001")
	leaked := stopStdout()
	if !strings.Contains(leaked, keyA) || !strings.Contains(leaked, "srcKey=") {
		t.Fatalf("expected the core to print the source key on stdout; got %d bytes", len(leaked))
	}

	// 2. Suppressed: the previous stdout receives nothing while the core logs in a player.
	stopStdout = capture(t, &os.Stdout)
	restore, err := quiet.SuppressStdout(false)
	if err != nil {
		t.Fatal(err)
	}
	// 3. Own log on stderr (production wiring: log.Printf) during a full login +
	//    secure connect + matchmake.
	stderrR, stderrW, _ := os.Pipe()
	log.SetOutput(stderrW)
	s2, err := New(testConfig(t), log.Printf)
	if err != nil {
		t.Fatal(err)
	}
	pidB, ctB, _ := loginEx(t, s2, "49154", "192.168.0.137:40002")
	cb := secureConnect(t, s2, pidB, ctB, "192.168.0.137:40002")
	if _, err := cb.call(nex.ProtocolMatchmakeExtension, nex.MethodAutoMatchmakeWithParamPostpone, 70, autoMatchmakeBody(s2.SecureSettings, false)); err != nil {
		t.Fatal(err)
	}
	log.SetOutput(os.Stderr)
	_ = stderrW.Close()
	ownLog, _ := io.ReadAll(stderrR)
	_ = stderrR.Close()
	restore()
	quietOut := stopStdout()
	if len(quietOut) != 0 {
		t.Fatalf("suppressed stdout still received %d bytes", len(quietOut))
	}

	own := string(ownLog)
	for name, secret := range map[string]string{
		"source key B":   keyB,
		"source key A":   keyA,
		"session key B":  hex.EncodeToString(ctB.SessionKey),
		"session key A":  hex.EncodeToString(ctA.SessionKey),
		"ticket blob B":  hex.EncodeToString(ctB.Internal),
		"srcKey marker":  "srcKey=",
		"resp hex dump":  "resp=",
		"ticketLen line": "ticketLen=",
	} {
		if strings.Contains(own, secret) {
			t.Errorf("own log contains %s", name)
		}
	}
	for _, want := range []string{"[identity] accept username=\"49154\" -> pid=49154", "[secure] connected pid=49154", "proto=0x6d method=40"} {
		if !strings.Contains(own, want) {
			t.Errorf("own log missing metadata line %q", want)
		}
	}
}
