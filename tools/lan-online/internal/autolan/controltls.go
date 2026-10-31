package autolan

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"errors"
	"math/big"
	"strings"
	"time"
)

// Optional TLS for cross-host coordinator connections (--control-tls).
//
// Each coordinator start generates a fresh ECDSA P-256 certificate in memory and
// advertises the SHA-256 fingerprint of that certificate with its endpoint.
// Clients complete a bounded TLS 1.3 handshake that verifies exactly that
// fingerprint before sending any preface, token or stream byte. The private key
// never leaves the process.
//
// Limitation: discovery is unauthenticated, so the fingerprint only protects
// against passive observation of the LAN. A malicious discovery participant can
// advertise its own endpoint and fingerprint. Hostname/CA verification for
// domain-hosted lobbies is not implemented.

const (
	controlALPN = "eden-nsolan-ctl/1"
)

var (
	errPinMismatch = errors.New("coordinator certificate does not match the advertised fingerprint")
	errNoPin       = errors.New("TLS mode requires an advertised coordinator fingerprint")
)

// newControlTLS creates the server configuration and fingerprint for one
// coordinator lifetime.
func newControlTLS() (*tls.Config, string, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, "", err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 120))
	if err != nil {
		return nil, "", err
	}
	now := time.Now()
	tmpl := &x509.Certificate{
		SerialNumber:          serial,
		Subject:               pkix.Name{CommonName: "eden-nsolan-auto-coordinator"},
		NotBefore:             now.Add(-time.Hour),
		NotAfter:              now.Add(30 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		return nil, "", err
	}
	cfg := &tls.Config{
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}},
		MinVersion:   tls.VersionTLS13,
		MaxVersion:   tls.VersionTLS13,
		NextProtos:   []string{controlALPN},
	}
	return cfg, certFingerprint(der), nil
}

func certFingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	return hex.EncodeToString(sum[:])
}

// pinnedClientConfig accepts exactly the leaf certificate with the given
// SHA-256 fingerprint, over TLS 1.3 with the control ALPN. Chain and hostname
// verification are replaced by this pin (the coordinator certificate is
// self-signed and addressed by IP).
func pinnedClientConfig(pin string) (*tls.Config, error) {
	if !isHex64(pin) { // the canonical form discovery accepts; nothing else is a pin
		return nil, errNoPin
	}
	want, err := hex.DecodeString(pin)
	if err != nil || len(want) != sha256.Size {
		return nil, errNoPin
	}
	return &tls.Config{
		MinVersion:         tls.VersionTLS13,
		MaxVersion:         tls.VersionTLS13,
		NextProtos:         []string{controlALPN},
		InsecureSkipVerify: true, // replaced by the exact fingerprint check below
		VerifyConnection: func(cs tls.ConnectionState) error {
			if len(cs.PeerCertificates) == 0 {
				return errPinMismatch
			}
			got := sha256.Sum256(cs.PeerCertificates[0].Raw)
			if subtle.ConstantTimeCompare(got[:], want) != 1 {
				return errPinMismatch
			}
			if cs.NegotiatedProtocol != controlALPN || cs.Version != tls.VersionTLS13 {
				return errors.New("coordinator did not negotiate the control protocol over TLS 1.3")
			}
			return nil
		},
	}, nil
}

// isHex64 accepts a canonical (lower-case) hex SHA-256 fingerprint.
func isHex64(s string) bool {
	if len(s) != 64 || strings.ToLower(s) != s {
		return false
	}
	_, err := hex.DecodeString(s)
	return err == nil
}
