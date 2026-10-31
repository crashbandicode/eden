//go:build linux

package autolan

import (
	"net"
	"net/netip"
	"os"
	"syscall"
	"testing"
	"time"
)

// Linux test edges: the frozen v6 harness behaviour.

// privateTempDir is a fresh 0700 directory below the temporary directory.
func privateTempDir(t *testing.T, prefix string) string {
	t.Helper()
	d, err := os.MkdirTemp("", prefix)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(d, 0o700); err != nil {
		t.Fatal(err)
	}
	return d
}

// assertPrivateFile checks that a runtime file is readable by the owner only (mode 0600).
func assertPrivateFile(t *testing.T, path string) {
	t.Helper()
	fi, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode().Perm() != 0o600 {
		t.Fatalf("%s mode %v", path, fi.Mode().Perm())
	}
}

// makeDirUntrusted widens a private directory so checkPrivateDir must refuse it.
func makeDirUntrusted(t *testing.T, dir string) {
	t.Helper()
	if err := os.Chmod(dir, 0o755); err != nil {
		t.Fatal(err)
	}
}

// createSocketNode creates a socket node at path that the broker must never remove.
func createSocketNode(t *testing.T, path string) func() {
	t.Helper()
	s, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: path, Net: "unixgram"})
	if err != nil {
		t.Fatal(err)
	}
	return func() { _ = s.Close() } // a datagram socket file is not unlinked on close
}

type unixgramGuest struct {
	sock   *net.UnixConn
	broker string
}

func newGuestTransport(t *testing.T, n *node, path string) guestTransport {
	t.Helper()
	sock, err := net.ListenUnixgram("unixgram", &net.UnixAddr{Name: path, Net: "unixgram"})
	if err != nil {
		t.Fatal(err)
	}
	return &unixgramGuest{sock: sock, broker: n.ready.AutoTransport.DatagramSocket}
}

func (g *unixgramGuest) sendRaw(frame []byte) error {
	_, err := g.sock.WriteToUnix(frame, &net.UnixAddr{Name: g.broker, Net: "unixgram"})
	return err
}

func (g *unixgramGuest) recv(timeout time.Duration) ([]byte, string, error) {
	buf := make([]byte, MaxFrame+1)
	_ = g.sock.SetReadDeadline(time.Now().Add(timeout))
	n, from, err := g.sock.ReadFromUnix(buf)
	if err != nil {
		return nil, "", err
	}
	sender := ""
	if from != nil {
		sender = from.Name
	}
	return buf[:n], sender, nil
}

func (g *unixgramGuest) close() { _ = g.sock.Close() }

// blackHoleEndpoint is a loopback TCP endpoint that silently drops connection
// attempts, like a default-deny firewall: a listener whose accept queue is full
// (backlog 0, filled and never accepted), so further SYNs are dropped. It checks
// that a connect really hangs rather than failing fast.
func blackHoleEndpoint(t *testing.T) netip.AddrPort {
	t.Helper()
	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_STREAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = syscall.Close(fd) })
	if err := syscall.Bind(fd, &syscall.SockaddrInet4{Addr: [4]byte{127, 0, 0, 1}}); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Listen(fd, 0); err != nil {
		t.Fatal(err)
	}
	sa, err := syscall.Getsockname(fd)
	if err != nil {
		t.Fatal(err)
	}
	ep := netip.AddrPortFrom(netip.MustParseAddr("127.0.0.1"), uint16(sa.(*syscall.SockaddrInet4).Port))
	for i := 0; i < 4; i++ {
		start := time.Now()
		c, err := dialCoordinator("tcp4", ep.String(), "", netip.Addr{}, true, nil, time.Now().Add(700*time.Millisecond))
		if err == nil {
			t.Cleanup(func() { _ = c.Close() }) // fills the accept queue; never accepted
			continue
		}
		if time.Since(start) < 600*time.Millisecond {
			t.Fatalf("black hole failed fast (%v): not a silent drop", err)
		}
		return ep
	}
	t.Fatal("could not fill the accept queue to emulate a silent drop")
	return netip.AddrPort{}
}

// processGone reports that pid has exited (a zombie has already exited).
func processGone(pid int) bool {
	data, err := os.ReadFile("/proc/" + itoa(pid) + "/stat")
	if err != nil {
		return true
	}
	text := string(data)
	fields := splitFields(text[lastIndexByte(text, ')')+1:])
	return len(fields) > 0 && fields[0] == "Z"
}

// killHard terminates a child without any chance to clean up.
func killHard(p *os.Process) error { return p.Signal(syscall.SIGKILL) }

const nonexistentDir = "/nonexistent-eln"
