package autolan

import (
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"
)

// retainingGoroutines lists the entry functions of live goroutines that belong
// to the NEX core, nsoserver or autolan (diagnostics for a retention failure).
func retainingGoroutines() string {
	buf := make([]byte, 1<<20)
	buf = buf[:runtime.Stack(buf, true)]
	seen := map[string]int{}
	for _, g := range strings.Split(string(buf), "\n\n") {
		lines := strings.Split(g, "\n")
		for _, l := range lines[1:] {
			if (strings.Contains(l, "nextendo-nex.") || strings.Contains(l, "nsoserver.") || strings.Contains(l, "autolan.")) && !strings.HasPrefix(l, "\t") {
				seen[strings.TrimSpace(strings.SplitN(l, "(0x", 2)[0])]++
				break
			}
		}
	}
	var out []string
	for fn, n := range seen {
		out = append(out, fn+" x"+strconv.Itoa(n))
	}
	return strings.Join(out, "\n")
}

// TestReturnToLeaderGetsFreshNEXHostAndReleasesOldOne: A leads with a real NEX
// host, B is a member. A's NEX host stops; A must stop leading, win the
// re-election again (lowest instance) with a fresh coordinator, keep both
// identities and ready.json, serve through the local listener again, and the
// first coordinator must become unreachable (nothing in the process retains it).
func TestReturnToLeaderGetsFreshNEXHostAndReleasesOldOne(t *testing.T) {
	t.Setenv("NEXTENDO_REAP_IDLE_SECONDS", "1")
	t.Setenv("NEXTENDO_REAP_EVERY_SECONDS", "1")
	w := newWorld(t)
	h := w.newHost(1)
	a := startNode(t, w.options(h, instanceID(1))).waitReady()
	b := startNode(t, w.options(h, instanceID(2))).waitReady()
	readyA, _ := os.ReadFile(filepath.Join(a.rd, readyFileName))
	c1 := a.b.coordPub.Load()
	if c1 == nil || !a.b.leading.Load() {
		t.Fatal("A is not leading")
	}
	reapPasses := func(c *Coordinator) uint64 { return c.host.(*realNEX).s.ReapPasses() }
	eventually(t, 4*time.Second, "idle reaper bound to the first NEX host", func() bool { return reapPasses(c1) > 0 })
	epoch1 := c1.epoch
	released := make(chan struct{})
	// A finalizer never runs on an object inside a reference cycle (the coordinator
	// and its NEX host reference each other), so watch a cycle-free leaf that only
	// the first coordinator owns: it is collectable exactly when nothing retains c1.
	runtime.SetFinalizer(c1.authL, func(*chanListener) { close(released) })
	c1.cancel() // the NEX host stops (Serve returns)
	c1 = nil
	eventually(t, 10*time.Second, "return to leader with a fresh coordinator", func() bool {
		c2 := a.b.coordPub.Load()
		r := b.control().call(t, `{"op":"status"}`)
		return c2 != nil && c2.epoch != epoch1 && a.b.leading.Load() && r.Connected != nil && *r.Connected
	})
	for _, n := range []*node{a, b} {
		r := n.control().call(t, `{"op":"status"}`)
		if r.AccountID != n.ready.AccountID || r.InstanceIP != n.ready.AutoTransport.InstanceIP {
			t.Fatalf("identity changed: %+v", r)
		}
	}
	if now, _ := os.ReadFile(filepath.Join(a.rd, readyFileName)); string(now) != string(readyA) {
		t.Fatal("ready.json changed")
	}
	for _, n := range []*node{a, b} {
		if got, err := httpsVia(t, n.ready.PortRedirects[0].To); err != nil || !strings.HasPrefix(got, "404") {
			t.Fatalf("new NEX host not reachable: %q %v", got, err)
		}
	}
	eventually(t, 4*time.Second, "idle reaper bound to the second NEX host", func() bool { return reapPasses(a.b.coordPub.Load()) > 0 })
	deadline := time.Now().Add(5 * time.Second)
	for {
		runtime.GC()
		select {
		case <-released:
			return
		case <-time.After(100 * time.Millisecond):
		}
		if time.Now().After(deadline) {
			t.Fatalf("the first coordinator is still reachable after re-election; live goroutines:\n%s", retainingGoroutines())
		}
	}
}
