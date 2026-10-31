package autolan

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

// Test harness. Every test runs real brokers in-process with real AF_UNIX,
// TCP and UDP sockets on loopback. Only LAN beacon delivery is injected
// (memHub), so no multicast leaves the host. Assets are synthetic bytes, never
// private game replies. The guest-side data endpoint is the platform's native
// one (guest_linux_test.go, guest_windows_test.go); test bodies are shared.

var testLog = os.Getenv("AUTOLAN_TEST_LOG") != ""

func tlogf(format string, args ...any) {
	if testLog {
		log.Printf(format, args...)
	}
}

const testTitle = "01006a800016e000"

// shortDir creates a private temporary directory with a short path (AF_UNIX
// paths are limited); the platform helper applies the private-directory model.
func shortDir(t *testing.T, prefix string) string {
	t.Helper()
	d := privateTempDir(t, prefix)
	t.Cleanup(func() { _ = os.RemoveAll(d) })
	return d
}

func syntheticAssets(t *testing.T) string {
	t.Helper()
	d := shortDir(t, "ela")
	for _, n := range requiredAssets {
		if err := os.WriteFile(filepath.Join(d, n), []byte("synthetic test asset; not a game reply\n"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return d
}

func instanceID(n int) string { return fmt.Sprintf("%032x", n) }

type world struct {
	t      *testing.T
	hub    *memHub
	assets string
}

func newWorld(t *testing.T) *world {
	return &world{t: t, hub: newMemHub(), assets: syntheticAssets(t)}
}

// host simulates one machine: one shared directory and one same-host key.
type host struct {
	shared string
	key    string
}

func (w *world) newHost(n int) host {
	return host{shared: shortDir(w.t, "els"), key: fmt.Sprintf("%032x", 0xabc000+n)}
}

func (w *world) options(h host, instance string) Options {
	return Options{
		ParentPID: os.Getppid(), RuntimeDir: shortDir(w.t, "elr"), AssetsDir: w.assets, TitleID: testTitle,
		SharedDir: h.shared, LANIP: netip.MustParseAddr("127.0.0.1"), LocalKey: h.key,
		Network: w.hub.announcer(instance), Instance: instance, ControlPeerPID: os.Getpid(),
		DiscoveryWindow: 300 * time.Millisecond, LeaderWait: time.Second,
		MemberTimeout: 2 * time.Second, LeaderTimeout: 1500 * time.Millisecond, PruneAfter: 2 * time.Second,
		Logf: tlogf, allowLoopback: true,
	}
}

type node struct {
	t      *testing.T
	b      *Broker
	rd     string
	cancel context.CancelFunc
	done   chan struct{}
	err    error
	once   sync.Once
	ready  readyFile
}

func startNode(t *testing.T, opt Options) *node {
	t.Helper()
	opt.withDefaults()
	b, err := newBroker(opt)
	if err != nil {
		t.Fatalf("newBroker: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	n := &node{t: t, b: b, rd: opt.RuntimeDir, cancel: cancel, done: make(chan struct{})}
	go func() {
		n.err = b.run(ctx)
		b.shutdown()
		close(n.done)
	}()
	t.Cleanup(n.stop)
	return n
}

func (n *node) stop() {
	n.once.Do(func() {
		n.cancel()
		select {
		case <-n.done:
		case <-time.After(10 * time.Second):
			n.t.Errorf("broker %s did not stop", n.b.instance)
		}
	})
}

func (n *node) waitReady() *node {
	n.t.Helper()
	p := filepath.Join(n.rd, readyFileName)
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if data, err := os.ReadFile(p); err == nil {
			if err := json.Unmarshal(data, &n.ready); err != nil {
				n.t.Fatalf("ready.json: %v", err)
			}
			return n
		}
		time.Sleep(20 * time.Millisecond)
	}
	n.t.Fatalf("broker %s never became ready", n.b.instance)
	return n
}

func (n *node) vip() netip.Addr { return netip.MustParseAddr(n.ready.AutoTransport.InstanceIP) }

type ctlClient struct {
	c *net.UnixConn
	r *bufio.Reader
}

func (n *node) control() *ctlClient {
	n.t.Helper()
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: n.ready.AutoTransport.ControlSocket, Net: "unix"})
	if err != nil {
		n.t.Fatal(err)
	}
	n.t.Cleanup(func() { _ = c.Close() })
	return &ctlClient{c: c, r: bufio.NewReader(c)}
}

func (cc *ctlClient) call(t *testing.T, line string) controlResponse {
	t.Helper()
	_ = cc.c.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := cc.c.Write([]byte(line + "\n")); err != nil {
		t.Fatalf("control write: %v", err)
	}
	resp, err := cc.r.ReadBytes('\n')
	if err != nil {
		t.Fatalf("control read: %v", err)
	}
	var out controlResponse
	if err := json.Unmarshal(resp, &out); err != nil {
		t.Fatalf("control reply %q: %v", resp, err)
	}
	return out
}

// guestTransport is a guest socket's data endpoint as the platform provides it: a Unix datagram
// socket on Linux, a hello-authenticated AF_UNIX stream on Windows.
type guestTransport interface {
	sendRaw(frame []byte) error
	// recv returns one frame and the name of the broker endpoint that delivered it.
	recv(timeout time.Duration) ([]byte, string, error)
	close()
}

type guest struct {
	t    *testing.T
	n    *node
	tr   guestTransport
	path string
	ep   netip.AddrPort
}

// guest creates the socket node `name` in the runtime dir and registers it with the broker.
func (n *node) guest(name string, port int) *guest {
	n.t.Helper()
	path := filepath.Join(n.rd, name+".sock")
	tr := newGuestTransport(n.t, n, path)
	n.t.Cleanup(tr.close)
	resp := n.control().call(n.t, fmt.Sprintf(`{"op":"bind","socket":%q,"port":%d}`, path, port))
	if !resp.OK {
		n.t.Fatalf("bind %s port %d: %s", name, port, resp.Error)
	}
	return &guest{t: n.t, n: n, tr: tr, path: path, ep: netip.AddrPortFrom(n.vip(), resp.Port)}
}

func (g *guest) sendRaw(frame []byte) {
	g.t.Helper()
	if err := g.tr.sendRaw(frame); err != nil {
		g.t.Fatalf("send: %v", err)
	}
}

func (g *guest) send(dst netip.AddrPort, payload []byte) {
	g.t.Helper()
	frame, err := AppendFrame(nil, g.ep, dst, payload)
	if err != nil {
		g.t.Fatal(err)
	}
	g.sendRaw(frame)
}

func (g *guest) recv(timeout time.Duration) (Frame, string, bool) {
	raw, sender, err := g.tr.recv(timeout)
	if err != nil {
		return Frame{}, "", false
	}
	f, err := DecodeFrame(raw)
	if err != nil {
		g.t.Fatalf("broker delivered an invalid frame: %v", err)
	}
	return f, sender, true
}

func (g *guest) mustRecv(timeout time.Duration) (Frame, string) {
	g.t.Helper()
	f, s, ok := g.recv(timeout)
	if !ok {
		g.t.Fatalf("guest %s received nothing", g.path)
	}
	return f, s
}

// drain discards anything queued (late probes from an eventually loop).
func (g *guest) drain() {
	for {
		if _, _, ok := g.recv(50 * time.Millisecond); !ok {
			return
		}
	}
}

func (g *guest) expectNone(d time.Duration) {
	g.t.Helper()
	if f, _, ok := g.recv(d); ok {
		g.t.Fatalf("unexpected delivery from %v to %v (%d bytes)", f.Src, f.Dst, len(f.Payload))
	}
}

func eventually(t *testing.T, d time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// status queries one broker's control socket (shared by the transport, review, tls and v6 suites).
func status(t *testing.T, n *node) controlResponse {
	t.Helper()
	return n.control().call(t, `{"op":"status"}`)
}

func leaders(nodes ...*node) []*node {
	var out []*node
	for _, n := range nodes {
		if n.b.leading.Load() {
			out = append(out, n)
		}
	}
	return out
}
