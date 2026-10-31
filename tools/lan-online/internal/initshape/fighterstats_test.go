package initshape

import (
	"bytes"
	"encoding/hex"
	"os"
	"path/filepath"
	"sync"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// Hand-authored synthetic wire oracle. v0, length25, fighter7, three distinct
// u32s, a distinct packed u64 and final u32. No production serializer generates
// these inputs or expectations; no captured Nintendo data is used.
func wire(t *testing.T, value string) []byte {
	t.Helper()
	b, err := hex.DecodeString(value)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

const report7 = "0019000000074433221188776655ccbbaa99080706050403020101000000"
const summaryZero = "00140000000000000000000000000000000000000000000000"

func fighterCall(r *Responder, pid uint64, method uint32, b []byte) *nex.RMCMessage {
	return r.Handler(ProtocolDataStore)(&nex.Connection{Settings: settings(), PID: pid}, req(ProtocolDataStore, method, b))
}

func TestFighterReportsArePrincipalScopedAndReplaceable(t *testing.T) {
	r := New(Options{FighterStats: true})
	a := wire(t, report7)
	b := wire(t, "00190000000304030201080706050c0b0a09181716151413121102000000")
	for _, step := range []struct {
		pid  uint64
		body []byte
	}{{49153, a}, {49153, b}, {49154, b}, {49153, a}} {
		if got := fighterCall(r, step.pid, 82, step.body); got.IsError || len(got.Body) != 0 {
			t.Fatalf("write: %+v", got)
		}
	}
	for _, tc := range []struct {
		pid  uint64
		want string
	}{
		{49153, "0200000003" + hex.EncodeToString(b) + "07" + report7 + summaryZero},
		{49154, "0100000003" + hex.EncodeToString(b) + summaryZero},
		{49155, "00000000" + summaryZero},
	} {
		got := fighterCall(r, tc.pid, 84, nil)
		if got.IsError || !bytes.Equal(got.Body, wire(t, tc.want)) {
			t.Fatalf("read pid%d: %x", tc.pid, got.Body)
		}
	}
	// The service owns a copy, and an outgoing response also cannot mutate it.
	a[6] = 0
	read := fighterCall(r, 49153, 84, nil)
	if !bytes.Equal(read.Body, wire(t, "0200000003"+hex.EncodeToString(b)+"07"+report7+summaryZero)) {
		t.Fatal("request aliases stored state")
	}
	read.Body[4] = 98
	if got := fighterCall(r, 49153, 84, nil); got.Body[4] != 3 {
		t.Fatal("response aliases stored state")
	}
	// Replace one field for the same key, then read through a fresh connection.
	updated := wire(t, "0019000000070403020188776655ccbbaa99080706050403020101000000")
	if got := fighterCall(r, 49153, 82, updated); got.IsError {
		t.Fatal("replace failed")
	}
	want := wire(t, "0200000003"+hex.EncodeToString(b)+"07"+hex.EncodeToString(updated)+summaryZero)
	if got := fighterCall(r, 49153, 84, nil); !bytes.Equal(got.Body, want) {
		t.Fatal("replacement not returned")
	}
}

func TestFighterReportsRejectMalformedWithoutMutation(t *testing.T) {
	r := New(Options{FighterStats: true})
	valid := wire(t, report7)
	fighterCall(r, 49153, 82, valid)
	before := fighterCall(r, 49153, 84, nil).Body
	inputs := [][]byte{nil, append(append([]byte{}, valid...), 0)}
	for n := 0; n < len(valid); n++ {
		inputs = append(inputs, append([]byte{}, valid[:n]...))
	}
	for _, change := range []struct {
		off   int
		value byte
	}{{0, 1}, {1, 24}, {2, 1}, {5, 99}, {5, 254}} {
		b := append([]byte{}, valid...)
		b[change.off] = change.value
		inputs = append(inputs, b)
	}
	for i, b := range inputs {
		if got := fighterCall(r, 49153, 82, b); !got.IsError || got.Result != nex.ResultCoreInvalidArgument|nex.ResultErrorMask {
			t.Fatalf("invalid #%d accepted", i)
		}
		if got := fighterCall(r, 49153, 84, nil); !bytes.Equal(before, got.Body) {
			t.Fatalf("invalid #%d mutated state", i)
		}
	}
	for _, method := range []uint32{82, 84} {
		body := valid
		if method == 84 {
			body = nil
		}
		if got := fighterCall(r, 0, method, body); !got.IsError {
			t.Fatal("anonymous request accepted")
		}
	}
	if got := fighterCall(r, 49153, 84, []byte{0}); !got.IsError {
		t.Fatal("nonempty read accepted")
	}
}

func TestUnmappedFighterSentinelIsPreserved(t *testing.T) {
	r := New(Options{FighterStats: true})
	// The game's own builder uses ff when its fighter-index lookup fails.
	report := wire(t, "0019000000ff4433221188776655ccbbaa99080706050403020101000000")
	if got := fighterCall(r, 49153, 82, report); got.IsError {
		t.Fatal("valid sentinel report rejected")
	}
	want := wire(t, "01000000ff"+hex.EncodeToString(report)+summaryZero)
	if got := fighterCall(r, 49153, 84, nil); got.IsError || !bytes.Equal(got.Body, want) {
		t.Fatal("sentinel not preserved")
	}
}

func TestFighterStateOverridesGlobalReplayAndIsBounded(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "0x73_84.bin"), []byte{0xff}, 0600); err != nil {
		t.Fatal(err)
	}
	r := New(Options{FighterStats: true, ReplayDir: dir})
	for pid := uint64(1); pid <= maxFighterPlayers; pid++ {
		if got := fighterCall(r, pid, 82, wire(t, report7)); got.IsError {
			t.Fatalf("pid%d rejected", pid)
		}
	}
	if got := fighterCall(r, maxFighterPlayers+1, 82, wire(t, report7)); !got.IsError {
		t.Fatal("unbounded player allocation")
	}
	if got := fighterCall(r, 1, 84, nil); !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
		t.Fatal("global reply shadowed personal state")
	}
	if got := fighterCall(r, 1, 82, wire(t, report7)); got.IsError {
		t.Fatal("existing player blocked at capacity")
	}
}

func TestConcurrentFighterPlayers(t *testing.T) {
	r := New(Options{FighterStats: true})
	b := wire(t, report7)
	var wg sync.WaitGroup
	for player := 0; player < 8; player++ {
		wg.Add(1)
		go func(pid uint64) {
			defer wg.Done()
			for n := 0; n < 50; n++ {
				if fighterCall(r, pid, 82, b).IsError {
					t.Error("write failed")
				}
				if got := fighterCall(r, pid, 84, nil); got.IsError || len(got.Body) != 60 {
					t.Errorf("read length=%d", len(got.Body))
				}
			}
		}(uint64(player + 1))
	}
	wg.Wait()
}

func TestMetadataUpdateAcknowledgementIsVoid(t *testing.T) {
	r := New(Options{})
	got, kind := r.Answer(settings(), ProtocolDataStore, req(ProtocolDataStore, 38, nil))
	if got.IsError || len(got.Body) != 0 || kind != KindEmptyBody {
		t.Fatal("method38 reply parser expects no body")
	}
}
