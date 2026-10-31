package identity

import (
	"strconv"
	"sync"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

func TestDynamicAddRemoveOnlyRegisteredResolve(t *testing.T) {
	r := NewDynamic()
	if pid, _, res := r.Resolve("305419896", nil); pid != 0 || res != nex.ResultAuthTokenParseError {
		t.Fatalf("unregistered username resolved: pid=%d res=%#x", pid, res)
	}
	if err := r.Add("305419896", 305419896); err != nil {
		t.Fatal(err)
	}
	if err := r.Add("305419896", 305419896); err != nil {
		t.Fatalf("identical re-add must be a no-op: %v", err)
	}
	if pid, key, res := r.Resolve("305419896", nil); pid != 305419896 || res != 0 || len(key) != SourceKeyLen {
		t.Fatalf("registered username: pid=%d res=%#x keylen=%d", pid, res, len(key))
	}
	for _, bad := range []struct {
		name string
		pid  uint64
	}{{"305419896", 7}, {"other", 305419896}, {"", 9}, {"zero", 0}} {
		if r.Add(bad.name, bad.pid) == nil {
			t.Fatalf("Add(%q,%d) accepted a conflicting/invalid identity", bad.name, bad.pid)
		}
	}
	if !r.Remove("305419896") || r.Remove("305419896") {
		t.Fatal("Remove must report presence exactly once")
	}
	if pid, _, _ := r.Resolve("305419896", nil); pid != 0 {
		t.Fatal("removed identity still resolves")
	}
}

func TestDynamicConcurrentAddResolve(t *testing.T) {
	r := NewDynamic()
	var wg sync.WaitGroup
	for i := 1; i <= 32; i++ {
		wg.Add(2)
		go func(i int) {
			defer wg.Done()
			_ = r.Add(strconv.Itoa(1000+i), uint64(1000+i))
		}(i)
		go func(i int) {
			defer wg.Done()
			r.Resolve(strconv.Itoa(1000+i), nil)
		}(i)
	}
	wg.Wait()
	if len(r.PIDs()) != 32 {
		t.Fatalf("expected 32 identities, got %d", len(r.PIDs()))
	}
}
