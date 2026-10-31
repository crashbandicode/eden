// Package lanstations builds the station list GetSessionURLs hands a joiner on a
// flat private LAN where no NAT separates the two players.
//
// Upstream's natbridge rewrites a host's stations around an external NAT
// observation; on a LAN the host's own address is already reachable, so the only
// thing that matters is handing over the UDP endpoint the host reported after its
// NAT check (ReplaceURL carries the UDP port, CID and RVCID). If Register still
// carries placeholder ports, the in-memory NNCS observation may supply the port
// only when the observed IP identifies one authenticated PID. Two presentations
// are offered so root can measure which one the game accepts:
//
//   - "raw": exactly the stations the host registered/replaced.
//   - "lan": the two-candidate shape upstream measured from Nintendo
//     ([LAN candidate without type/Pa, public candidate type=0x0B Pa=<addr>]),
//     both pointing at the host's LAN address and reported UDP port.
package lanstations

import (
	"net/netip"
	"strconv"

	"edennsolan/backend/internal/natcheck"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

// switchPublicType is BehindNAT|Public|Switch (0x0B) — the public-candidate type
// Pia 5.19 expects (upstream wire measurement; also used by the core's own bridge).
const switchPublicType = int(nex.StationURLFlagBehindNAT | nex.StationURLFlagPublic | 0x08)

// Mode selects the presentation.
type Mode string

const (
	ModeRaw Mode = "raw"
	ModeLAN Mode = "lan"
)

// Builder returns a nex.Matchmaking.SessionStations function for the mode.
func Builder(mode Mode, logf func(string, ...any), observations func() []natcheck.Observation) func(joiner, host *nex.Connection) []*nex.StationURL {
	return func(joiner, host *nex.Connection) []*nex.StationURL {
		stations := host.Stations()
		if mode == ModeLAN && observations != nil {
			if src := pickSource(stations); src != nil && !reportedUDP(src) {
				if endpoint, ok := observedEndpoint(host, observations()); ok && privateAddress(src.Get("address")) {
					// Register may carry only placeholder ports1/2. NNCS observed this
					// uniquely identified host's UDP endpoint; retain every other field.
					station := src.Copy()
					station.Set("address", endpoint.Addr().String())
					station.SetInt("port", int(endpoint.Port()))
					stations = []*nex.StationURL{station}
				}
			}
		}
		urls := Build(mode, host.ID, stations)
		if logf != nil {
			strs := make([]string, 0, len(urls))
			for _, u := range urls {
				strs = append(strs, u.String())
			}
			logf("[lanstations] mode=%s joiner=%d host=%d -> %v", mode, joiner.PID, host.PID, strs)
		}
		return urls
	}
}

// PIDBuilder is Builder for the automatic runtime. Every instance has its own
// virtual address there, and the broker records the NNCS-observed virtual UDP
// endpoint under the authenticated PID, so the fallback is an exact PID lookup
// instead of an IP correlation. A CID-bearing reported endpoint still wins.
func PIDBuilder(mode Mode, logf func(string, ...any), endpoint func(pid uint64) (netip.AddrPort, bool)) func(joiner, host *nex.Connection) []*nex.StationURL {
	return func(joiner, host *nex.Connection) []*nex.StationURL {
		stations := host.Stations()
		if mode == ModeLAN && endpoint != nil && authenticatedSecure(host) {
			if src := pickSource(stations); src != nil && !reportedUDP(src) && privateAddress(src.Get("address")) {
				if observed, ok := endpoint(host.PID); ok && observed.Port() > 2 && privateAddress(observed.Addr().String()) {
					station := src.Copy()
					station.Set("address", observed.Addr().String())
					station.SetInt("port", int(observed.Port()))
					stations = []*nex.StationURL{station}
				}
			}
		}
		urls := Build(mode, host.ID, stations)
		if logf != nil {
			strs := make([]string, 0, len(urls))
			for _, u := range urls {
				strs = append(strs, u.String())
			}
			logf("[lanstations] mode=%s joiner=%d host=%d -> %v", mode, joiner.PID, host.PID, strs)
		}
		return urls
	}
}

func authenticatedSecure(host *nex.Connection) bool {
	return host.PID != 0 && host.Endpoint != nil && host.Endpoint.Secure &&
		host.Endpoint.FindConnectionByID(host.ID) == host
}

func privateAddress(text string) bool {
	ip, err := netip.ParseAddr(text)
	return err == nil && ip.Is4() && (ip.IsPrivate() || ip.IsLoopback())
}

func reportedUDP(station *nex.StationURL) bool {
	port, err := strconv.ParseUint(station.Get("port"), 10, 16)
	cid, cidErr := strconv.ParseUint(station.Get("CID"), 10, 32)
	// Register's derived public station carries the WSS TCP source port.
	// Only the CID-bearing ReplaceURL station proves a reported UDP endpoint.
	return err == nil && port > 2 && cidErr == nil && cid != 0 && station.Get("address") != ""
}

// An IP-only NNCS observation is usable only when it identifies one authenticated
// PID on this secure endpoint. In particular, two clients behind one IP must not
// inherit whichever UDP port happened to probe last.
func observedEndpoint(host *nex.Connection, observations []natcheck.Observation) (netip.AddrPort, bool) {
	if host.PID == 0 || host.Endpoint == nil || !host.Endpoint.Secure ||
		host.Endpoint.FindConnectionByID(host.ID) != host {
		return netip.AddrPort{}, false
	}
	remote, err := netip.ParseAddrPort(host.RemoteAddr)
	if err != nil || !privateAddress(remote.Addr().String()) {
		return netip.AddrPort{}, false
	}
	for _, id := range host.Endpoint.ConnectionIDs() {
		peer := host.Endpoint.FindConnectionByID(id)
		if peer == nil || peer.PID == 0 || peer.PID == host.PID {
			continue
		}
		address, err := netip.ParseAddrPort(peer.RemoteAddr)
		if err == nil && address.Addr() == remote.Addr() {
			return netip.AddrPort{}, false
		}
	}
	var endpoint netip.AddrPort
	for _, observation := range observations {
		if observation.IP != remote.Addr().String() {
			continue
		}
		if observation.Port <= 2 || observation.Port > 65535 || endpoint.IsValid() {
			return netip.AddrPort{}, false
		}
		endpoint = netip.AddrPortFrom(remote.Addr(), uint16(observation.Port))
	}
	return endpoint, endpoint.IsValid()
}

// Build is the pure transformation (testable without connections).
func Build(mode Mode, hostConnID uint32, stations []*nex.StationURL) []*nex.StationURL {
	if mode == ModeRaw || len(stations) == 0 {
		return stations
	}
	src := pickSource(stations)
	if src == nil {
		return stations
	}
	lan := src.Copy()
	lan.Remove("type")
	lan.Remove("Pa")
	lan.SetInt("RVCID", int(hostConnID))

	pub := src.Copy()
	pub.SetInt("type", switchPublicType)
	pub.Set("Pa", src.Get("address"))
	pub.SetInt("RVCID", int(hostConnID))
	return []*nex.StationURL{lan, pub}
}

// pickSource prefers the station carrying a CID (the post-NAT-check ReplaceURL
// station, which has the real UDP port), else the first station with an address.
func pickSource(stations []*nex.StationURL) *nex.StationURL {
	for _, u := range stations {
		if u != nil && reportedUDP(u) && u.GetInt("CID") != 0 {
			return u
		}
	}
	for _, u := range stations {
		if u != nil && u.Get("address") != "" {
			return u
		}
	}
	return nil
}
