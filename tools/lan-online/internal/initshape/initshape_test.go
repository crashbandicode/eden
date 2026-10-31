package initshape

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"sync"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

func settings() *nex.Settings { return nex.NewSwitchSettings("9587602b", 40000) }

func req(proto uint16, method uint32, body []byte) *nex.RMCMessage {
	return nex.NewRMCRequest(settings(), proto, method, 7, body)
}

// Independent oracle for GetIntegerSettings: the documented Map<Uint16, Sint32>
// layout is u32 count + count*(2+4) bytes. With the upstream-reported count of
// 431 that is 4 + 431*6 = 2590 bytes (the arithmetic upstream used to validate
// its own capture), reached here without any captured bytes.
func TestGetIntegerSettingsShape(t *testing.T) {
	r := New(Options{UtilityIntSettings: 431, UtilityIntValues: map[int]int32{21: 60, 430: -1}})
	resp, kind := r.Answer(settings(), ProtocolUtility, req(ProtocolUtility, nex.MethodGetIntegerSettings, []byte{0, 0, 0, 0}))
	if kind != KindDocumented || resp.IsError {
		t.Fatalf("kind=%s err=%v", kind, resp.IsError)
	}
	if len(resp.Body) != 4+431*6 {
		t.Fatalf("body len %d, want %d", len(resp.Body), 4+431*6)
	}
	if binary.LittleEndian.Uint32(resp.Body[:4]) != 431 {
		t.Fatalf("count %d", binary.LittleEndian.Uint32(resp.Body[:4]))
	}
	for k := 0; k < 431; k++ {
		off := 4 + k*6
		if int(binary.LittleEndian.Uint16(resp.Body[off:])) != k {
			t.Fatalf("key at position %d is %d", k, binary.LittleEndian.Uint16(resp.Body[off:]))
		}
		v := int32(binary.LittleEndian.Uint32(resp.Body[off+2:]))
		want := int32(0)
		switch k {
		case 21:
			want = 60
		case 430:
			want = -1
		}
		if v != want {
			t.Fatalf("value at %d = %d, want %d", k, v, want)
		}
	}
	// Zero entries is a valid empty map (four zero bytes).
	resp, _ = New(Options{}).Answer(settings(), ProtocolUtility, req(ProtocolUtility, nex.MethodGetIntegerSettings, nil))
	if !bytes.Equal(resp.Body, []byte{0, 0, 0, 0}) {
		t.Fatalf("empty map body % x", resp.Body)
	}
}

func TestDataStoreDocumentedAndFallbackShapes(t *testing.T) {
	r := New(Options{})
	s := settings()

	// 21 PostMetaBinary -> Uint64 data id, monotonically increasing from 1.
	for want := uint64(1); want <= 3; want++ {
		resp, kind := r.Answer(s, ProtocolDataStore, req(ProtocolDataStore, 21, nil))
		if kind != KindDocumented || len(resp.Body) != 8 || binary.LittleEndian.Uint64(resp.Body) != want {
			t.Fatalf("PostMetaBinary #%d: kind=%s body=% x", want, kind, resp.Body)
		}
	}
	// 8 GetMeta -> DataStore::NotFound error with the error bit set.
	resp, kind := r.Answer(s, ProtocolDataStore, req(ProtocolDataStore, 8, nil))
	if kind != KindNotFound || !resp.IsError || resp.Result != (resultDataStoreNotFound|nex.ResultErrorMask) {
		t.Fatalf("GetMeta: kind=%s err=%v result=%#x", kind, resp.IsError, resp.Result)
	}
	// 82/83 -> zero-length body (not an empty list).
	for _, m := range []uint32{82, 83} {
		resp, kind := r.Answer(s, ProtocolDataStore, req(ProtocolDataStore, m, nil))
		if kind != KindEmptyBody || len(resp.Body) != 0 || resp.IsError {
			t.Fatalf("0x73.%d: kind=%s body=% x", m, kind, resp.Body)
		}
	}
	// Undocumented Smash methods -> empty list fallback (never an error, never fabricated content).
	for _, m := range []uint32{60, 61, 84, 95, 47, 999} {
		resp, kind := r.Answer(s, ProtocolDataStore, req(ProtocolDataStore, m, nil))
		if kind != KindEmptyList || !bytes.Equal(resp.Body, []byte{0, 0, 0, 0}) {
			t.Fatalf("0x73.%d: kind=%s body=% x", m, kind, resp.Body)
		}
	}
	for _, m := range []uint32{9, 10, 11} {
		resp, kind := r.Answer(s, ProtocolUtility, req(ProtocolUtility, m, nil))
		if kind != KindEmptyList || !bytes.Equal(resp.Body, []byte{0, 0, 0, 0}) {
			t.Fatalf("0x6e.%d: kind=%s body=% x", m, kind, resp.Body)
		}
	}
	// Documented Utility unique-id methods are delegated to the core (PID-derived).
	resp, kind = r.Answer(s, ProtocolUtility, req(ProtocolUtility, nex.MethodAcquireNexUniqueID, nil))
	if kind != KindDelegated || resp.IsError || len(resp.Body) != 8 {
		t.Fatalf("AcquireNexUniqueID: kind=%s err=%v body=% x", kind, resp.IsError, resp.Body)
	}
}

// Dispatch through the real handler entry point (what the endpoint calls) and
// the call counters.
func TestHandlerDispatchAndCounters(t *testing.T) {
	r := New(Options{UtilityIntSettings: 3})
	conn := &nex.Connection{Settings: settings(), PID: 49153}
	h := r.Handler(ProtocolUtility)
	for i := 0; i < 2; i++ {
		resp := h(conn, req(ProtocolUtility, nex.MethodGetIntegerSettings, []byte{0, 0, 0, 0}))
		if resp == nil || resp.IsError || len(resp.Body) != 4+3*6 {
			t.Fatalf("dispatch: %+v", resp)
		}
	}
	if r.Calls(ProtocolUtility, nex.MethodGetIntegerSettings) != 2 || r.Calls(ProtocolDataStore, 60) != 0 {
		t.Fatalf("counters: %d / %d", r.Calls(ProtocolUtility, 7), r.Calls(ProtocolDataStore, 60))
	}
}

// Different connected players enter Online concurrently. Both first insertion
// and counter reads must be synchronized, including new method keys.
func TestConcurrentPlayerCalls(t *testing.T) {
	r := New(Options{})
	const players, calls = 8, 100
	start := make(chan struct{})
	var wg sync.WaitGroup
	for player := 0; player < players; player++ {
		wg.Add(1)
		go func(player int) {
			defer wg.Done()
			conn := &nex.Connection{Settings: settings(), PID: uint64(49153 + player)}
			h := r.Handler(ProtocolDataStore)
			<-start
			for i := 0; i < calls; i++ {
				for _, method := range []uint32{82, uint32(1000 + player)} {
					if reply := h(conn, req(ProtocolDataStore, method, nil)); reply == nil || reply.IsError {
						t.Errorf("player %d method %d failed", player, method)
					}
					_ = r.Calls(ProtocolDataStore, method)
				}
			}
		}(player)
	}
	close(start)
	wg.Wait()
	if got := r.Calls(ProtocolDataStore, 82); got != players*calls {
		t.Fatalf("shared method count = %d, want %d", got, players*calls)
	}
	for player := 0; player < players; player++ {
		if got := r.Calls(ProtocolDataStore, uint32(1000+player)); got != calls {
			t.Fatalf("player %d count = %d, want %d", player, got, calls)
		}
	}
}

// Operator override files are only consulted when ReplayDir is configured, use
// the exact file name convention, and are returned verbatim.
func TestOperatorOverrideFiles(t *testing.T) {
	dir := t.TempDir()
	payload := []byte{0x02, 0, 0, 0, 0xAA, 0xBB}
	if err := os.WriteFile(filepath.Join(dir, OverrideFileName(ProtocolDataStore, 60)), payload, 0o600); err != nil {
		t.Fatal(err)
	}
	if OverrideFileName(ProtocolDataStore, 60) != "0x73_60.bin" || OverrideFileName(ProtocolUtility, 7) != "0x6e_7.bin" {
		t.Fatalf("file names: %s %s", OverrideFileName(ProtocolDataStore, 60), OverrideFileName(ProtocolUtility, 7))
	}
	// An unrelated or mis-named file is not an override and is not listed.
	if err := os.WriteFile(filepath.Join(dir, "resp_0x73_84.bin"), []byte{1}, 0o600); err != nil {
		t.Fatal(err)
	}
	with := New(Options{ReplayDir: dir})
	if got := with.Overrides(); len(got) != 1 || got[0] != "0x73_60.bin" {
		t.Fatalf("override listing: %v", got)
	}
	if got := New(Options{}).Overrides(); got != nil {
		t.Fatalf("no replay dir must list nothing: %v", got)
	}
	resp, kind := with.Answer(settings(), ProtocolDataStore, req(ProtocolDataStore, 60, nil))
	if kind != KindOverride || !bytes.Equal(resp.Body, payload) {
		t.Fatalf("override: kind=%s body=% x", kind, resp.Body)
	}
	// A method without a file keeps the shape fallback.
	resp, kind = with.Answer(settings(), ProtocolDataStore, req(ProtocolDataStore, 84, nil))
	if kind != KindEmptyList || !bytes.Equal(resp.Body, []byte{0, 0, 0, 0}) {
		t.Fatalf("no file: kind=%s body=% x", kind, resp.Body)
	}
	without := New(Options{})
	resp, kind = without.Answer(settings(), ProtocolDataStore, req(ProtocolDataStore, 60, nil))
	if kind != KindEmptyList || !bytes.Equal(resp.Body, []byte{0, 0, 0, 0}) {
		t.Fatalf("replay dir unset must be ignored: kind=%s body=% x", kind, resp.Body)
	}
}
