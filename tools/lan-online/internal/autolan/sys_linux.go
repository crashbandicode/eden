//go:build linux

package autolan

import (
	"bufio"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

// Linux platform edges of the runtime: Unix credentials, /proc facts and the 0700-directory
// ownership model. The behaviour here is the frozen v6 behaviour; sys_windows.go supplies the
// Windows equivalents of the same functions.

const (
	noFollow    = syscall.O_NOFOLLOW // opening a symlink in place of a private file fails
	useStopFile = false              // Linux parents stop the runtime with SIGTERM
)

// currentUserID names the account for per-user directory names (the numeric uid).
func currentUserID() string { return strconv.Itoa(os.Getuid()) }

// ownedByCurrentUser reports whether the node described by fi belongs to the current user.
func ownedByCurrentUser(fi os.FileInfo, _ string) bool {
	st, ok := fi.Sys().(*syscall.Stat_t)
	return ok && int(st.Uid) == os.Getuid()
}

// mkdirPrivate creates dir for the current user only; an existing directory is left alone and
// verified by the caller.
func mkdirPrivate(dir string) error {
	if err := os.Mkdir(dir, 0o700); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	return nil
}

// normalizeDir is the identity on Linux: checkPrivateDir requires an already clean path.
func normalizeDir(dir string) string { return dir }

// checkPrivateDir verifies an absolute, clean, non-symlink directory owned by us with mode
// exactly 0700.
func checkPrivateDir(dir string) error {
	if !filepath.IsAbs(dir) || filepath.Clean(dir) != dir {
		return errors.New("must be an absolute clean path")
	}
	fi, err := os.Lstat(dir)
	if err != nil {
		return err
	}
	if !fi.IsDir() || fi.Mode()&os.ModeSymlink != 0 {
		return errors.New("must be a real directory")
	}
	st, ok := fi.Sys().(*syscall.Stat_t)
	if !ok || int(st.Uid) != os.Getuid() {
		return errors.New("must be owned by the current user")
	}
	if fi.Mode().Perm() != 0o700 {
		return fmt.Errorf("mode is %04o, need 0700", fi.Mode().Perm())
	}
	return nil
}

func checkOwnedDir(dir string) error {
	fi, err := os.Lstat(dir)
	if err != nil {
		return err
	}
	st, ok := fi.Sys().(*syscall.Stat_t)
	if !fi.IsDir() || !ok || int(st.Uid) != os.Getuid() {
		return errors.New("not an owned directory")
	}
	return nil
}

// defaultSharedDir is the per-user discovery directory: under XDG_RUNTIME_DIR when that is an
// owned directory, otherwise a uid-suffixed directory below the temporary directory.
func defaultSharedDir() string {
	if x := os.Getenv("XDG_RUNTIME_DIR"); x != "" && filepath.IsAbs(x) && checkOwnedDir(x) == nil {
		return filepath.Join(x, "eden-nsolan-auto-v1")
	}
	return filepath.Join(os.TempDir(), "eden-nsolan-auto-v1-"+strconv.Itoa(os.Getuid()))
}

// hostBootKey identifies this boot of this host (the kernel boot id, else the host name).
func hostBootKey() string {
	boot, err := os.ReadFile("/proc/sys/kernel/random/boot_id")
	if err != nil {
		host, _ := os.Hostname()
		boot = []byte(host)
	}
	return strings.TrimSpace(string(boot))
}

func defaultRouteInterface() string {
	f, err := os.Open("/proc/net/route")
	if err != nil {
		return ""
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) < 8 || fields[1] != "00000000" || fields[7] != "00000000" {
			continue
		}
		flags, err := strconv.ParseUint(fields[3], 16, 32)
		if err == nil && flags&0x1 != 0 { // RTF_UP
			return fields[0]
		}
	}
	return ""
}

// unixMaxDgramQlen reads net.unix.max_dgram_qlen (0 when unavailable). It is
// reported and warned about only; the runtime never changes sysctls.
func unixMaxDgramQlen() int {
	b, err := os.ReadFile("/proc/sys/net/unix/max_dgram_qlen")
	if err != nil {
		return 0
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil || n < 0 {
		return 0
	}
	return n
}

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	err := syscall.Kill(pid, 0)
	return err == nil || errors.Is(err, syscall.EPERM)
}

// parentMonitor reports whether the launching process is still our parent.
type parentMonitor struct{ pid int }

func newParentMonitor(pid int) (*parentMonitor, error) { return &parentMonitor{pid: pid}, nil }

func (m *parentMonitor) alive() bool { return os.Getppid() == m.pid }

func (m *parentMonitor) close() {}

func peerCred(c *net.UnixConn) (*syscall.Ucred, error) {
	rc, err := c.SyscallConn()
	if err != nil {
		return nil, err
	}
	var cred *syscall.Ucred
	var cerr error
	if err := rc.Control(func(fd uintptr) {
		cred, cerr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	}); err != nil {
		return nil, err
	}
	return cred, cerr
}

// authenticateControl admits a control connection from our own uid and the configured process.
func authenticateControl(c *net.UnixConn, peerPID int) bool {
	cred, err := peerCred(c)
	return err == nil && int(cred.Uid) == os.Getuid() && int(cred.Pid) == peerPID
}

func setMulticastSendOptions(rc syscall.RawConn, src [4]byte) {
	_ = rc.Control(func(fd uintptr) {
		_ = syscall.SetsockoptInet4Addr(int(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, src)
		_ = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_TTL, 1)
		_ = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1)
	})
}

// udpSend never blocks: a full socket buffer drops the datagram.
func udpSend(_ *net.UDPConn, rc syscall.RawConn, to syscall.Sockaddr, pkt []byte) error {
	var serr error
	err := rc.Write(func(fd uintptr) bool {
		serr = syscall.Sendto(int(fd), pkt, syscall.MSG_DONTWAIT, to)
		return true
	})
	if err == nil {
		err = serr
	}
	return err
}

func ownedSocket(path string) bool {
	fi, err := os.Lstat(path)
	if err != nil || fi.Mode()&os.ModeSocket == 0 {
		return false
	}
	st, ok := fi.Sys().(*syscall.Stat_t)
	return ok && int(st.Uid) == os.Getuid()
}
