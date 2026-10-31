package autolan

import (
	"errors"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"edennsolan/backend/internal/initshape"
	nex "github.com/NextendoNetwork/nextendo-nex"
)

func (b *Broker) exchangeProfile(method uint32, body []byte) ([]byte, uint32) {
	b.profileMu.Lock()
	defer b.profileMu.Unlock()
	if b.profileClosed || b.profile == nil {
		return nil, nex.ResultCoreUnknown
	}
	return b.profile.ExchangeFighterStats(1, method, body)
}

func (b *Broker) openProfile() error {
	b.profile = initshape.New(initshape.Options{FighterStats: true, Logf: b.logf})
	if b.opt.ProfileDir == "" {
		return nil
	}
	dir := b.opt.ProfileDir
	if !filepath.IsAbs(dir) || filepath.Clean(dir) != dir {
		return statusErr("profile_store_invalid", "profile directory must be an absolute clean path")
	}
	if err := mkdirPrivate(dir); err != nil {
		return statusErr("profile_store_invalid", "%v", err)
	}
	if err := checkPrivateDir(dir); err != nil {
		return statusErr("profile_store_invalid", "%v", err)
	}
	lock, err := lockProfile(filepath.Join(dir, "ratings.lock"))
	if err != nil {
		return statusErr("profile_in_use", "profile ratings are locked or inaccessible")
	}
	fail := func(err error) error { _ = lock.Close(); return statusErr("profile_store_invalid", "%v", err) }
	path := filepath.Join(dir, "ratings.json")
	if fi, err := os.Lstat(path); err == nil {
		if !fi.Mode().IsRegular() || !ownedByCurrentUser(fi, path) {
			return fail(errors.New("ratings must be an owned regular file"))
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return fail(err)
	}
	seeds := map[uint64]map[byte]uint32{}
	if b.opt.FighterSeeds != "" {
		seeds[1] = map[byte]uint32{}
		for _, item := range strings.Split(b.opt.FighterSeeds, ",") {
			parts := strings.Split(item, ":")
			if len(parts) != 2 {
				return fail(errors.New("fighter seeds must be slot:rating"))
			}
			key, e1 := strconv.ParseUint(parts[0], 10, 8)
			value, e2 := strconv.ParseUint(parts[1], 10, 32)
			if e1 != nil || e2 != nil || key >= 99 {
				return fail(errors.New("invalid fighter seed"))
			}
			if _, ok := seeds[1][byte(key)]; ok {
				return fail(errors.New("duplicate fighter seed"))
			}
			seeds[1][byte(key)] = uint32(value)
		}
	}
	// Local key1 names this profile's singleton store. It never represents a
	// leased NEX principal, and no profile path or key is accepted over the LAN.
	if err := b.profile.LoadFighterState(path, map[uint64]bool{1: true}, seeds); err != nil {
		return fail(err)
	}
	b.profileLock = lock
	return nil
}

// Route using the currently registered authenticated principal. Each member
// serves only its own local profile. New coordinators need no copied database.
func (c *Coordinator) fighterExchange(pid uint64, remote string, method uint32, body []byte) ([]byte, uint32) {
	fail := func() ([]byte, uint32) { return nil, nex.ResultCoreUnknown }
	if pid > 0xffffffff || (method != 82 && method != 84) {
		return fail()
	}
	c.mu.Lock()
	st := c.members[c.byPID[uint32(pid)]]
	// Mock NEX credentials are protocol stubs, not proof of lease ownership.
	// RemoteAddr is assigned by serveStream after it validates the member token.
	// Bind the claimed principal to that server-owned virtual source address.
	addr, err := netip.ParseAddrPort(remote)
	if st == nil || err != nil || addr.Addr().Unmap() != st.vip {
		c.mu.Unlock()
		return nil, nex.ResultCoreAccessDenied
	}
	if st == nil || c.closed || len(st.fighterWaiters) >= 4 {
		c.mu.Unlock()
		return fail()
	}
	seq := c.fighterSeq.Add(1)
	ch := make(chan coordEvent, 1)
	st.fighterWaiters[seq] = ch
	c.mu.Unlock()
	defer func() { c.mu.Lock(); delete(st.fighterWaiters, seq); c.mu.Unlock() }()
	if st.w.send(coordEvent{Op: "fighter_request", Seq: seq, Method: method, Body: body}) != nil {
		return fail()
	}
	t := time.NewTimer(3 * time.Second)
	defer t.Stop()
	select {
	case reply := <-ch:
		if reply.Result != 0 {
			return nil, nex.ResultCoreUnknown
		}
		if method == 82 && len(reply.Body) != 0 {
			return fail()
		}
		if method == 84 && (len(reply.Body) < 29 || len(reply.Body) > 3129) {
			return fail()
		}
		return reply.Body, 0
	case <-st.done:
		return fail()
	case <-c.ctx.Done():
		return fail()
	case <-t.C:
		return fail()
	}
}
