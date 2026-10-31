package autolan

import (
	"bytes"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

const profileReportHex = "0019000000074433221188776655ccbbaa99080706050403020101000000"
const profileReplyHex = "0100000007" + profileReportHex + "00140000000000000000000000000000000000000000000000"

func profileWire(t *testing.T, s string) []byte {
	t.Helper()
	b, e := hex.DecodeString(s)
	if e != nil {
		t.Fatal(e)
	}
	return b
}

// Two real brokers on simulated distinct hosts, with real TCP/member links.
// Invoke the registered NEX handler after the auth boundary, with leased PIDs.
// Auth itself is covered by nsoserver's real auth/secure integration tests.
func TestProfileRoutingSurvivesCoordinatorAndLeaseChanges(t *testing.T) {
	w := newWorld(t)
	ha, hb := w.newHost(1), w.newHost(2)
	pa, pb := shortDir(t, "elp"), shortDir(t, "elp")
	oa, ob := w.options(ha, instanceID(1)), w.options(hb, instanceID(2))
	oa.ProfileDir = pa
	ob.ProfileDir = pb
	a := startNode(t, oa).waitReady()
	b := startNode(t, ob).waitReady()
	eventually(t, 5*time.Second, "one leader", func() bool {
		return len(leaders(a, b)) == 1 && a.b.coordPub.Load() != nil && a.b.coordPub.Load().memberCount() == 2
	})
	call := func(c *Coordinator, pid uint32, method uint32, body []byte, source ...string) *nex.RMCMessage {
		t.Helper()
		s := c.host.(*realNEX).s
		remote := ""
		c.mu.Lock()
		if st := c.members[c.byPID[pid]]; st != nil {
			remote = st.vip.String() + ":1024"
		}
		c.mu.Unlock()
		if len(source) > 0 {
			remote = source[0]
		}
		return s.Init.Handler(0x73)(&nex.Connection{PID: uint64(pid), RemoteAddr: remote, Settings: s.SecureSettings}, nex.NewRMCRequest(s.SecureSettings, 0x73, method, 123, body))
	}
	c := a.b.coordPub.Load()
	oldPID := a.ready.AccountID
	if r := call(c, oldPID, 82, profileWire(t, profileReportHex)); r.IsError {
		t.Fatal("profile write failed")
	}
	if r := call(c, b.ready.AccountID, 84, nil); r.IsError || len(r.Body) != 29 {
		t.Fatal("rating leaked to other profile")
	}
	for _, method := range []uint32{82, 84} {
		var body []byte
		if method == 82 {
			body = profileWire(t, profileReportHex)
			body[14] ^= 0xff
		}
		if r := call(c, oldPID, method, body, b.vip().String()+":1025"); !r.IsError {
			t.Fatal("another member impersonated profile owner")
		}
	}
	if r := call(c, oldPID, 84, nil); r.IsError || !bytes.Equal(r.Body, profileWire(t, profileReplyHex)) {
		t.Fatal("rejected cross-profile write mutated owner")
	}
	if r := call(c, 42, 84, nil); !r.IsError {
		t.Fatal("unleased PID read allowed")
	}
	a.stop()
	eventually(t, 8*time.Second, "survivor coordinator", func() bool { return b.b.coordPub.Load() != nil && b.b.coordPub.Load().memberCount() == 1 })
	oa = w.options(ha, instanceID(3))
	oa.ProfileDir = pa
	a2 := startNode(t, oa).waitReady()
	eventually(t, 5*time.Second, "restarted profile joined", func() bool { return b.b.coordPub.Load() != nil && b.b.coordPub.Load().memberCount() == 2 })
	if a2.ready.AccountID == oldPID {
		t.Fatal("test did not change leased PID")
	}
	c = b.b.coordPub.Load()
	if r := call(c, a2.ready.AccountID, 84, nil); r.IsError || !bytes.Equal(r.Body, profileWire(t, profileReplyHex)) {
		t.Fatal("profile lost rating after coordinator/lease change")
	}
	if r := call(c, b.ready.AccountID, 84, nil); r.IsError || len(r.Body) != 29 {
		t.Fatal("survivor inherited departed profile")
	}
	if r := call(c, oldPID, 84, nil); !r.IsError {
		t.Fatal("departed lease still reads profile")
	}
}

func TestProfileExclusiveOwnershipAndSeeds(t *testing.T) {
	dir := shortDir(t, "elp")
	b := &Broker{opt: Options{ProfileDir: dir}}
	if err := b.openProfile(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		if b.profileLock != nil {
			b.profileLock.Close()
		}
	}()
	second := &Broker{opt: Options{ProfileDir: dir}}
	if err := second.openProfile(); err == nil {
		second.profileLock.Close()
		t.Fatal("concurrent profile writer accepted")
	}
	if _, code := b.exchangeProfile(82, profileWire(t, profileReportHex)); code != 0 {
		t.Fatal("write")
	}
	b.profileLock.Close()
	b.profileLock = nil
	second.opt.FighterSeeds = "7:10000001"
	if err := second.openProfile(); err != nil {
		t.Fatal(err)
	}
	defer second.profileLock.Close()
	want := profileWire(t, "010000000700190000000781969800887766558196980008070605040302010100000000140000000000000000000000000000000000000000000000")
	got, code := second.exchangeProfile(84, nil)
	if code != 0 || !bytes.Equal(got, want) {
		t.Fatalf("seed reply %x", got)
	}
}

// The saver process exits successfully before a different process restores.
func TestProfileFreshProcessContinuation(t *testing.T) {
	if path := os.Getenv("EDEN_PROFILE_HELPER_PATH"); path != "" {
		b := &Broker{opt: Options{ProfileDir: path}}
		if err := b.openProfile(); err != nil {
			t.Fatal(err)
		}
		defer b.profileLock.Close()
		if os.Getenv("EDEN_PROFILE_HELPER_ACTION") == "write" {
			if _, code := b.exchangeProfile(82, profileWire(t, profileReportHex)); code != 0 {
				t.Fatal("write")
			}
		} else {
			got, code := b.exchangeProfile(84, nil)
			if code != 0 || !bytes.Equal(got, profileWire(t, profileReplyHex)) {
				t.Fatal("restore continuation mismatch")
			}
		}
		return
	}
	path := shortDir(t, "elp")
	for _, action := range []string{"write", "read"} {
		cmd := exec.Command(os.Args[0], "-test.run=^TestProfileFreshProcessContinuation$", "-test.count=1")
		cmd.Env = append(os.Environ(), "EDEN_PROFILE_HELPER_PATH="+path, "EDEN_PROFILE_HELPER_ACTION="+action)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("%s subprocess: %v %s", action, err, out)
		}
	}
	if _, err := os.Stat(filepath.Join(path, "ratings.json")); err != nil {
		t.Fatal(err)
	}
}
