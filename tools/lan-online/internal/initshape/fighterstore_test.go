package initshape

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// The subprocess starts a new Go process, not just a new connection or object.
func TestFighterStoreSurvivesFreshProcess(t *testing.T) {
	if path := os.Getenv("EDEN_FIGHTER_STORE_HELPER"); path != "" {
		r := New(Options{FighterStats: true})
		if err := r.LoadFighterState(path, map[uint64]bool{49153: true, 49154: true}, nil); err != nil {
			t.Fatal(err)
		}
		if got := fighterCall(r, 49153, 84, nil); got.IsError || !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
			t.Fatal("new process lost report")
		}
		if got := fighterCall(r, 49154, 84, nil); got.IsError || len(got.Body) != 29 {
			t.Fatal("new process leaked other PID")
		}
		return
	}
	path := filepath.Join(t.TempDir(), "ratings.json")
	r := New(Options{FighterStats: true})
	if err := r.LoadFighterState(path, map[uint64]bool{49153: true, 49154: true}, nil); err != nil {
		t.Fatal(err)
	}
	if fighterCall(r, 49153, 82, wire(t, report7)).IsError {
		t.Fatal("write failed")
	}
	cmd := exec.Command(os.Args[0], "-test.run=^TestFighterStoreSurvivesFreshProcess$", "-test.count=1")
	cmd.Env = append(os.Environ(), "EDEN_FIGHTER_STORE_HELPER="+path)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("fresh process: %v %s", err, out)
	}
}

func TestFighterStoreSeedsConsistentRatingPair(t *testing.T) {
	path := filepath.Join(t.TempDir(), "ratings.json")
	allowed := map[uint64]bool{49153: true, 49154: true}
	r := New(Options{FighterStats: true})
	if err := r.LoadFighterState(path, allowed, nil); err != nil {
		t.Fatal(err)
	}
	for _, pid := range []uint64{49153, 49154} {
		if fighterCall(r, pid, 82, wire(t, report7)).IsError {
			t.Fatal("write")
		}
	}
	seeded := New(Options{FighterStats: true})
	if err := seeded.LoadFighterState(path, allowed, map[uint64]map[byte]uint32{49153: {7: 10000001}}); err != nil {
		t.Fatal(err)
	}
	want := wire(t, "0100000007"+report7+summaryZero)
	binary.LittleEndian.PutUint32(want[11:15], 10000001)
	binary.LittleEndian.PutUint32(want[19:23], 10000001)
	if got := fighterCall(seeded, 49153, 84, nil); !bytes.Equal(got.Body, want) {
		t.Fatal("seed altered other fields")
	}
	if got := fighterCall(seeded, 49154, 84, nil); !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
		t.Fatal("seed altered opponent")
	}
	// Once removed from startup config, a subsequent game report wins.
	if fighterCall(seeded, 49153, 82, wire(t, report7)).IsError {
		t.Fatal("write after seed")
	}
	plain := New(Options{FighterStats: true})
	if err := plain.LoadFighterState(path, allowed, nil); err != nil {
		t.Fatal(err)
	}
	if got := fighterCall(plain, 49153, 84, nil); !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
		t.Fatal("seed was sticky")
	}
	before, _ := os.ReadFile(path)
	if err := plain.LoadFighterState(path, allowed, map[uint64]map[byte]uint32{49153: {8: 42}}); err == nil {
		t.Fatal("invented missing fighter")
	}
	after, _ := os.ReadFile(path)
	if !bytes.Equal(before, after) {
		t.Fatal("failed seed changed file")
	}
}

func TestFighterStoreWriteFailureDoesNotAcknowledgeOrMutate(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "ratings.json")
	r := New(Options{FighterStats: true})
	if err := r.LoadFighterState(path, map[uint64]bool{49153: true}, nil); err != nil {
		t.Fatal(err)
	}
	if fighterCall(r, 49153, 82, wire(t, report7)).IsError {
		t.Fatal("initial write")
	}
	// Deterministic on Unix and Windows, including privileged test users.
	if err := os.Rename(dir, dir+"-moved"); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(dir + "-moved") })
	b := wire(t, report7)
	b[14] = 0
	if !fighterCall(r, 49153, 82, b).IsError {
		t.Fatal("failed write acknowledged")
	}
	if got := fighterCall(r, 49153, 84, nil); !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
		t.Fatal("failed write changed memory")
	}
	newReader := New(Options{FighterStats: true})
	if err := newReader.LoadFighterState(filepath.Join(dir+"-moved", "ratings.json"), map[uint64]bool{49153: true}, nil); err != nil {
		t.Fatal(err)
	}
	if got := fighterCall(newReader, 49153, 84, nil); !bytes.Equal(got.Body, wire(t, "0100000007"+report7+summaryZero)) {
		t.Fatal("failed write changed disk")
	}
}

func TestFighterStoreRejectsInvalidFile(t *testing.T) {
	for _, data := range []string{
		`{"version":2,"records":[]}`,
		`{"version":1,"records":[]} {}`,
		`{"version":1,"records":[],"typo":1}`,
		`{"version":1,"records":[{"pid":49154,"record":"` + report7 + `"}]}`,
		`{"version":1,"records":[{"pid":49153,"record":"00"}]}`,
		`{"version":1,"records":[{"pid":49153,"record":"` + report7 + `"},{"pid":49153,"record":"` + report7 + `"}]}`,
	} {
		path := filepath.Join(t.TempDir(), "ratings.json")
		if err := os.WriteFile(path, []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		r := New(Options{FighterStats: true})
		if err := r.LoadFighterState(path, map[uint64]bool{49153: true}, nil); err == nil {
			t.Fatalf("accepted %s", data)
		}
		after, _ := os.ReadFile(path)
		if string(after) != data {
			t.Fatal("bad file overwritten")
		}
	}
}
