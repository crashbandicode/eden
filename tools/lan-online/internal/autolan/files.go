package autolan

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// writeFileAtomic writes data to path through an exclusive temporary file in the
// same directory and a rename, so readers never observe a partial file.
func writeFileAtomic(path string, data []byte) error {
	var suffix [8]byte
	if _, err := rand.Read(suffix[:]); err != nil {
		return err
	}
	tmp := path + ".tmp-" + hex.EncodeToString(suffix[:])
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_EXCL|noFollow, 0o600)
	if err != nil {
		return err
	}
	_, werr := f.Write(data)
	serr := f.Sync()
	cerr := f.Close()
	if err := errors.Join(werr, serr, cerr); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

func writeJSONAtomic(path string, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return writeFileAtomic(path, append(b, '\n'))
}

// writeErrorStatus records a fatal condition. Messages are bounded and carry no
// payload or credential material.
func writeErrorStatus(dir string, se *StatusError) error {
	msg := se.Message
	if len(msg) > 512 {
		msg = msg[:512]
	}
	return writeJSONAtomic(filepath.Join(dir, errorFileName), map[string]string{
		"status": "error", "code": se.Code, "message": msg,
	})
}

// sharedDirectory returns (creating if needed) the per-user discovery directory.
// An existing directory must be a real private directory owned by us (checkPrivateDir).
func sharedDirectory(override string) (string, error) {
	dir := override
	if dir == "" {
		dir = defaultSharedDir()
	}
	dir = filepath.Clean(dir)
	if err := mkdirPrivate(dir); err != nil {
		return "", err
	}
	if err := checkPrivateDir(dir); err != nil {
		return "", fmt.Errorf("shared directory %s: %w", dir, err)
	}
	brokers := filepath.Join(dir, "brokers")
	if err := mkdirPrivate(brokers); err != nil {
		return "", err
	}
	if err := checkPrivateDir(brokers); err != nil {
		return "", fmt.Errorf("shared directory %s: %w", brokers, err)
	}
	return dir, nil
}

// sameHostKey identifies "same host, same user, same shared directory": the
// only scope in which another broker's same-host data endpoint is reachable.
func sameHostKey(sharedDir string) string {
	sum := sha256.Sum256([]byte(hostBootKey() + "\x00" + currentUserID() + "\x00" + sharedDir))
	return hex.EncodeToString(sum[:16])
}

// lanEndpoint selects the broker's private IPv4 LAN address. An explicit
// interface must exist, be up and carry a private IPv4 address. Without one,
// the interface holding the IPv4 default route wins, then the first candidate
// by name. No candidate means local-only operation (same-host play still works).
func lanEndpoint(ifname string) (netip.Addr, *net.Interface, error) {
	if ifname != "" {
		ifi, err := net.InterfaceByName(ifname)
		if err != nil || ifi.Flags&net.FlagUp == 0 {
			return netip.Addr{}, nil, statusErr("interface_unusable", "interface %q is missing or down", ifname)
		}
		if a, ok := privateIPv4Of(ifi); ok {
			return a, ifi, nil
		}
		return netip.Addr{}, nil, statusErr("interface_unusable", "interface %q has no private IPv4 address", ifname)
	}
	ifaces, err := net.Interfaces()
	if err != nil {
		return netip.Addr{}, nil, nil
	}
	sort.Slice(ifaces, func(i, j int) bool { return ifaces[i].Name < ifaces[j].Name })
	def := defaultRouteInterface()
	var first *net.Interface
	var firstAddr netip.Addr
	for i := range ifaces {
		ifi := &ifaces[i]
		if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagLoopback != 0 {
			continue
		}
		a, ok := privateIPv4Of(ifi)
		if !ok {
			continue
		}
		if ifi.Name == def {
			return a, ifi, nil
		}
		if first == nil {
			first, firstAddr = ifi, a
		}
	}
	if first != nil {
		return firstAddr, first, nil
	}
	return netip.Addr{}, nil, nil
}

func privateIPv4Of(ifi *net.Interface) (netip.Addr, bool) {
	addrs, err := ifi.Addrs()
	if err != nil {
		return netip.Addr{}, false
	}
	for _, a := range addrs {
		ipn, ok := a.(*net.IPNet)
		if !ok {
			continue
		}
		ip, ok := netip.AddrFromSlice(ipn.IP)
		if !ok {
			continue
		}
		ip = ip.Unmap()
		if isLANAddress(ip) {
			return ip, true
		}
	}
	return netip.Addr{}, false
}

// isLANAddress accepts RFC 1918 unicast IPv4 outside the runtime's own virtual
// ranges.
func isLANAddress(ip netip.Addr) bool {
	return ip.Is4() && ip.IsPrivate() && !instanceNet.Contains(ip) && !serviceNet.Contains(ip)
}

func randomHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func isInstanceID(s string) bool {
	if len(s) != 32 {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil && strings.ToLower(s) == s
}
