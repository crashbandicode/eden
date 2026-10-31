// Package identity resolves NEX login usernames to fixture PIDs.
//
// The resolver is a pure in-memory table. It never contacts an account service,
// never derives identities from unknown usernames, and rejects everything that is
// not one of the registered identities. The fixed fixture registers its two
// identities at construction; the automatic runtime adds and removes leased
// identities while running (Add/Remove).
package identity

import (
	"crypto/sha256"
	"errors"
	"sync"
	"sync/atomic"

	nex "github.com/NextendoNetwork/nextendo-nex"

	"edennsolan/backend/internal/fixture"
)

// Resolver maps usernames to PIDs.
type Resolver struct {
	mu         sync.RWMutex
	byUsername map[string]uint64
	// Accepted / Rejected count resolutions (observability for tests and logs).
	Accepted atomic.Uint64
	Rejected atomic.Uint64
	// Logf receives one line per decision; nil disables logging.
	Logf func(format string, args ...any)
}

// New builds a resolver from fixture identities.
func New(ids []fixture.Identity) (*Resolver, error) {
	if len(ids) == 0 {
		return nil, errors.New("identity: no identities")
	}
	r := &Resolver{byUsername: make(map[string]uint64, len(ids))}
	for _, id := range ids {
		if id.Username == "" || id.PID == 0 {
			return nil, errors.New("identity: empty username or zero PID")
		}
		if _, dup := r.byUsername[id.Username]; dup {
			return nil, errors.New("identity: duplicate username " + id.Username)
		}
		r.byUsername[id.Username] = id.PID
	}
	return r, nil
}

// NewDynamic builds an empty resolver for the automatic runtime; identities are
// registered with Add when a coordinator lease is granted.
func NewDynamic() *Resolver {
	return &Resolver{byUsername: map[string]uint64{}}
}

// Add registers one identity. The username and PID must be non-empty/non-zero
// and must not already belong to a different identity; re-adding the identical
// pair is a no-op.
func (r *Resolver) Add(username string, pid uint64) error {
	if username == "" || pid == 0 {
		return errors.New("identity: empty username or zero PID")
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if existing, ok := r.byUsername[username]; ok {
		if existing == pid {
			return nil
		}
		return errors.New("identity: duplicate username " + username)
	}
	for _, existing := range r.byUsername {
		if existing == pid {
			return errors.New("identity: duplicate PID")
		}
	}
	r.byUsername[username] = pid
	return nil
}

// Remove unregisters a username and reports whether it was present.
func (r *Resolver) Remove(username string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, ok := r.byUsername[username]; !ok {
		return false
	}
	delete(r.byUsername, username)
	return true
}

// SourceKeyLen is the Switch Kerberos key size; a 16-byte key is rejected by the
// console (upstream measurement), so the key is always 32 bytes.
const SourceKeyLen = 32

// SourceKey derives the per-user ticket key. It is deterministic so a client
// that re-logs in gets the same key; it is handed back to the client in the
// login reply, so it protects nothing beyond protocol shape.
func SourceKey(username string) []byte {
	sum := sha256.Sum256([]byte("nsolan-src:" + username))
	return sum[:SourceKeyLen]
}

// Resolve implements nex.AuthConfig.ResolveUserEx. extraData (the id-token
// DataHolder) is accepted as-is and only its length is logged.
func (r *Resolver) Resolve(username string, extraData []byte) (uint64, []byte, uint32) {
	r.mu.RLock()
	pid, ok := r.byUsername[username]
	r.mu.RUnlock()
	if !ok {
		r.Rejected.Add(1)
		if r.Logf != nil {
			r.Logf("[identity] REJECT username=%q (not a registered identity) extraDataLen=%d", username, len(extraData))
		}
		return 0, nil, nex.ResultAuthTokenParseError
	}
	r.Accepted.Add(1)
	if r.Logf != nil {
		r.Logf("[identity] accept username=%q -> pid=%d extraDataLen=%d", username, pid, len(extraData))
	}
	return pid, SourceKey(username), 0
}

// PIDs returns the registered PIDs (for logging and tests).
func (r *Resolver) PIDs() []uint64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]uint64, 0, len(r.byUsername))
	for _, pid := range r.byUsername {
		out = append(out, pid)
	}
	return out
}
