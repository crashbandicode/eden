package nsoserver

import (
	"crypto/hmac"
	"crypto/md5"
	"crypto/rc4"
	"errors"
	"fmt"
	"sync"
	"time"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// liteClient is a minimal PRUDP-Lite console stand-in. It feeds encoded packets
// into a real nex.Connection (the same entry point the WebSocket transport uses)
// and decodes everything the server sends back, so handshake, Kerberos CONNECT,
// RMC dispatch, fragmentation and server-initiated notifications are all
// exercised through the production code paths.
type liteClient struct {
	s        *nex.Settings
	conn     *nex.Connection
	connSig  []byte
	seq      uint16
	mu       sync.Mutex
	frames   [][]byte
	consumed int
	fragBuf  []byte
	seen     map[uint16]bool
	// decoded messages
	responses     []*nex.RMCMessage
	notifications []*nex.RMCMessage
	acks          []*nex.Packet
}

const (
	cVType, cVPort = uint8(10), uint8(15)
	sVType, sVPort = uint8(10), uint8(1)
)

func newLiteClient(ep *nex.Endpoint, addr string) *liteClient {
	c := &liteClient{s: ep.Settings, seq: 1, seen: map[uint16]bool{}}
	c.conn = nex.NewConnection(ep, addr, func(b []byte) {
		c.mu.Lock()
		c.frames = append(c.frames, append([]byte(nil), b...))
		c.mu.Unlock()
	})
	return c
}

// handshake runs SYN -> SYN+ACK -> CONNECT(payload) -> CONNECT+ACK.
func (c *liteClient) handshake(payload []byte) error {
	c.conn.Feed(nex.EncodePacket(&nex.Packet{
		Type: nex.PacketSYN, Flags: nex.FlagNeedACK,
		SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort,
		MinorVersion: 5,
	}))
	pk, err := c.nextRaw(time.Second)
	if err != nil {
		return fmt.Errorf("syn-ack: %w", err)
	}
	if pk.Type != nex.PacketSYN || !pk.HasFlag(nex.FlagACK) || len(pk.ConnectionSig) != 16 {
		return fmt.Errorf("bad syn-ack %+v", pk)
	}
	c.connSig = pk.ConnectionSig
	connect := &nex.Packet{
		Type: nex.PacketCONNECT, Flags: nex.FlagNeedACK,
		SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort,
		MinorVersion: 5, PacketID: 1, Payload: payload,
	}
	connect.Signature = nex.LitePacketSignature(c.s.AccessKey, connect, c.connSig)
	c.conn.Feed(nex.EncodePacket(connect))
	pk, err = c.nextRaw(time.Second)
	if err != nil {
		return fmt.Errorf("connect-ack: %w", err)
	}
	if pk.Type != nex.PacketCONNECT || !pk.HasFlag(nex.FlagACK) {
		return fmt.Errorf("bad connect-ack %+v", pk)
	}
	return nil
}

// nextRaw returns the next undecoded frame as a packet (handshake use only).
func (c *liteClient) nextRaw(timeout time.Duration) (*nex.Packet, error) {
	deadline := time.Now().Add(timeout)
	for {
		c.mu.Lock()
		if c.consumed < len(c.frames) {
			f := c.frames[c.consumed]
			c.consumed++
			c.mu.Unlock()
			pks, _, err := nex.DecodePackets(f)
			if err != nil || len(pks) != 1 {
				return nil, fmt.Errorf("decode: %v n=%d", err, len(pks))
			}
			return pks[0], nil
		}
		c.mu.Unlock()
		if time.Now().After(deadline) {
			return nil, errors.New("timeout")
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// pump decodes all pending frames: ACKs are recorded, reliable DATA is
// reassembled by fragment id, deduplicated by sequence id and acknowledged.
func (c *liteClient) pump() {
	for {
		c.mu.Lock()
		if c.consumed >= len(c.frames) {
			c.mu.Unlock()
			return
		}
		f := c.frames[c.consumed]
		c.consumed++
		c.mu.Unlock()
		pks, _, err := nex.DecodePackets(f)
		if err != nil {
			continue
		}
		for _, p := range pks {
			if p.Type != nex.PacketDATA {
				continue
			}
			if p.HasFlag(nex.FlagACK) || p.HasFlag(nex.FlagMultiACK) {
				c.acks = append(c.acks, p)
				continue
			}
			if c.seen[p.PacketID] {
				continue // retransmission
			}
			c.seen[p.PacketID] = true
			// ACK it so the server's retransmit loop stays quiet.
			c.conn.Feed(nex.EncodePacket(&nex.Packet{Type: nex.PacketDATA, Flags: nex.FlagACK,
				SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort, PacketID: p.PacketID}))
			c.fragBuf = append(c.fragBuf, p.Payload...)
			if p.FragmentID != 0 {
				continue
			}
			full := c.fragBuf
			c.fragBuf = nil
			m, err := nex.ParseRMC(c.s, full)
			if err != nil {
				continue
			}
			if m.Mode == nex.RMCRequest {
				c.notifications = append(c.notifications, m)
			} else {
				c.responses = append(c.responses, m)
			}
		}
	}
}

// call sends one RMC request and waits for its response.
func (c *liteClient) call(proto uint16, method, callID uint32, body []byte) (*nex.RMCMessage, error) {
	req := nex.NewRMCRequest(c.s, proto, method, callID, body)
	c.conn.Feed(nex.EncodePacket(&nex.Packet{
		Type: nex.PacketDATA, Flags: nex.FlagReliable | nex.FlagNeedACK | nex.FlagHasSize,
		SourceType: cVType, SourcePort: cVPort, DestType: sVType, DestPort: sVPort,
		PacketID: c.seq, Payload: req.Encode(),
	}))
	c.seq++
	deadline := time.Now().Add(3 * time.Second)
	for {
		c.pump()
		for i, r := range c.responses {
			if r.CallID == callID && r.Protocol == proto {
				c.responses = append(c.responses[:i], c.responses[i+1:]...)
				return r, nil
			}
		}
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("timeout waiting for response proto=%#x method=%d call=%d", proto, method, callID)
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// takeNotifications drains decoded server-initiated requests.
func (c *liteClient) takeNotifications() []*nex.RMCMessage {
	c.pump()
	out := c.notifications
	c.notifications = nil
	return out
}

// kerberosSeal is an independent re-implementation of the documented Kerberos
// payload protection (RC4 keystream, then HMAC-MD5 of the ciphertext appended),
// used to build the console side of the secure CONNECT.
func kerberosSeal(key, data []byte) []byte {
	ciph, err := rc4.NewCipher(key)
	if err != nil {
		panic(err)
	}
	enc := make([]byte, len(data))
	ciph.XORKeyStream(enc, data)
	mac := hmac.New(md5.New, key)
	mac.Write(enc)
	return append(enc, mac.Sum(nil)...)
}
