//go:build windows

package autolan

import (
	"errors"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"time"
	"unsafe"
)

// Windows platform edges. Measured on Windows 11 26200 (WINDOWS_PROGRESS.md): AF_UNIX supports
// SOCK_STREAM only, SIO_AF_UNIX_GETPEERPID reports the peer process of an AF_UNIX connection, and
// a private directory is a protected DACL (this account and SYSTEM) rather than a 0700 mode.

const (
	noFollow    = 0    // no O_NOFOLLOW; Lstat checks and O_EXCL on random names stand in
	useStopFile = true // QProcess::terminate cannot signal a console-less child; a marker file does

	sioAFUnixGetPeerPID            = 0x58000100 // _WSAIOR(IOC_VENDOR, 256), afunix.h
	processQueryLimitedInformation = 0x1000
	stillActive                    = 259
	keyWow64_64Key                 = 0x0100
)

var (
	modiphlpapi            = syscall.NewLazyDLL("iphlpapi.dll")
	procGetBestInterfaceEx = modiphlpapi.NewProc("GetBestInterfaceEx")
)

// currentUserID names the account for per-user directory names (the string SID).
func currentUserID() string {
	user, _, err := currentIdentity()
	if err != nil {
		return "unknown-account"
	}
	return user
}

// ownedByCurrentUser checks the account-private descriptor, including when an elevated
// creator assigned Administrators as owner and the reader is now unelevated.
func ownedByCurrentUser(_ os.FileInfo, path string) bool {
	return checkPrivateSecurity(path) == nil
}

// mkdirPrivate creates dir with a protected DACL for this account and SYSTEM only.
func mkdirPrivate(dir string) error { return createPrivateDirectory(dir) }

// normalizeDir accepts the forward slashes a Qt launcher passes and returns the native clean form.
func normalizeDir(dir string) string {
	native := filepath.FromSlash(dir)
	if filepath.IsAbs(native) {
		return filepath.Clean(native)
	}
	return dir
}

// checkPrivateDir verifies an absolute, clean, non-symlink directory owned by this account whose
// access-control entries grant access only to this account and SYSTEM.
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
	return checkPrivateSecurity(dir)
}

// defaultSharedDir is a short per-user directory: coord-<32 hex>.sock must still fit sun_path.
func defaultSharedDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" || !filepath.IsAbs(base) {
		base = os.TempDir()
	}
	return filepath.Join(base, "eden-lan-shared-v1")
}

// hostBootKey identifies this installation (the machine GUID, else the host name). Liveness of
// records comes from process checks and timestamps, so a boot-specific value is not required.
func hostBootKey() string {
	if guid, err := machineGUID(); err == nil && guid != "" {
		return guid
	}
	host, _ := os.Hostname()
	return host
}

func machineGUID() (string, error) {
	sub, err := syscall.UTF16PtrFromString(`SOFTWARE\Microsoft\Cryptography`)
	if err != nil {
		return "", err
	}
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_LOCAL_MACHINE, sub, 0, syscall.KEY_READ|keyWow64_64Key, &key); err != nil {
		return "", err
	}
	defer syscall.RegCloseKey(key)
	name, err := syscall.UTF16PtrFromString("MachineGuid")
	if err != nil {
		return "", err
	}
	var typ uint32
	buf := make([]uint16, 128)
	n := uint32(len(buf) * 2)
	if err := syscall.RegQueryValueEx(key, name, nil, &typ, (*byte)(unsafe.Pointer(&buf[0])), &n); err != nil {
		return "", err
	}
	return syscall.UTF16ToString(buf[:n/2]), nil
}

// defaultRouteInterface asks the routing table which interface would carry traffic to a
// documentation address. This is a table lookup only; no packet is sent.
func defaultRouteInterface() string {
	sa := syscall.RawSockaddrInet4{Family: syscall.AF_INET, Addr: [4]byte{192, 0, 2, 1}}
	var index uint32
	r, _, _ := procGetBestInterfaceEx.Call(uintptr(unsafe.Pointer(&sa)), uintptr(unsafe.Pointer(&index)))
	if r != 0 {
		return ""
	}
	ifi, err := net.InterfaceByIndex(int(index))
	if err != nil {
		return ""
	}
	return ifi.Name
}

// unixMaxDgramQlen has no Windows equivalent (the same-host transport is a stream).
func unixMaxDgramQlen() int { return 0 }

func processAlive(pid int) bool {
	if pid <= 0 {
		return false
	}
	h, err := syscall.OpenProcess(processQueryLimitedInformation, false, uint32(pid))
	if err != nil {
		return errors.Is(err, syscall.ERROR_ACCESS_DENIED) // exists, owned by another account
	}
	defer syscall.CloseHandle(h)
	var code uint32
	return syscall.GetExitCodeProcess(h, &code) == nil && code == stillActive
}

// parentMonitor holds a handle on the launching process: Windows never re-parents a child, so
// the parent id alone cannot reveal that the parent exited.
type parentMonitor struct{ h syscall.Handle }

func newParentMonitor(pid int) (*parentMonitor, error) {
	h, err := syscall.OpenProcess(syscall.SYNCHRONIZE|processQueryLimitedInformation, false, uint32(pid))
	if err != nil {
		return nil, err
	}
	return &parentMonitor{h: h}, nil
}

func (m *parentMonitor) alive() bool {
	r, err := syscall.WaitForSingleObject(m.h, 0)
	return err == nil && r == syscall.WAIT_TIMEOUT
}

func (m *parentMonitor) close() { _ = syscall.CloseHandle(m.h) }

// unixPeerPID returns the process at the other end of an AF_UNIX stream connection.
func unixPeerPID(c *net.UnixConn) (int, error) {
	rc, err := c.SyscallConn()
	if err != nil {
		return 0, err
	}
	var pid, got uint32
	var ierr error
	if err := rc.Control(func(fd uintptr) {
		ierr = syscall.WSAIoctl(syscall.Handle(fd), sioAFUnixGetPeerPID, nil, 0,
			(*byte)(unsafe.Pointer(&pid)), 4, &got, nil, 0)
	}); err != nil {
		return 0, err
	}
	if ierr != nil {
		return 0, ierr
	}
	return int(pid), nil
}

// authenticateControl admits a control connection from the configured process. The private
// directory's DACL already limits who can reach the socket to this account.
func authenticateControl(c *net.UnixConn, peerPID int) bool {
	pid, err := unixPeerPID(c)
	return err == nil && pid == peerPID
}

func setMulticastSendOptions(rc syscall.RawConn, src [4]byte) {
	_ = rc.Control(func(fd uintptr) {
		h := syscall.Handle(fd)
		_ = syscall.SetsockoptInet4Addr(h, syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, src)
		_ = syscall.SetsockoptInt(h, syscall.IPPROTO_IP, syscall.IP_MULTICAST_TTL, 1)
		_ = syscall.SetsockoptInt(h, syscall.IPPROTO_IP, syscall.IP_MULTICAST_LOOP, 1)
	})
}

// udpSend has no MSG_DONTWAIT on Winsock; a short write deadline bounds the rare full buffer.
func udpSend(conn *net.UDPConn, _ syscall.RawConn, to syscall.Sockaddr, pkt []byte) error {
	sa, ok := to.(*syscall.SockaddrInet4)
	if !ok {
		return errors.New("udp send: not an IPv4 destination")
	}
	_ = conn.SetWriteDeadline(time.Now().Add(50 * time.Millisecond))
	_, err := conn.WriteToUDP(pkt, &net.UDPAddr{IP: net.IPv4(sa.Addr[0], sa.Addr[1], sa.Addr[2], sa.Addr[3]), Port: sa.Port})
	return err
}

func ownedSocket(path string) bool {
	fi, err := os.Lstat(path)
	return err == nil && fi.Mode()&os.ModeSocket != 0 && ownedByCurrentUser(fi, path)
}
