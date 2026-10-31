package natcheck

import (
	"context"
	"encoding/binary"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// decodeReply is an independent decoder written from the two client
// implementations' descriptions (echoed id, observed port, observed IPv4,
// responder IPv4; all big-endian u32), not from the responder's encoder.
func decodeReply(t *testing.T, b []byte) (id uint32, port uint32, src net.IP, server net.IP) {
	t.Helper()
	if len(b) != ProbeLen {
		t.Fatalf("reply length %d, want %d", len(b), ProbeLen)
	}
	id = binary.BigEndian.Uint32(b[0:4])
	port = binary.BigEndian.Uint32(b[4:8])
	src = net.IPv4(b[8], b[9], b[10], b[11])
	server = net.IPv4(b[12], b[13], b[14], b[15])
	return
}

func startResponder(t *testing.T, file string) *Responder {
	t.Helper()
	r := &Responder{Addrs: []string{"127.0.0.1"}, Ports: []int{0, 0}, ObservationFile: file}
	if err := r.Listen(); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { r.Serve(ctx); close(done) }()
	t.Cleanup(func() { cancel(); <-done })
	return r
}

func TestProbeEchoContract(t *testing.T) {
	file := filepath.Join(t.TempDir(), "nat_endpoints.txt")
	r := startResponder(t, file)
	addrs := r.LocalAddrs()
	if len(addrs) != 2 {
		t.Fatalf("want 2 sockets, got %v", addrs)
	}

	client, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	clientPort := client.LocalAddr().(*net.UDPAddr).Port

	for _, target := range addrs {
		for _, testID := range []uint32{101, 102, 103} {
			probe := make([]byte, 16)
			binary.BigEndian.PutUint32(probe, testID)
			dst, _ := net.ResolveUDPAddr("udp4", target)
			if _, err := client.WriteTo(probe, dst); err != nil {
				t.Fatal(err)
			}
			buf := make([]byte, 64)
			_ = client.SetReadDeadline(time.Now().Add(2 * time.Second))
			n, from, err := client.ReadFrom(buf)
			if err != nil {
				t.Fatalf("no reply from %s id %d: %v", target, testID, err)
			}
			if from.String() != target {
				t.Fatalf("reply must come from the probed socket: got %s want %s", from, target)
			}
			id, port, src, server := decodeReply(t, buf[:n])
			if id != testID || int(port) != clientPort || !src.Equal(net.IPv4(127, 0, 0, 1)) || !server.Equal(net.IPv4(127, 0, 0, 1)) {
				t.Fatalf("reply fields: id=%d port=%d src=%s server=%s", id, port, src, server)
			}
		}
	}
	if n := r.ProbeCount(); n != 6 {
		t.Fatalf("probes answered %d, want 6", n)
	}
	obs := r.Observations()
	if len(obs) != 1 || obs[0].IP != "127.0.0.1" || obs[0].Port != clientPort {
		t.Fatalf("observations: %+v", obs)
	}
	b, err := os.ReadFile(file)
	if err != nil || strings.TrimSpace(string(b)) != "127.0.0.1 "+itoa(clientPort) {
		t.Fatalf("observation file: %q err=%v", b, err)
	}
}

// A datagram shorter than the 16-byte probe is ignored: no reply, no observation.
func TestShortProbeIgnored(t *testing.T) {
	r := startResponder(t, "")
	client, _ := net.ListenPacket("udp4", "127.0.0.1:0")
	defer client.Close()
	dst, _ := net.ResolveUDPAddr("udp4", r.LocalAddrs()[0])
	_, _ = client.WriteTo([]byte{0, 0, 0, 101, 0, 0, 0, 0}, dst)
	buf := make([]byte, 64)
	_ = client.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if n, _, err := client.ReadFrom(buf); err == nil {
		t.Fatalf("short probe must not be answered, got %d bytes", n)
	}
	if n := r.ProbeCount(); n != 0 || len(r.Observations()) != 0 {
		t.Fatalf("short probe must not be recorded: %d %+v", n, r.Observations())
	}
}

func TestListenFailsClosedOnBadAddress(t *testing.T) {
	r := &Responder{Addrs: []string{"nncs1.example"}, Ports: []int{0}}
	if err := r.Listen(); err == nil {
		t.Fatal("hostname must be rejected")
	}
	r = &Responder{Addrs: []string{"127.0.0.1"}, Ports: nil}
	if err := r.Listen(); err == nil {
		t.Fatal("no ports must be rejected")
	}
}

func TestReplyEncoding(t *testing.T) {
	src := mustAddrPort("192.168.50.11:49821")
	b := Reply(101, src, mustAddrPort("192.168.50.10:10025").Addr())
	want := []byte{0, 0, 0, 101, 0, 0, 0xC2, 0x9D, 192, 168, 50, 11, 192, 168, 50, 10}
	if string(b) != string(want) {
		t.Fatalf("reply % x want % x", b, want)
	}
}
