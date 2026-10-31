package identity

import (
	"bytes"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"

	"edennsolan/backend/internal/fixture"
)

func fixtureIDs() []fixture.Identity {
	return []fixture.Identity{{Username: "49153", PID: 49153}, {Username: "49154", PID: 49154}}
}

func TestResolveKnownIdentitiesReturnSamePID(t *testing.T) {
	r, err := New(fixtureIDs())
	if err != nil {
		t.Fatal(err)
	}
	for _, id := range fixtureIDs() {
		pid, key, rc := r.Resolve(id.Username, nil)
		if rc != 0 || pid != id.PID {
			t.Fatalf("%s: pid=%d rc=%#x", id.Username, pid, rc)
		}
		if len(key) != SourceKeyLen {
			t.Fatalf("%s: source key len %d, want %d", id.Username, len(key), SourceKeyLen)
		}
		if !bytes.Equal(key, SourceKey(id.Username)) {
			t.Fatalf("%s: source key not deterministic", id.Username)
		}
	}
	if !bytes.Equal(SourceKey("49153"), SourceKey("49153")) || bytes.Equal(SourceKey("49153"), SourceKey("49154")) {
		t.Fatal("source keys must be stable per user and distinct across users")
	}
	if r.Accepted.Load() != 2 || r.Rejected.Load() != 0 {
		t.Fatalf("counters: accepted=%d rejected=%d", r.Accepted.Load(), r.Rejected.Load())
	}
}

// Anything that is not a configured identity is refused with the auth token
// error: no hashing, no anonymous PIDs, no range heuristics, no lookups.
func TestResolveUnknownIsRejected(t *testing.T) {
	r, _ := New(fixtureIDs())
	for _, u := range []string{"", "49155", "1800000001", "1810000000", "nx2.abc.def", "49153 ", "SlotA", "0"} {
		pid, key, rc := r.Resolve(u, []byte("eyJhbGciOiJub25lIn0.eyJzdWIiOiI0OTE1MyJ9."))
		if rc != nex.ResultAuthTokenParseError || pid != 0 || key != nil {
			t.Errorf("%q: pid=%d key=%v rc=%#x; want rejection", u, pid, key, rc)
		}
	}
	if r.Rejected.Load() != 8 {
		t.Fatalf("rejected=%d", r.Rejected.Load())
	}
}

func TestNewRejectsBadTables(t *testing.T) {
	if _, err := New(nil); err == nil {
		t.Error("empty table accepted")
	}
	if _, err := New([]fixture.Identity{{Username: "a", PID: 0}}); err == nil {
		t.Error("zero PID accepted")
	}
	if _, err := New([]fixture.Identity{{Username: "a", PID: 1}, {Username: "a", PID: 2}}); err == nil {
		t.Error("duplicate username accepted")
	}
}
