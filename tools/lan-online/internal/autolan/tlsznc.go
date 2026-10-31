package autolan

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"io"
	"math/big"
	"net"
	"strconv"
	"sync"
	"time"

	"edennsolan/backend/internal/nsoserver"
)

// selfSignedTLS creates an in-memory RSA-2048 certificate for the virtual
// service names. Guest TLS verification is disabled by the emulator's SSL
// service, so no trust store or private key file is involved.
func selfSignedTLS(hosts ...string) (*tls.Config, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 120))
	if err != nil {
		return nil, err
	}
	now := time.Now()
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "nsolan-auto"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(30 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		DNSNames:              hosts,
		IPAddresses:           []net.IP{serviceIP1.AsSlice(), serviceIP2.AsSlice()},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, err
	}
	return nsoserver.ServerTLSConfig(tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}), nil
}

// zncServer answers the voice-chat API host locally with HTTP 503. It is a
// bounded metadata responder, not voice chat: it reads at most 64 KiB of
// request head, never logs paths or queries, and always closes.
type zncServer struct {
	ln    net.Listener
	tls   *tls.Config
	logf  func(string, ...any)
	wg    sync.WaitGroup
	mu    sync.Mutex
	conns map[net.Conn]struct{}
}

const zncMaxHead = 64 << 10

var znc503Body = []byte(`{"status":503,"error":"service_unavailable","detail":"voice chat is not provided by the LAN runtime"}`)

func (z *zncServer) serve(ctx context.Context) {
	z.conns = map[net.Conn]struct{}{}
	go func() {
		<-ctx.Done()
		_ = z.ln.Close()
		z.mu.Lock()
		for c := range z.conns {
			_ = c.Close()
		}
		z.mu.Unlock()
	}()
	for {
		c, err := z.ln.Accept()
		if err != nil {
			z.wg.Wait()
			return
		}
		z.mu.Lock()
		z.conns[c] = struct{}{}
		z.mu.Unlock()
		z.wg.Add(1)
		go func() {
			defer z.wg.Done()
			defer func() {
				z.mu.Lock()
				delete(z.conns, c)
				z.mu.Unlock()
				_ = c.Close()
			}()
			z.handle(c)
		}()
	}
}

func (z *zncServer) handle(raw net.Conn) {
	_ = raw.SetDeadline(time.Now().Add(10 * time.Second))
	conn := tls.Server(raw, z.tls)
	if err := conn.Handshake(); err != nil {
		return
	}
	r := bufio.NewReader(io.LimitReader(conn, zncMaxHead))
	method := ""
	for {
		line, err := r.ReadSlice('\n')
		if err != nil {
			return // oversized or truncated head: close without a reply
		}
		if method == "" {
			if i := bytes.IndexByte(line, ' '); i > 0 && i <= 16 {
				method = string(line[:i])
			}
		}
		if len(bytes.TrimRight(line, "\r\n")) == 0 {
			break
		}
	}
	z.logf("[znc] answered 503 method=%q", method)
	var b bytes.Buffer
	b.WriteString("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ")
	b.WriteString(strconv.Itoa(len(znc503Body)))
	b.WriteString("\r\n\r\n")
	b.Write(znc503Body)
	_, _ = conn.Write(b.Bytes())
	_ = conn.CloseWrite()
}
