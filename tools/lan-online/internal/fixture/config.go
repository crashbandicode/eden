// Package fixture holds the private-LAN fixture configuration.
//
// Everything is read from the environment with the NSOLAN_ prefix and validated
// fail-closed: the backend refuses to start unless every endpoint it binds or
// advertises is a private (RFC1918 / loopback / link-local / CGNAT) address and
// exactly two distinct fixture identities are configured.
package fixture

import (
	"errors"
	"fmt"
	"net/netip"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// Identity is one fixture player: the LoginEx username the game sends and the
// NEX PID the backend returns for it. By default both are the same decimal
// number, because upstream measured that a PID different from the identity the
// game knows itself by breaks Pia self-recognition (2618-562 SessionKeepFailed).
type Identity struct {
	Username string
	PID      uint64
}

// NATCheck configures the UDP NAT-check responders.
type NATCheck struct {
	Enabled bool
	// Addrs are the local IPv4 addresses to bind; the game expects two
	// distinguishable responders (nncs1/nncs2), each on every port in Ports.
	Addrs []string
	Ports []int
	// ObservationFile, when non-empty, receives "<ip> <port>" lines for the last
	// probe seen from each source IP (the upstream natbridge file shape).
	ObservationFile string
}

// Config is the validated fixture configuration.
type Config struct {
	AdvertiseHost string
	BindHost      string
	AuthPort      int
	SecurePort    int
	TLSCert       string
	TLSKey        string
	ServerName    string

	AccessKey  string
	NexVersion int
	SecurePID  uint64

	Identities []Identity

	NATCheck NATCheck

	// SessionStations selects how GetSessionURLs presents the host to a joiner:
	// "lan" (two-candidate shape built from the host's own LAN station) or
	// "raw" (exactly what the host registered). Both are measurable by root.
	SessionStations string

	// UtilityIntSettings is the number of (position-indexed) entries answered to
	// Utility GetIntegerSettings; values default to zero unless overridden.
	UtilityIntSettings int
	UtilityIntValues   map[int]int32

	// ReplayDir, when non-empty, lets the operator override individual init
	// responses with files named "0x<proto>_<method>.bin". Off by default and
	// never populated by this repository.
	ReplayDir string

	// FighterStats opts into the candidate stateful SSBU report/read service.
	// Live Elite transitions and aggregate semantics are not accepted yet.
	FighterStats    bool
	FighterExchange func(uint64, string, uint32, []byte) ([]byte, uint32)
	// Fixed-fixture persistence and explicit current-rating test overrides.
	FighterStateFile string
	FighterSeeds     map[uint64]map[byte]uint32

	// Dynamic selects the automatic LAN runtime: identities are added and removed
	// at runtime from coordinator leases (none are configured statically) and NAT
	// checks are answered by the per-instance brokers, not by host UDP sockets.
	// FromEnv never sets it, so the fixed two-identity fixture is unchanged.
	Dynamic bool
}

// Defaults that are game-side constants (from the pinned upstream SSBU server),
// not response bytes.
const (
	DefaultAccessKey  = "9587602b"
	DefaultNexVersion = 40000
	DefaultSecurePID  = 2
	DefaultAuthPort   = 443
	DefaultSecurePort = 60005
	DefaultIdentities = "49153,49154"
	// DefaultUtilityIntSettings is the entry count implied by the upstream
	// capture README arithmetic (4 + 431*6 = 2590 bytes); only the count is
	// reused, never the values.
	DefaultUtilityIntSettings = 431
)

// NewDynamic returns the automatic-runtime configuration. advertise is the
// virtual service address the guest reaches the secure endpoint through; the
// listeners themselves are supplied by the caller (see nsoserver.Serve).
func NewDynamic(advertise, replayDir string) (*Config, error) {
	c := &Config{
		AdvertiseHost:      advertise,
		BindHost:           advertise,
		AuthPort:           DefaultAuthPort,
		SecurePort:         DefaultSecurePort,
		ServerName:         "nsolan",
		AccessKey:          DefaultAccessKey,
		NexVersion:         DefaultNexVersion,
		SecurePID:          DefaultSecurePID,
		SessionStations:    "lan",
		UtilityIntSettings: DefaultUtilityIntSettings,
		UtilityIntValues:   map[int]int32{},
		ReplayDir:          replayDir,
		Dynamic:            true,
	}
	if err := c.Validate(); err != nil {
		return nil, err
	}
	return c, nil
}

// Getenv is the lookup function FromEnv uses (os.Getenv in production).
type Getenv func(string) string

// FromEnv builds and validates a Config from NSOLAN_* variables.
func FromEnv(get Getenv) (*Config, error) {
	c := &Config{
		AdvertiseHost:      strings.TrimSpace(get("NSOLAN_ADVERTISE_HOST")),
		BindHost:           strings.TrimSpace(get("NSOLAN_BIND_HOST")),
		TLSCert:            orDefault(get("NSOLAN_TLS_CERT"), "fixture/tls/cert.pem"),
		TLSKey:             orDefault(get("NSOLAN_TLS_KEY"), "fixture/tls/key.pem"),
		ServerName:         orDefault(get("NSOLAN_SERVER_NAME"), "nsolan"),
		AccessKey:          orDefault(get("NSOLAN_ACCESS_KEY"), DefaultAccessKey),
		SessionStations:    orDefault(get("NSOLAN_SESSION_STATIONS"), "lan"),
		ReplayDir:          strings.TrimSpace(get("NSOLAN_REPLAY_DIR")),
		UtilityIntSettings: DefaultUtilityIntSettings,
		SecurePID:          DefaultSecurePID,
	}
	var err error
	c.FighterStateFile = strings.TrimSpace(get("NSOLAN_FIGHTER_STATE_FILE"))
	if c.FighterSeeds, err = parseFighterSeeds(get("NSOLAN_FIGHTER_SEEDS")); err != nil {
		return nil, err
	}
	switch get("NSOLAN_FIGHTER_STATS") {
	case "", "0":
	case "1":
		c.FighterStats = true
	default:
		return nil, errors.New("NSOLAN_FIGHTER_STATS must be 0 or 1")
	}
	if c.AuthPort, err = intOr(get("NSOLAN_AUTH_PORT"), DefaultAuthPort); err != nil {
		return nil, fmt.Errorf("NSOLAN_AUTH_PORT: %w", err)
	}
	if c.SecurePort, err = intOr(get("NSOLAN_SECURE_PORT"), DefaultSecurePort); err != nil {
		return nil, fmt.Errorf("NSOLAN_SECURE_PORT: %w", err)
	}
	if c.NexVersion, err = intOr(get("NSOLAN_NEX_VERSION"), DefaultNexVersion); err != nil {
		return nil, fmt.Errorf("NSOLAN_NEX_VERSION: %w", err)
	}
	if c.UtilityIntSettings, err = intOr(get("NSOLAN_UTILITY_INT_SETTINGS"), DefaultUtilityIntSettings); err != nil {
		return nil, fmt.Errorf("NSOLAN_UTILITY_INT_SETTINGS: %w", err)
	}
	if c.UtilityIntValues, err = parseIntValues(get("NSOLAN_UTILITY_INT_VALUES")); err != nil {
		return nil, fmt.Errorf("NSOLAN_UTILITY_INT_VALUES: %w", err)
	}
	if c.Identities, err = ParseIdentities(orDefault(get("NSOLAN_IDENTITIES"), DefaultIdentities)); err != nil {
		return nil, fmt.Errorf("NSOLAN_IDENTITIES: %w", err)
	}
	if c.BindHost == "" {
		c.BindHost = c.AdvertiseHost
	}

	nat := NATCheck{Enabled: orDefault(get("NSOLAN_NATCHECK"), "1") != "0", ObservationFile: strings.TrimSpace(get("NSOLAN_NAT_FILE"))}
	if nat.Addrs = splitList(get("NSOLAN_NATCHECK_ADDRS")); len(nat.Addrs) == 0 {
		nat.Addrs = []string{c.BindHost}
	}
	for _, p := range splitList(orDefault(get("NSOLAN_NATCHECK_PORTS"), "10025,10125")) {
		n, perr := strconv.Atoi(p)
		if perr != nil {
			return nil, fmt.Errorf("NSOLAN_NATCHECK_PORTS: %q is not a port", p)
		}
		nat.Ports = append(nat.Ports, n)
	}
	c.NATCheck = nat

	if err := c.Validate(); err != nil {
		return nil, err
	}
	return c, nil
}

// Validate enforces the fail-closed rules.
func (c *Config) Validate() error {
	if c.FighterStats && c.Dynamic {
		return errors.New("fighter stats require fixed identities; dynamic leases are not persistent profile IDs")
	}
	if c.FighterStateFile != "" && (!c.FighterStats || !filepath.IsAbs(c.FighterStateFile)) {
		return errors.New("NSOLAN_FIGHTER_STATE_FILE needs enabled fighter stats and an absolute path")
	}
	if len(c.FighterSeeds) > 0 && c.FighterStateFile == "" {
		return errors.New("NSOLAN_FIGHTER_SEEDS requires a persistent fighter state file")
	}
	for pid := range c.FighterSeeds {
		found := false
		for _, identity := range c.Identities {
			if identity.PID == pid {
				found = true
			}
		}
		if !found {
			return errors.New("fighter seed PID is not a configured identity")
		}
	}
	if c.AdvertiseHost == "" {
		return errors.New("NSOLAN_ADVERTISE_HOST is required (the LAN IPv4 both Edens can reach)")
	}
	if err := requirePrivateIPv4("NSOLAN_ADVERTISE_HOST", c.AdvertiseHost); err != nil {
		return err
	}
	if err := requirePrivateIPv4("NSOLAN_BIND_HOST", c.BindHost); err != nil {
		return err
	}
	for _, p := range []struct {
		name string
		v    int
	}{{"NSOLAN_AUTH_PORT", c.AuthPort}, {"NSOLAN_SECURE_PORT", c.SecurePort}} {
		if p.v < 1 || p.v > 65535 {
			return fmt.Errorf("%s: %d out of range", p.name, p.v)
		}
	}
	if c.AuthPort == c.SecurePort {
		return errors.New("auth and secure ports must differ")
	}
	if len(c.AccessKey) != 8 {
		return errors.New("NSOLAN_ACCESS_KEY must be 8 hex characters")
	}
	if c.NexVersion < 40000 {
		return errors.New("NSOLAN_NEX_VERSION must be a NEX 4 version (>= 40000)")
	}
	if c.Dynamic {
		if len(c.Identities) != 0 {
			return errors.New("dynamic mode takes identities from coordinator leases only")
		}
		if c.NATCheck.Enabled {
			return errors.New("dynamic mode answers NAT checks in the instance brokers")
		}
	} else if len(c.Identities) != 2 {
		return fmt.Errorf("exactly two fixture identities are required, got %d", len(c.Identities))
	}
	if c.SessionStations != "lan" && c.SessionStations != "raw" {
		return fmt.Errorf("NSOLAN_SESSION_STATIONS must be lan or raw, got %q", c.SessionStations)
	}
	if c.UtilityIntSettings < 0 || c.UtilityIntSettings > 4096 {
		return errors.New("NSOLAN_UTILITY_INT_SETTINGS out of range 0..4096")
	}
	if c.NATCheck.Enabled {
		if len(c.NATCheck.Addrs) == 0 || len(c.NATCheck.Ports) == 0 {
			return errors.New("NAT check enabled but no addresses/ports")
		}
		for _, a := range c.NATCheck.Addrs {
			if err := requirePrivateIPv4("NSOLAN_NATCHECK_ADDRS", a); err != nil {
				return err
			}
		}
		for _, p := range c.NATCheck.Ports {
			if p < 1 || p > 65535 {
				return fmt.Errorf("NSOLAN_NATCHECK_PORTS: %d out of range", p)
			}
		}
	}
	return nil
}

// ParseIdentities parses "pid,pid" or "username=pid,username=pid". Usernames
// default to the decimal PID. PIDs must be non-zero and distinct; usernames
// must be non-empty and distinct.
func ParseIdentities(spec string) ([]Identity, error) {
	var out []Identity
	seenPID := map[uint64]bool{}
	seenName := map[string]bool{}
	for _, tok := range splitList(spec) {
		name, pidStr, hasName := strings.Cut(tok, "=")
		if !hasName {
			pidStr, name = tok, tok
		}
		name = strings.TrimSpace(name)
		pid, err := strconv.ParseUint(strings.TrimSpace(pidStr), 10, 64)
		if err != nil || pid == 0 {
			return nil, fmt.Errorf("%q: PID must be a non-zero decimal integer", tok)
		}
		if name == "" {
			return nil, fmt.Errorf("%q: empty username", tok)
		}
		if seenPID[pid] || seenName[name] {
			return nil, fmt.Errorf("%q: duplicate identity", tok)
		}
		seenPID[pid], seenName[name] = true, true
		out = append(out, Identity{Username: name, PID: pid})
	}
	if len(out) == 0 {
		return nil, errors.New("no identities")
	}
	return out, nil
}

// IsPrivateIPv4 reports whether s is an IPv4 literal in a private, loopback,
// link-local or CGNAT (100.64/10) range.
func IsPrivateIPv4(s string) bool {
	a, err := netip.ParseAddr(s)
	if err != nil || !a.Is4() {
		return false
	}
	if a.IsPrivate() || a.IsLoopback() || a.IsLinkLocalUnicast() {
		return true
	}
	cgnat := netip.MustParsePrefix("100.64.0.0/10")
	return cgnat.Contains(a)
}

func requirePrivateIPv4(name, v string) error {
	if !IsPrivateIPv4(v) {
		return fmt.Errorf("%s: %q must be a private/loopback IPv4 literal (fail closed: no public or hostname destinations)", name, v)
	}
	return nil
}

func orDefault(v, def string) string {
	if strings.TrimSpace(v) == "" {
		return def
	}
	return strings.TrimSpace(v)
}

func intOr(v string, def int) (int, error) {
	if strings.TrimSpace(v) == "" {
		return def, nil
	}
	return strconv.Atoi(strings.TrimSpace(v))
}

func splitList(v string) []string {
	var out []string
	for _, p := range strings.Split(v, ",") {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func parseIntValues(v string) (map[int]int32, error) {
	out := map[int]int32{}
	for _, part := range splitList(v) {
		k, val, ok := strings.Cut(part, "=")
		if !ok {
			return nil, fmt.Errorf("%q: expected key=value", part)
		}
		ki, err1 := strconv.Atoi(strings.TrimSpace(k))
		vi, err2 := strconv.ParseInt(strings.TrimSpace(val), 10, 32)
		if err1 != nil || err2 != nil || ki < 0 || ki > 0xFFFF {
			return nil, fmt.Errorf("%q: bad key or value", part)
		}
		out[ki] = int32(vi)
	}
	return out, nil
}

// Test seeds are absolute current ratings: PID:fighter-slot:rating, separated
// by commas. No guessed global Elite threshold or win/loss formula is used.
func parseFighterSeeds(spec string) (map[uint64]map[byte]uint32, error) {
	seeds := map[uint64]map[byte]uint32{}
	for _, entry := range splitList(spec) {
		parts := strings.Split(entry, ":")
		if len(parts) != 3 {
			return nil, errors.New("NSOLAN_FIGHTER_SEEDS expects PID:slot:rating")
		}
		pid, e1 := strconv.ParseUint(parts[0], 10, 64)
		key, e2 := strconv.ParseUint(parts[1], 10, 8)
		rating, e3 := strconv.ParseUint(parts[2], 10, 32)
		if e1 != nil || e2 != nil || e3 != nil || pid == 0 || key >= 99 {
			return nil, errors.New("invalid fighter seed PID, slot or rating")
		}
		if seeds[pid] == nil {
			seeds[pid] = map[byte]uint32{}
		}
		if _, exists := seeds[pid][byte(key)]; exists {
			return nil, errors.New("duplicate fighter seed")
		}
		seeds[pid][byte(key)] = uint32(rating)
	}
	return seeds, nil
}

// SortedIdentities returns the identities ordered by PID (stable logging).
func (c *Config) SortedIdentities() []Identity {
	out := append([]Identity(nil), c.Identities...)
	sort.Slice(out, func(i, j int) bool { return out[i].PID < out[j].PID })
	return out
}
