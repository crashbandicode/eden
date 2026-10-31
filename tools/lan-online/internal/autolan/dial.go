package autolan

import (
	"context"
	"crypto/tls"
	"errors"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The automatic runtime's only outbound connections: to the elected
// coordinator, either over a same-user Unix socket in the private shared
// directory or over TCP to a private (RFC 1918) LAN address. Loopback TCP is
// permitted only under the unexported test option. Every other destination is
// refused here at runtime. internal/guard confines all outbound dials in the
// backend to dialCoordinator, behind this check.

var errDestinationRefused = errors.New("refused: not a private coordinator endpoint")

func coordinatorDestinationAllowed(network, address, sharedDir string, allowLoopback bool) bool {
	switch network {
	case "unix":
		base := filepath.Base(address)
		return filepath.IsAbs(address) && filepath.Clean(address) == address &&
			filepath.Dir(address) == sharedDir && strings.HasPrefix(base, "coord-") && strings.HasSuffix(base, ".sock")
	case "tcp4":
		ap, err := netip.ParseAddrPort(address)
		if err != nil || ap.Port() == 0 {
			return false
		}
		a := ap.Addr().Unmap()
		return isLANAddress(a) || (allowLoopback && a.Is4() && a.IsLoopback())
	}
	return false
}

// controlTLSClient makes a cross-host dial TLS-only: the coordinator must
// present exactly the certificate whose fingerprint was advertised with the
// endpoint. There is no plaintext fallback.
type controlTLSClient struct {
	pin string
}

// dialCoordinator connects within the caller's absolute deadline: the TCP or
// Unix connect is additionally capped at lanDialTimeout, and the TLS handshake
// must finish by the deadline.
func dialCoordinator(network, address, sharedDir string, local netip.Addr, allowLoopback bool, tlsc *controlTLSClient, deadline time.Time) (net.Conn, error) {
	if !coordinatorDestinationAllowed(network, address, sharedDir, allowLoopback) {
		return nil, errDestinationRefused
	}
	timeout := min(time.Until(deadline), lanDialTimeout)
	if timeout <= 0 {
		return nil, os.ErrDeadlineExceeded
	}
	if network == "unix" {
		return net.DialTimeout("unix", address, timeout) // same-host IPC stays Unix
	}
	var cfg *tls.Config
	if tlsc != nil {
		var err error
		if cfg, err = pinnedClientConfig(tlsc.pin); err != nil {
			return nil, err // no valid fingerprint: no connection at all
		}
	}
	d := net.Dialer{Timeout: timeout}
	if local.IsValid() {
		d.LocalAddr = net.TCPAddrFromAddrPort(netip.AddrPortFrom(local, 0))
	}
	conn, err := d.Dial("tcp4", address)
	if err != nil || cfg == nil {
		return conn, err
	}
	tc := tls.Client(conn, cfg)
	ctx, cancel := context.WithDeadline(context.Background(), deadline)
	defer cancel()
	if err := tc.HandshakeContext(ctx); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return tc, nil
}

// peerDestinationAllowed admits only a same-host peer broker's data endpoint: an absolute,
// clean AF_UNIX path named data.sock whose socket node this account owns. Callers pass only the
// data socket named by a live record in our own private shared directory.
func peerDestinationAllowed(address string) bool {
	return validSocketPath(address) && filepath.Base(address) == dataSocketName && ownedSocket(address)
}

// dialLocalPeer connects to a same-host peer broker's data endpoint. It is used by the Windows
// stream transport (Linux delivers same-host frames as datagrams without a connection).
func dialLocalPeer(address string, deadline time.Time) (net.Conn, error) {
	if !peerDestinationAllowed(address) {
		return nil, errDestinationRefused
	}
	timeout := min(time.Until(deadline), lanDialTimeout)
	if timeout <= 0 {
		return nil, os.ErrDeadlineExceeded
	}
	return net.DialTimeout("unix", address, timeout)
}
