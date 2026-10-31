// Package autolan is the automatic LAN runtime: one broker process per Eden
// instance that carries guest UDP over AF_UNIX datagrams, leases a virtual
// identity from an elected coordinator, and (when elected) hosts the NEX
// fixture endpoints for every instance on the LAN.
package autolan

import (
	"encoding/binary"
	"errors"
	"net/netip"
)

// ELN1 datagram frame: a 24-byte big-endian header followed by the payload.
//
//	[0:4]   "ELN1"
//	[4]     version 1
//	[5:8]   zero
//	[8:12]  source IPv4        [12:16] destination IPv4
//	[16:18] source port        [18:20] destination port
//	[20:24] payload length (must equal the remaining bytes, <= MaxPayload)
const (
	HeaderLen    = 24
	MaxPayload   = 65483 // HeaderLen+MaxPayload = 65507, the largest IPv4 UDP payload
	MaxFrame     = HeaderLen + MaxPayload
	frameVersion = 1
)

var frameMagic = [4]byte{'E', 'L', 'N', '1'}

var (
	errShortFrame    = errors.New("frame: shorter than header")
	errBadMagic      = errors.New("frame: bad magic")
	errBadVersion    = errors.New("frame: unsupported version")
	errReserved      = errors.New("frame: reserved bytes not zero")
	errLength        = errors.New("frame: length field does not match datagram")
	errPayloadTooBig = errors.New("frame: payload too large")
	errNotIPv4       = errors.New("frame: endpoint is not IPv4")
)

// Frame is a decoded datagram; Payload aliases the decoded buffer.
type Frame struct {
	Src, Dst netip.AddrPort
	Payload  []byte
}

// AppendFrame appends the encoded frame to dst.
func AppendFrame(dst []byte, src, to netip.AddrPort, payload []byte) ([]byte, error) {
	if len(payload) > MaxPayload {
		return dst, errPayloadTooBig
	}
	if !src.Addr().Is4() || !to.Addr().Is4() {
		return dst, errNotIPv4
	}
	var h [HeaderLen]byte
	copy(h[0:4], frameMagic[:])
	h[4] = frameVersion
	s4, d4 := src.Addr().As4(), to.Addr().As4()
	copy(h[8:12], s4[:])
	copy(h[12:16], d4[:])
	binary.BigEndian.PutUint16(h[16:18], src.Port())
	binary.BigEndian.PutUint16(h[18:20], to.Port())
	binary.BigEndian.PutUint32(h[20:24], uint32(len(payload)))
	dst = append(dst, h[:]...)
	return append(dst, payload...), nil
}

// DecodeFrame validates and decodes one complete datagram.
func DecodeFrame(b []byte) (Frame, error) {
	if len(b) < HeaderLen {
		return Frame{}, errShortFrame
	}
	if [4]byte(b[0:4]) != frameMagic {
		return Frame{}, errBadMagic
	}
	if b[4] != frameVersion {
		return Frame{}, errBadVersion
	}
	if b[5] != 0 || b[6] != 0 || b[7] != 0 {
		return Frame{}, errReserved
	}
	n := binary.BigEndian.Uint32(b[20:24])
	if n > MaxPayload {
		return Frame{}, errPayloadTooBig
	}
	if uint64(n) != uint64(len(b)-HeaderLen) {
		return Frame{}, errLength
	}
	return Frame{
		Src:     netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[8:12])), binary.BigEndian.Uint16(b[16:18])),
		Dst:     netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[12:16])), binary.BigEndian.Uint16(b[18:20])),
		Payload: b[HeaderLen:],
	}, nil
}
