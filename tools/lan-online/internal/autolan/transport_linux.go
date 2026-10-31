//go:build linux

package autolan

import (
	"errors"
	"net"
	"os"
	"syscall"
)

// unixgramTransport is the frozen Linux same-host transport: one AF_UNIX datagram socket
// (data.sock) receives frames from guest sockets and same-host peer brokers, each identified by
// the bound pathname of the sending socket.
type unixgramTransport struct {
	data    *net.UnixConn
	dataRaw syscall.RawConn
}

func newDataTransport() dataTransport { return &unixgramTransport{} }

func (t *unixgramTransport) listen(b *Broker) error {
	removeStaleSocket(b.dataPath)
	var err error
	if t.data, err = net.ListenUnixgram("unixgram", &net.UnixAddr{Name: b.dataPath, Net: "unixgram"}); err != nil {
		return statusErr("listen_failed", "datagram socket: %v", err)
	}
	_ = os.Chmod(b.dataPath, 0o600)
	_ = t.data.SetReadBuffer(4 << 20)
	if t.dataRaw, err = t.data.SyscallConn(); err != nil {
		return statusErr("listen_failed", "datagram socket: %v", err)
	}
	return nil
}

func (t *unixgramTransport) serve(b *Broker) {
	buf := make([]byte, MaxFrame+1)
	for {
		n, from, err := t.data.ReadFromUnix(buf)
		if err != nil {
			if b.ctx.Err() != nil {
				return
			}
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if from == nil || from.Name == "" {
			b.stats.unknownSender.Add(1)
			continue
		}
		b.handleUnix(from.Name, buf[:n])
	}
}

// hasClient is always true: a datagram client needs no connection before binding.
func (t *unixgramTransport) hasClient(string) bool { return true }

func (t *unixgramTransport) sendClient(path string, pkt []byte) error { return t.sendUnix(path, pkt) }

func (t *unixgramTransport) sendPeer(_ *Broker, path string, pkt []byte) error {
	return t.sendUnix(path, pkt)
}

// sendUnix never blocks: a full or missing receiver drops the datagram.
func (t *unixgramTransport) sendUnix(path string, pkt []byte) error {
	var serr error
	err := t.dataRaw.Write(func(fd uintptr) bool {
		serr = syscall.Sendto(int(fd), pkt, syscall.MSG_DONTWAIT, &syscall.SockaddrUnix{Name: path})
		return true
	})
	if err == nil {
		err = serr
	}
	return err
}

func (t *unixgramTransport) close() {
	if t.data != nil {
		_ = t.data.Close()
	}
}

// clientGone reports a send failure that means the guest socket no longer exists.
func clientGone(err error) bool {
	return errors.Is(err, syscall.ECONNREFUSED) || errors.Is(err, syscall.ENOENT)
}
