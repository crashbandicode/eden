//go:build windows

package autolan

import (
	"bufio"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

// streamTransport is the Windows same-host transport. Windows AF_UNIX has no datagram sockets
// (measured: socket(AF_UNIX, SOCK_DGRAM) fails with WSAEAFNOSUPPORT), so data.sock is an AF_UNIX
// stream listener. Every guest socket and every same-host peer broker holds one connection: a
// newline JSON hello identifies it, then consecutive ELN1 frames follow, the 24-byte header
// carrying each record's length. Senders are identified by their connection and authenticated
// with the peer process id (SIO_AF_UNIX_GETPEERPID): a guest connection must come from the
// parent Eden process and name a socket node it owns in the runtime directory; a peer connection
// must come from the process recorded for that instance in our private shared directory.
//
// Outbound records go through a bounded per-connection queue drained by one writer goroutine.
// A receiver that stops draining fills its queue and then loses the newest records, exactly like
// a full datagram receive buffer on Linux; it is never disconnected for being slow, and the
// broker's routing goroutines never block on it (review N1).
const (
	dataHelloTimeout = 2 * time.Second
	peerDialTimeout  = 250 * time.Millisecond
	dataReadBuffer   = 64 << 10
	maxQueuedRecords = 256     // per connection, like a 4 MiB datagram receive buffer
	maxQueuedBytes   = 2 << 20 // per connection
)

type dataHello struct {
	Kind     string `json:"hello"`
	Socket   string `json:"socket,omitempty"`   // client: its own socket node in the runtime dir
	Instance string `json:"instance,omitempty"` // peer: the connecting broker's instance id
}

type dataHelloReply struct {
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
}

var (
	errNoClient = errors.New("no stream for this socket")
	// errFrameDropped: the receiver's queue is full, so this record is dropped like a datagram.
	errFrameDropped = errors.New("frame dropped: receiver not draining")
)

// streamConn is one authenticated data connection with its bounded outbound queue.
type streamConn struct {
	c      *net.UnixConn
	mu     sync.Mutex
	queue  [][]byte
	bytes  int
	closed bool
	wake   chan struct{} // buffered(1): the writer sleeps here while the queue is empty
}

func newStreamConn(c *net.UnixConn) *streamConn {
	s := &streamConn{c: c, wake: make(chan struct{}, 1)}
	go s.writer()
	return s
}

// enqueue copies one record into the queue, or drops it when the queue is full.
func (s *streamConn) enqueue(pkt []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return net.ErrClosed
	}
	if len(s.queue) >= maxQueuedRecords || s.bytes+len(pkt) > maxQueuedBytes {
		return errFrameDropped
	}
	s.queue = append(s.queue, append([]byte(nil), pkt...))
	s.bytes += len(pkt)
	select {
	case s.wake <- struct{}{}:
	default:
	}
	return nil
}

// writer sends queued records in order. A blocked receiver only blocks this goroutine; a write
// error closes the connection (its reader then releases the guest's bindings).
func (s *streamConn) writer() {
	for {
		s.mu.Lock()
		for len(s.queue) == 0 && !s.closed {
			s.mu.Unlock()
			<-s.wake
			s.mu.Lock()
		}
		if s.closed {
			s.mu.Unlock()
			return
		}
		pkt := s.queue[0]
		s.queue[0] = nil // do not retain consumed frames in the backing array
		s.queue = s.queue[1:]
		s.bytes -= len(pkt)
		s.mu.Unlock()
		if _, err := s.c.Write(pkt); err != nil {
			s.close()
			return
		}
	}
}

func (s *streamConn) close() {
	s.mu.Lock()
	s.closed = true
	s.queue, s.bytes = nil, 0
	s.mu.Unlock()
	select {
	case s.wake <- struct{}{}:
	default:
	}
	_ = s.c.Close()
}

type streamTransport struct {
	ln       *net.UnixListener
	mu       sync.Mutex
	closed   bool
	clients  map[string]*streamConn // guest socket path -> its connection
	peersOut map[string]*streamConn // peer broker data.sock path -> our connection to it
}

func newDataTransport() dataTransport {
	return &streamTransport{clients: map[string]*streamConn{}, peersOut: map[string]*streamConn{}}
}

func (t *streamTransport) listen(b *Broker) error {
	removeStaleSocket(b.dataPath)
	ln, err := net.ListenUnix("unix", &net.UnixAddr{Name: b.dataPath, Net: "unix"})
	if err != nil {
		return statusErr("listen_failed", "datagram socket: %v", err)
	}
	ln.SetUnlinkOnClose(true)
	t.ln = ln
	return nil
}

func (t *streamTransport) serve(b *Broker) {
	for {
		c, err := t.ln.AcceptUnix()
		if err != nil {
			return
		}
		if !b.track(c) {
			_ = c.Close()
			return
		}
		b.goRun(func() {
			defer b.untrack(c)
			defer c.Close()
			t.handle(b, c)
		})
	}
}

// handle authenticates one accepted connection and then forwards its frames.
func (t *streamTransport) handle(b *Broker, c *net.UnixConn) {
	_ = c.SetReadDeadline(time.Now().Add(dataHelloTimeout))
	r := bufio.NewReaderSize(c, dataReadBuffer)
	line, err := readLine(r, maxControlLine)
	if err != nil {
		return
	}
	w := &lineWriter{w: c, conn: c}
	refuse := func(code string) { _ = w.send(dataHelloReply{Error: code}) }
	var hello dataHello
	if strictUnmarshal(line, &hello) != nil {
		refuse("bad_request")
		return
	}
	pid, err := unixPeerPID(c)
	if err != nil {
		refuse("forbidden")
		return
	}
	var sender string
	switch hello.Kind {
	case "client":
		if pid != b.opt.ControlPeerPID {
			refuse("forbidden")
			return
		}
		if code := b.clientPath(hello.Socket); code != "" {
			refuse(code)
			return
		}
		t.mu.Lock()
		if t.closed {
			t.mu.Unlock()
			return
		}
		if _, dup := t.clients[hello.Socket]; dup {
			t.mu.Unlock()
			refuse("already_connected")
			return
		}
		sc := newStreamConn(c)
		t.clients[hello.Socket] = sc
		t.mu.Unlock()
		sender = hello.Socket
		defer func() {
			t.mu.Lock()
			if t.clients[sender] == sc {
				delete(t.clients, sender)
			}
			t.mu.Unlock()
			sc.close()
			b.releaseClient(sender) // the guest socket is gone: free its port like a closed UDP socket
		}()
	case "peer":
		if !isInstanceID(hello.Instance) {
			refuse("bad_request") // never join an unvalidated string into a path (review N2)
			return
		}
		rec, ok := b.localPeerRecord(hello.Instance)
		if !ok || rec.OSPID != pid {
			refuse("forbidden")
			return
		}
		sender = rec.DataSocket
	default:
		refuse("bad_request")
		return
	}
	if w.send(dataHelloReply{OK: true}) != nil {
		return
	}
	// The hello reader and lineWriter each install a deadline. The persistent
	// data stream owns its bounded output queue, so neither handshake deadline
	// may expire a later gameplay write after an idle lobby.
	_ = c.SetDeadline(time.Time{})
	buf := make([]byte, MaxFrame)
	for {
		if _, err := io.ReadFull(r, buf[:HeaderLen]); err != nil {
			return
		}
		n := binary.BigEndian.Uint32(buf[20:24])
		if n > MaxPayload {
			b.stats.malformed.Add(1)
			return // a stream cannot resynchronise after a bad header
		}
		if _, err := io.ReadFull(r, buf[HeaderLen:HeaderLen+int(n)]); err != nil {
			return
		}
		b.handleUnix(sender, buf[:HeaderLen+int(n)])
	}
}

func (t *streamTransport) hasClient(path string) bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.clients[path] != nil
}

func (t *streamTransport) sendClient(path string, pkt []byte) error {
	t.mu.Lock()
	sc := t.clients[path]
	t.mu.Unlock()
	if sc == nil {
		return errNoClient
	}
	return sc.enqueue(pkt)
}

func (t *streamTransport) sendPeer(b *Broker, path string, pkt []byte) error {
	t.mu.Lock()
	sc := t.peersOut[path]
	closed := t.closed
	t.mu.Unlock()
	if closed {
		return net.ErrClosed
	}
	if sc == nil {
		c, err := t.connectPeer(b, path)
		if err != nil {
			return err
		}
		t.mu.Lock()
		existing := t.peersOut[path]
		if existing != nil || t.closed {
			t.mu.Unlock()
			_ = c.Close()
			if existing == nil {
				return net.ErrClosed
			}
			sc = existing
		} else {
			sc = newStreamConn(c)
			t.peersOut[path] = sc
			t.mu.Unlock()
		}
	}
	err := sc.enqueue(pkt)
	if errors.Is(err, net.ErrClosed) {
		// The writer closed it after a failure; forget it so the next record re-dials.
		t.mu.Lock()
		if t.peersOut[path] == sc {
			delete(t.peersOut, path)
		}
		t.mu.Unlock()
	}
	return err
}

// connectPeer opens our stream to a same-host peer broker and completes its hello exchange.
// Frames only flow outward on this connection; the peer reaches us through its own.
func (t *streamTransport) connectPeer(b *Broker, path string) (*net.UnixConn, error) {
	conn, err := dialLocalPeer(path, time.Now().Add(peerDialTimeout))
	if err != nil {
		return nil, err
	}
	c, ok := conn.(*net.UnixConn)
	if !ok {
		_ = conn.Close()
		return nil, errors.New("peer dial did not return a Unix connection")
	}
	_ = c.SetDeadline(time.Now().Add(dataHelloTimeout))
	w := &lineWriter{w: c, conn: c}
	if err := w.send(dataHello{Kind: "peer", Instance: b.instance}); err != nil {
		_ = c.Close()
		return nil, err
	}
	line, err := readLineUnbuffered(c, maxControlLine)
	if err != nil {
		_ = c.Close()
		return nil, err
	}
	var reply dataHelloReply
	if strictUnmarshal(line, &reply) != nil || !reply.OK {
		_ = c.Close()
		return nil, errors.New("peer refused the data stream: " + reply.Error)
	}
	_ = c.SetDeadline(time.Time{})
	return c, nil
}

func (t *streamTransport) close() {
	t.mu.Lock()
	t.closed = true
	clients, peers := t.clients, t.peersOut
	t.clients, t.peersOut = map[string]*streamConn{}, map[string]*streamConn{}
	t.mu.Unlock()
	if t.ln != nil {
		_ = t.ln.Close()
	}
	for _, sc := range clients {
		sc.close()
	}
	for _, sc := range peers {
		sc.close()
	}
}

// clientGone: a closed stream means the guest socket is unusable (its reader goroutine releases
// the bindings as well); a dropped record keeps the stream and its bindings.
func clientGone(err error) bool { return errors.Is(err, net.ErrClosed) || errors.Is(err, errNoClient) }
