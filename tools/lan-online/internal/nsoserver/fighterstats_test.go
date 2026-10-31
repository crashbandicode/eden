package nsoserver

import (
	"bytes"
	"encoding/hex"
	"path/filepath"
	"testing"

	"edennsolan/backend/internal/fixture"
)

// Real auth and secure-RMC dispatch must preserve principal ownership. Direct
// Responder tests alone would not catch the old loss of PID at that boundary.
func TestFighterStatsAuthenticatedRoundTrip(t *testing.T) {
	stateFile := filepath.Join(t.TempDir(), "ratings.json")
	cfg, err := fixture.FromEnv(func(k string) string {
		return map[string]string{"NSOLAN_ADVERTISE_HOST": "192.168.50.10", "NSOLAN_FIGHTER_STATS": "1", "NSOLAN_FIGHTER_STATE_FILE": stateFile}[k]
	})
	if err != nil {
		t.Fatal(err)
	}
	s, err := New(cfg, (&logSink{}).logf)
	if err != nil {
		t.Fatal(err)
	}
	a := connectPlayer(t, s, "49153", "192.168.50.11:40001")
	b := connectPlayer(t, s, "49154", "192.168.50.12:40002")
	report, err := hex.DecodeString("0019000000074433221188776655ccbbaa99080706050403020101000000")
	if err != nil {
		t.Fatal(err)
	}
	if got, err := a.call(0x73, 82, 101, report); err != nil || got == nil || got.IsError || len(got.Body) != 0 {
		t.Fatalf("write %v %+v", err, got)
	}
	want, err := hex.DecodeString("0100000007" + hex.EncodeToString(report) + "00140000000000000000000000000000000000000000000000")
	if err != nil {
		t.Fatal(err)
	}
	if got, err := a.call(0x73, 84, 102, nil); err != nil || got == nil || got.IsError || !bytes.Equal(got.Body, want) {
		t.Fatalf("owner read %v %+v", err, got)
	}
	if got, err := b.call(0x73, 84, 103, nil); err != nil || got == nil || got.IsError || len(got.Body) != 29 || !bytes.Equal(got.Body[:4], []byte{0, 0, 0, 0}) {
		t.Fatalf("other principal read %v %+v", err, got)
	}
	a.conn.Close()
	a = connectPlayer(t, s, "49153", "192.168.50.11:40003")
	if got, err := a.call(0x73, 84, 104, nil); err != nil || got == nil || got.IsError || !bytes.Equal(got.Body, want) {
		t.Fatalf("reconnect read %v %+v", err, got)
	}
	a.conn.Close()
	b.conn.Close()
	// Rebuild the actual server, including its authenticated dispatch boundary.
	s, err = New(cfg, (&logSink{}).logf)
	if err != nil {
		t.Fatal(err)
	}
	a = connectPlayer(t, s, "49153", "192.168.50.11:40004")
	defer a.conn.Close()
	if got, err := a.call(0x73, 84, 105, nil); err != nil || got == nil || got.IsError || !bytes.Equal(got.Body, want) {
		t.Fatalf("server restart read %v %+v", err, got)
	}
}
