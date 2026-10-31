//go:build windows

package autolan

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// Windows test edges. Private temporary directories are created below one protected root per
// test process (inherited DACL: this account and SYSTEM only), the guest data endpoint is a
// hello-authenticated AF_UNIX stream, and process facts use Win32 handles.

var testRoot struct {
	path string
	err  error
	done bool
}

func privateTempDir(t *testing.T, prefix string) string {
	t.Helper()
	if !testRoot.done {
		testRoot.done = true
		base := os.Getenv("LOCALAPPDATA")
		if base == "" {
			base = os.TempDir()
		}
		testRoot.path = filepath.Join(base, "Temp", fmt.Sprintf("el%d", os.Getpid()))
		if err := os.MkdirAll(filepath.Dir(testRoot.path), 0o700); err != nil {
			testRoot.err = err
		} else {
			testRoot.err = createPrivateDirectory(testRoot.path)
		}
	}
	if testRoot.err != nil {
		t.Fatal(testRoot.err)
	}
	d, err := os.MkdirTemp(testRoot.path, prefix)
	if err != nil {
		t.Fatal(err)
	}
	return d
}

var (
	procSetNamedSecurityInfoW = modadvapi32.NewProc("SetNamedSecurityInfoW")
)

// assertPrivateFile checks that a runtime file is owned by this account and carries only the
// inherited private DACL (Windows has no POSIX modes; os.Stat reports 0666 for writable files).
func assertPrivateFile(t *testing.T, path string) {
	t.Helper()
	if _, err := os.Stat(path); err != nil {
		t.Fatal(err)
	}
	if !ownedByCurrentUser(nil, path) {
		t.Fatalf("%s is not owned by this account", path)
	}
	if err := checkPrivateSecurity(path); err != nil {
		t.Fatalf("%s is not private: %v", path, err)
	}
}

// makeDirUntrusted grants Authenticated Users read access so checkPrivateDir must refuse the directory.
func makeDirUntrusted(t *testing.T, dir string) {
	t.Helper()
	user, _, err := currentIdentity()
	if err != nil {
		t.Fatal(err)
	}
	sddl, _ := syscall.UTF16PtrFromString("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + user + ")(A;OICI;GR;;;AU)")
	var sd *byte
	var size uint32
	if r, _, e := procConvertStringSDToSD.Call(uintptr(unsafe.Pointer(sddl)), sddlRevision1,
		uintptr(unsafe.Pointer(&sd)), uintptr(unsafe.Pointer(&size))); r == 0 {
		t.Fatalf("security descriptor: %v", e)
	}
	defer syscall.LocalFree(syscall.Handle(uintptr(unsafe.Pointer(sd))))
	var dacl *byte
	var present, defaulted int32
	if r, _, _ := modadvapi32.NewProc("GetSecurityDescriptorDacl").Call(uintptr(unsafe.Pointer(sd)),
		uintptr(unsafe.Pointer(&present)), uintptr(unsafe.Pointer(&dacl)), uintptr(unsafe.Pointer(&defaulted))); r == 0 {
		t.Fatal("GetSecurityDescriptorDacl failed")
	}
	p, _ := syscall.UTF16PtrFromString(dir)
	// DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION
	if r, _, _ := procSetNamedSecurityInfoW.Call(uintptr(unsafe.Pointer(p)), seFileObject, 0x4|0x80000000,
		0, 0, uintptr(unsafe.Pointer(dacl)), 0); r != 0 {
		t.Fatalf("SetNamedSecurityInfoW: error %d", r)
	}
}

// createSocketNode creates a socket node at path (an AF_UNIX listener that is never accepted on).
func createSocketNode(t *testing.T, path string) func() {
	t.Helper()
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	ln.SetUnlinkOnClose(false)
	return func() { _ = ln.Close() }
}

type streamGuest struct {
	ln     *net.UnixListener
	c      *net.UnixConn
	r      *bufio.Reader
	broker string
}

// dialDataStream opens a client stream to the broker and performs the hello exchange.
func dialDataStream(t *testing.T, broker, socketPath string) (*net.UnixConn, *bufio.Reader, dataHelloReply) {
	t.Helper()
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: broker, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	_ = c.SetDeadline(time.Now().Add(5 * time.Second))
	hello, _ := json.Marshal(dataHello{Kind: "client", Socket: socketPath})
	if _, err := c.Write(append(hello, '\n')); err != nil {
		t.Fatal(err)
	}
	r := bufio.NewReader(c)
	line, err := r.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var reply dataHelloReply
	if err := json.Unmarshal(line, &reply); err != nil {
		t.Fatalf("hello reply %q: %v", line, err)
	}
	_ = c.SetDeadline(time.Time{})
	return c, r, reply
}

func newGuestTransport(t *testing.T, n *node, path string) guestTransport {
	t.Helper()
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	c, r, reply := dialDataStream(t, n.ready.AutoTransport.DatagramSocket, path)
	if !reply.OK {
		t.Fatalf("data hello for %s refused: %s", path, reply.Error)
	}
	return &streamGuest{ln: ln, c: c, r: r, broker: n.ready.AutoTransport.DatagramSocket}
}

func (g *streamGuest) sendRaw(frame []byte) error {
	_ = g.c.SetWriteDeadline(time.Now().Add(2 * time.Second))
	_, err := g.c.Write(frame)
	return err
}

func (g *streamGuest) recv(timeout time.Duration) ([]byte, string, error) {
	_ = g.c.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, MaxFrame)
	if _, err := io.ReadFull(g.r, buf[:HeaderLen]); err != nil {
		return nil, "", err
	}
	n := binary.BigEndian.Uint32(buf[20:24])
	if n > MaxPayload {
		return nil, "", fmt.Errorf("stream carries an oversized record (%d)", n)
	}
	if _, err := io.ReadFull(g.r, buf[HeaderLen:HeaderLen+int(n)]); err != nil {
		return nil, "", err
	}
	return buf[:HeaderLen+int(n)], g.broker, nil
}

func (g *streamGuest) close() {
	_ = g.c.Close()
	_ = g.ln.Close()
}

// blackHoleEndpoint: Windows resets connections to a full accept queue instead of dropping
// them, so the Linux "silent drop" cannot be emulated with a listener. The V1 budget test still
// runs with its pretending (accepting but silent) endpoints; this helper reports the gap.
func blackHoleEndpoint(t *testing.T) netip.AddrPort {
	t.Helper()
	t.Skip("Windows TCP resets a full accept queue; silent-drop firewall emulation is Linux-only")
	return netip.AddrPort{}
}

// processGone reports that pid no longer runs.
func processGone(pid int) bool { return !processAlive(pid) }

// killHard terminates a child without any chance to clean up.
func killHard(p *os.Process) error { return p.Kill() }

const nonexistentDir = `C:\nonexistent-eln`
