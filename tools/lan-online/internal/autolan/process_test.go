package autolan

import (
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"
)

// Small string helpers so the platform files need no extra imports.
func itoa(n int) string              { return strconv.Itoa(n) }
func splitFields(s string) []string  { return strings.Fields(s) }
func lastIndexByte(s string, c byte) int { return strings.LastIndexByte(s, c) }

// TestHelperProcess is re-executed by TestParentExitStopsBrokerAndCleansUp. In
// "parent" mode it starts a broker child and then blocks until it is killed;
// in "broker" mode it runs the real broker loop with the parent's PID.
func TestHelperProcess(t *testing.T) {
	switch os.Getenv("AUTOLAN_HELPER") {
	case "parent":
		cmd := exec.Command(os.Args[0], "-test.run=^TestHelperProcess$")
		cmd.Env = append(os.Environ(), "AUTOLAN_HELPER=broker")
		if err := cmd.Start(); err != nil {
			os.Exit(3)
		}
		_ = os.WriteFile(os.Getenv("AUTOLAN_PIDFILE"), []byte(strconv.Itoa(cmd.Process.Pid)), 0o600)
		for {
			time.Sleep(time.Hour) // killed by the test (a bare select{} trips the deadlock detector)
		}
	case "broker":
		opt := Options{
			ParentPID: os.Getppid(), RuntimeDir: os.Getenv("AUTOLAN_RD"), AssetsDir: os.Getenv("AUTOLAN_ASSETS"),
			TitleID: testTitle, SharedDir: os.Getenv("AUTOLAN_SHARED"), LocalOnly: true,
			Instance: os.Getenv("AUTOLAN_INSTANCE"), ControlPeerPID: os.Getppid(),
			DiscoveryWindow: 200 * time.Millisecond,
		}
		if err := Run(context.Background(), opt); err != nil {
			os.Exit(1)
		}
		os.Exit(0)
	case "main":
		os.Exit(Main(strings.Fields(os.Getenv("AUTOLAN_ARGS"))))
	default:
		t.Skip("helper process only")
	}
}

// TestMainEntryPointStatus runs the real CLI entry point in a child process.
// Only argument/asset/title failures are exercised here, so no LAN discovery runs.
func TestMainEntryPointStatus(t *testing.T) {
	assets := syntheticAssets(t)
	runMain := func(args string) (int, string) {
		t.Helper()
		cmd := exec.Command(os.Args[0], "-test.run=^TestHelperProcess$")
		cmd.Env = append(os.Environ(), "AUTOLAN_HELPER=main", "AUTOLAN_ARGS="+args)
		err := cmd.Run()
		code := 0
		if ee, ok := err.(*exec.ExitError); ok {
			code = ee.ExitCode()
		} else if err != nil {
			t.Fatal(err)
		}
		return code, ""
	}
	parent := strconv.Itoa(os.Getpid())
	rd := shortDir(t, "elr")
	if code, _ := runMain("--parent-pid " + parent + " --runtime-dir " + rd + " --assets " + assets + " --title-id 0100000000000000"); code != 1 {
		t.Fatalf("unsupported title exit %d", code)
	}
	if data, _ := os.ReadFile(filepath.Join(rd, errorFileName)); !strings.Contains(string(data), `"code":"unsupported_title"`) {
		t.Fatalf("error.json %q", data)
	}
	rd2 := shortDir(t, "elr")
	if code, _ := runMain("--parent-pid " + parent + " --runtime-dir " + rd2 + " --assets " + rd2 + " --title-id " + testTitle); code != 1 {
		t.Fatalf("missing assets exit %d", code)
	}
	if data, _ := os.ReadFile(filepath.Join(rd2, errorFileName)); !strings.Contains(string(data), `"code":"missing_assets"`) {
		t.Fatalf("error.json %q", data)
	}
	if code, _ := runMain("--parent-pid x --runtime-dir " + rd); code != 2 {
		t.Fatalf("bad arguments exit %d", code)
	}
	if code, _ := runMain("--parent-pid " + parent + " --runtime-dir " + nonexistentDir + " --assets " + assets + " --title-id " + testTitle); code != 2 {
		t.Fatalf("invalid runtime dir exit %d", code)
	}
}

func TestRestartInSameRuntimeDirectory(t *testing.T) {
	w := newWorld(t)
	h := w.newHost(1)
	opt := w.options(h, instanceID(1))
	first := startNode(t, opt).waitReady()
	first.stop()
	for _, p := range []string{readyFileName, controlSocketName, dataSocketName} {
		if _, err := os.Lstat(filepath.Join(opt.RuntimeDir, p)); err == nil {
			t.Fatalf("%s left after stop", p)
		}
	}
	opt2 := w.options(h, instanceID(2))
	opt2.RuntimeDir = opt.RuntimeDir
	opt2.Network = w.hub.announcer(instanceID(2))
	second := startNode(t, opt2).waitReady()
	g := second.guest("g", 50300)
	g.send(g.ep, []byte("self"))
	if f, _ := g.mustRecv(2 * time.Second); string(f.Payload) != "self" {
		t.Fatal("restarted broker does not route")
	}
}

func TestLogWriterNeverBlocks(t *testing.T) {
	r, w := io.Pipe() // nobody reads: every write to w blocks
	defer r.Close()
	d := newDropWriter(w, 4)
	start := time.Now()
	for i := 0; i < 100; i++ {
		_, _ = d.Write([]byte("line\n"))
	}
	if time.Since(start) > time.Second || d.dropped.Load() == 0 {
		t.Fatalf("writer blocked or dropped nothing: %v dropped=%d", time.Since(start), d.dropped.Load())
	}
	_ = w.Close()
	d.Close()
	_, _ = d.Write([]byte("after close\n")) // must not panic
}

func TestParentExitStopsBrokerAndCleansUp(t *testing.T) {
	rd, shared, assets, tmp := shortDir(t, "elr"), shortDir(t, "els"), syntheticAssets(t), shortDir(t, "elp")
	// A guest-owned client socket that the broker must never unlink.
	clientPath := filepath.Join(rd, "client.sock")
	closeClient := createSocketNode(t, clientPath)
	defer closeClient()
	inst := instanceID(0x77)
	pidfile := filepath.Join(tmp, "broker.pid")
	parent := exec.Command(os.Args[0], "-test.run=^TestHelperProcess$")
	parent.Env = append(os.Environ(), "AUTOLAN_HELPER=parent", "AUTOLAN_PIDFILE="+pidfile, "AUTOLAN_RD="+rd,
		"AUTOLAN_ASSETS="+assets, "AUTOLAN_SHARED="+shared, "AUTOLAN_INSTANCE="+inst)
	if err := parent.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = parent.Process.Kill(); _, _ = parent.Process.Wait() }()
	eventually(t, 15*time.Second, "child broker ready", func() bool {
		_, err := os.Stat(filepath.Join(rd, readyFileName))
		return err == nil
	})
	raw, err := os.ReadFile(pidfile)
	if err != nil {
		t.Fatal(err)
	}
	brokerPID, _ := strconv.Atoi(string(raw))
	meta := filepath.Join(shared, "brokers", inst+".json")
	coordSock := filepath.Join(shared, "coord-"+inst+".sock")
	for _, p := range []string{filepath.Join(rd, "control.sock"), filepath.Join(rd, "data.sock"), meta, coordSock} {
		if _, err := os.Lstat(p); err != nil {
			t.Fatalf("expected %s while running: %v", p, err)
		}
	}
	if err := killHard(parent.Process); err != nil {
		t.Fatal(err)
	}
	_, _ = parent.Process.Wait()
	eventually(t, 5*time.Second, "broker exit after parent death", func() bool { return processGone(brokerPID) })
	for _, p := range []string{filepath.Join(rd, "control.sock"), filepath.Join(rd, "data.sock"), filepath.Join(rd, readyFileName), meta, coordSock, filepath.Join(rd, errorFileName)} {
		if _, err := os.Lstat(p); err == nil {
			t.Fatalf("%s left behind after parent exit", p)
		}
	}
	if _, err := os.Lstat(clientPath); err != nil {
		t.Fatalf("broker removed a guest-owned socket: %v", err)
	}
}
