package autolan

import (
	"flag"
	"fmt"
	"io"
	"net/netip"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Virtual addressing. Instances live in 10.253.0.0/16; the two virtual service
// addresses stand in for the game's auth/NNCS1 and NNCS2/ZNC hosts.
var (
	instanceNet = netip.MustParsePrefix("10.253.0.0/16")
	serviceNet  = netip.MustParsePrefix("10.254.0.0/16")
	serviceIP1  = netip.MustParseAddr("10.254.0.1")
	serviceIP2  = netip.MustParseAddr("10.254.0.2")
)

const (
	natPort1 = 10025
	natPort2 = 10125

	controlSocketName = "control.sock"
	dataSocketName    = "data.sock"
	readyFileName     = "ready.json"
	errorFileName     = "error.json"
	// stopFileName is created by a parent that cannot send a signal (Windows); see useStopFile.
	stopFileName = "stop"

	maxAssetBytes = 1 << 20
	maxSunPath    = 107 // sockaddr_un.sun_path minus the terminating NUL (108 on both platforms)

	// LAN coordinator exchanges. LAN round trips take milliseconds; these bounds
	// keep a silently dropping or pretending peer from consuming the frontend's
	// 10 s readiness deadline (ready.json is due well within 8 s).
	lanDialTimeout            = 750 * time.Millisecond
	defaultJoinAttemptTimeout = time.Second
	defaultStartupBudget      = 5 * time.Second
	serverTLSHandshakeTimeout = 2 * time.Second

	// minUnixDgramQlen: below this net.unix.max_dgram_qlen the runtime warns
	// (the kernel default is 10). The sysctl is only read, never changed.
	minUnixDgramQlen = 128
)

// requiredAssets are the reviewed initialization replies the coordinator serves.
// Their bytes are private install inputs: they are opened, never parsed here.
var requiredAssets = []string{
	"0x6e_7.bin", "0x6e_9.bin", "0x6e_10.bin", "0x6e_11.bin",
	"0x73_21.bin", "0x73_60.bin", "0x73_61.bin", "0x73_84.bin", "0x73_95.bin",
}

// Options configures one broker. The CLI fills the first block; the second
// block exists for tests and for deterministic injection only.
type Options struct {
	ParentPID  int
	RuntimeDir string
	AssetsDir  string
	TitleID    string
	Interface  string
	// ProfileDir is stable local storage selected by Eden's account/title.
	// Empty permits legacy/test callers, whose ratings remain process-local.
	ProfileDir   string
	FighterSeeds string

	// ControlTLS (CLI --control-tls) runs cross-host coordinator connections
	// (membership, NAT records, NEX streams) over TLS 1.3 pinned to the
	// coordinator's advertised certificate fingerprint. It is part of discovery
	// compatibility: brokers only join and elect with brokers of the same mode.
	// Same-host IPC stays Unix and gameplay datagrams are unchanged.
	ControlTLS bool

	// LocalOnly (CLI --local-only) disables the LAN path entirely: no NIC
	// discovery, no UDP socket, no multicast, no coordinator TCP listener.
	// Same-host instances still coordinate and exchange datagrams over AF_UNIX.
	LocalOnly bool

	// SharedDir overrides the per-user discovery directory.
	SharedDir string
	// LANIP overrides NIC discovery; it must be a private LAN address (loopback
	// only with the unexported test option allowLoopback).
	LANIP netip.Addr
	// LocalKey overrides the same-host key (tests simulate separate hosts).
	LocalKey string
	// Network supplies LAN beacons; nil selects UDP multicast on the chosen NIC.
	Network Announcer
	// Instance overrides the random instance id (tests force election order).
	Instance string
	// ControlPeerPID is the only process allowed on control.sock (default ParentPID).
	ControlPeerPID int
	// Timings (zero selects the production value).
	DiscoveryWindow time.Duration
	LeaderWait      time.Duration
	MemberTimeout   time.Duration
	LeaderTimeout   time.Duration
	PruneAfter      time.Duration
	// LeaderCooldown is how long a leader that failed joinFailuresBeforeCooldown
	// consecutive joins/probes is ignored by elections.
	LeaderCooldown time.Duration
	// Logf receives metadata-only log lines (stderr in production).
	Logf func(format string, args ...any)
	// coordHost replaces the NEX host (tests with a labelled mock NEX server).
	coordHost func(c *Coordinator) (nexHost, error)
	// allowLoopback admits 127.0.0.0/8 as a LAN, peer, beacon and coordinator
	// address. Test-only: production accepts RFC 1918 addresses only.
	allowLoopback bool
	// tlsHandshakeTimeout bounds the coordinator side of a control TLS handshake.
	// (A client's handshake is bounded by its attempt deadline.)
	tlsHandshakeTimeout time.Duration
	// joinAttemptTimeout bounds one whole coordinator exchange (dial, TLS,
	// preface and reply) for joins, streams and probes.
	joinAttemptTimeout time.Duration
	// startupBudget is how long a starting broker considers network leaders and
	// candidates before it leads itself (unless a same-host coordinator exists).
	startupBudget time.Duration
}

func (o *Options) withDefaults() {
	if o.ControlPeerPID == 0 {
		o.ControlPeerPID = o.ParentPID
	}
	def := func(d *time.Duration, v time.Duration) {
		if *d == 0 {
			*d = v
		}
	}
	def(&o.DiscoveryWindow, time.Second)
	def(&o.LeaderWait, 3*time.Second)
	def(&o.MemberTimeout, 5*time.Second)
	def(&o.LeaderTimeout, 3*time.Second)
	def(&o.PruneAfter, 10*time.Second)
	def(&o.LeaderCooldown, 30*time.Second)
	def(&o.tlsHandshakeTimeout, serverTLSHandshakeTimeout)
	def(&o.joinAttemptTimeout, defaultJoinAttemptTimeout)
	def(&o.startupBudget, defaultStartupBudget)
	if o.Logf == nil {
		o.Logf = func(string, ...any) {}
	}
}

// StatusError is a fatal condition reported through error.json.
type StatusError struct {
	Code    string
	Message string
}

func (e *StatusError) Error() string { return e.Code + ": " + e.Message }

func statusErr(code, format string, args ...any) *StatusError {
	return &StatusError{Code: code, Message: fmt.Sprintf(format, args...)}
}

// ParseArgs parses the arguments that follow --auto.
func ParseArgs(args []string) (Options, error) {
	fs := flag.NewFlagSet("nsolan --auto", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	var o Options
	parent := fs.String("parent-pid", "", "")
	fs.StringVar(&o.RuntimeDir, "runtime-dir", "", "")
	fs.StringVar(&o.AssetsDir, "assets", "", "")
	fs.StringVar(&o.TitleID, "title-id", "", "")
	fs.StringVar(&o.Interface, "interface", "", "")
	fs.StringVar(&o.ProfileDir, "profile-dir", os.Getenv("EDEN_LAN_PROFILE_DIR"), "")
	fs.StringVar(&o.FighterSeeds, "fighter-seeds", os.Getenv("EDEN_LAN_FIGHTER_SEEDS"), "")
	fs.BoolVar(&o.LocalOnly, "local-only", false, "")
	fs.BoolVar(&o.ControlTLS, "control-tls", false, "")
	if err := fs.Parse(args); err != nil {
		return o, statusErr("bad_arguments", "%v", err)
	}
	if fs.NArg() != 0 {
		return o, statusErr("bad_arguments", "unexpected positional arguments")
	}
	if o.LocalOnly && o.Interface != "" {
		return o, statusErr("bad_arguments", "--local-only and --interface are mutually exclusive")
	}
	pid, err := strconv.Atoi(*parent)
	if err != nil || pid <= 1 {
		return o, statusErr("bad_arguments", "--parent-pid must be a process id greater than 1")
	}
	o.ParentPID = pid
	if o.RuntimeDir == "" || o.AssetsDir == "" || o.TitleID == "" {
		return o, statusErr("bad_arguments", "--runtime-dir, --assets and --title-id are required")
	}
	// Linux keeps the arguments byte for byte; Windows accepts the launcher's forward slashes.
	o.RuntimeDir = normalizeDir(o.RuntimeDir)
	o.AssetsDir = normalizeDir(o.AssetsDir)
	if o.ProfileDir != "" {
		o.ProfileDir = normalizeDir(o.ProfileDir)
	}
	if o.FighterSeeds != "" && o.ProfileDir == "" {
		return o, statusErr("bad_arguments", "fighter seeds require a profile directory")
	}
	return o, nil
}

func normalizeTitle(id string) (string, titleInfo, error) {
	if len(id) != 16 {
		return "", titleInfo{}, statusErr("bad_arguments", "--title-id must be 16 hex digits")
	}
	if _, err := strconv.ParseUint(id, 16, 64); err != nil {
		return "", titleInfo{}, statusErr("bad_arguments", "--title-id must be 16 hex digits")
	}
	id = strings.ToUpper(id)
	info, ok := supportedTitles[id]
	if !ok {
		return id, titleInfo{}, statusErr("unsupported_title", "title %s is not supported by the LAN runtime", id)
	}
	return id, info, nil
}

func validateAssets(dir string) error {
	if !filepath.IsAbs(dir) {
		return statusErr("missing_assets", "--assets must be absolute")
	}
	for _, name := range requiredAssets {
		p := filepath.Join(dir, name)
		fi, err := os.Lstat(p)
		if err != nil {
			return statusErr("missing_assets", "initialization reply %s is missing", name)
		}
		if !fi.Mode().IsRegular() {
			return statusErr("missing_assets", "initialization reply %s is not a regular file", name)
		}
		if fi.Size() < 1 || fi.Size() > maxAssetBytes {
			return statusErr("missing_assets", "initialization reply %s has an invalid size", name)
		}
		f, err := os.OpenFile(p, os.O_RDONLY|noFollow, 0)
		if err != nil {
			return statusErr("missing_assets", "initialization reply %s is not readable", name)
		}
		_ = f.Close()
	}
	return nil
}

// forbiddenEnvironment lists core persistence variables that would make the
// pinned core write state outside the private runtime directory.
func forbiddenEnvironment() []string {
	var out []string
	for _, kv := range os.Environ() {
		k, _, _ := strings.Cut(kv, "=")
		if strings.HasPrefix(k, "NEXTENDO_") && strings.HasSuffix(k, "_FILE") {
			out = append(out, k)
		}
	}
	return out
}
