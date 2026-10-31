package autolan

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

const (
	// v3: explicit probe exchange, member-token reconnects and control-TLS fields.
	// (v2 removed same-host data from the wire.) Brokers of other versions ignore
	// each other; every host must run the same tested build.
	protocolVersion = 4 // v4 adds profile-owned fighter report/read exchanges
	maxControlLine  = 4096
	maxCoordLine    = 64 << 10 // member snapshots of a full LAN fit comfortably
)

var errLineTooLong = errors.New("line too long")

// readLine reads one '\n'-terminated line of at most max bytes (excluding the
// terminator). A longer line is an error; the caller closes the connection.
func readLine(r *bufio.Reader, max int) ([]byte, error) {
	var line []byte
	for {
		chunk, err := r.ReadSlice('\n')
		if len(line)+len(chunk) > max+1 {
			return nil, errLineTooLong
		}
		line = append(line, chunk...)
		if err == nil {
			return line[:len(line)-1], nil
		}
		if !errors.Is(err, bufio.ErrBufferFull) {
			if errors.Is(err, io.EOF) && len(line) > 0 {
				return nil, io.ErrUnexpectedEOF
			}
			return nil, err
		}
	}
}

// readLineUnbuffered reads a short line byte by byte so no byte after the
// newline is consumed (the stream continues with raw TLS).
func readLineUnbuffered(c net.Conn, max int) ([]byte, error) {
	var line []byte
	var b [1]byte
	for {
		if _, err := io.ReadFull(c, b[:]); err != nil {
			return nil, err
		}
		if b[0] == '\n' {
			return line, nil
		}
		if len(line) >= max {
			return nil, errLineTooLong
		}
		line = append(line, b[0])
	}
}

// lineWriter serializes JSON lines onto one connection. With conn set, every
// write is bounded so a stalled peer cannot block its writers indefinitely.
type lineWriter struct {
	mu   sync.Mutex
	w    io.Writer
	conn net.Conn
}

func (l *lineWriter) send(v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.conn != nil {
		_ = l.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	}
	_, err = l.w.Write(append(b, '\n'))
	return err
}

// strictUnmarshal decodes exactly one JSON object, rejecting unknown fields.
func strictUnmarshal(b []byte, v any) error {
	d := json.NewDecoder(bytes.NewReader(b))
	d.DisallowUnknownFields()
	if err := d.Decode(v); err != nil {
		return err
	}
	if d.More() {
		return errors.New("trailing data")
	}
	return nil
}

// Lease is an instance's coordinator-assigned identity.
type Lease struct {
	PID uint32 `json:"pid"`
	VIP string `json:"vip"`
}

// MemberInfo is one instance as the coordinator publishes it. It deliberately
// has no same-host fields: whether a peer shares this host (and its socket) is
// decided only from our own shared directory, never by the coordinator.
type MemberInfo struct {
	Instance string `json:"instance"`
	PID      uint32 `json:"pid"`
	VIP      string `json:"vip"`
	LANIP    string `json:"lan_ip,omitempty"`
	UDPPort  uint16 `json:"udp_port,omitempty"`
}

// coordPreface is the first line on every coordinator connection.
type coordPreface struct {
	V        int    `json:"v"`
	Kind     string `json:"kind"` // "member", "stream" or "probe"
	Title    string `json:"title"`
	Instance string `json:"instance"`
	// member
	Lease   *Lease `json:"lease,omitempty"`
	LANIP   string `json:"lan_ip,omitempty"`
	UDPPort uint16 `json:"udp_port,omitempty"`
	TLS     bool   `json:"tls,omitempty"` // control-TLS mode; must equal the coordinator's
	// member: the current member token when reconnecting to the same
	// coordinator lifetime; stream: the member token authorising the stream.
	Token   string `json:"token,omitempty"`
	Service string `json:"service,omitempty"`
}

type coordReply struct {
	OK    bool   `json:"ok"`
	Error string `json:"error,omitempty"`
	PID   uint32 `json:"pid,omitempty"`
	VIP   string `json:"vip,omitempty"`
	Token string `json:"token,omitempty"`
	Epoch uint64 `json:"epoch,omitempty"`
}

// coordEvent is every subsequent line in either direction on a member link.
type coordEvent struct {
	Op       string       `json:"op"`
	Members  []MemberInfo `json:"members,omitempty"`
	Member   *MemberInfo  `json:"member,omitempty"`
	Instance string       `json:"instance,omitempty"`
	Port     uint16       `json:"port,omitempty"`
	Seq      uint64       `json:"seq,omitempty"`
	Method   uint32       `json:"method,omitempty"`
	Body     []byte       `json:"body,omitempty"`
	Result   uint32       `json:"result,omitempty"`
}
