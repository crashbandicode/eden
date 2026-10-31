package autolan

// Service host names the guest resolves. They are only DNS-override keys handed
// to Eden in ready.json, each mapped to a virtual 10.254.0.x address; nothing in
// the runtime resolves or contacts them. internal/guard confines production
// host names in the backend to this file.

// titleInfo describes one supported title. Only the title the accepted fixture
// was measured with is listed; every other title is reported as unsupported.
type titleInfo struct {
	GameServerHost string
}

var supportedTitles = map[string]titleInfo{
	"01006A800016E000": {GameServerHost: "g23380901-lp1.s.n.srv.nintendo.net"},
}

const (
	nncs1Host = "nncs1-lp1.n.n.srv.nintendo.net"
	nncs2Host = "nncs2-lp1.n.n.srv.nintendo.net"
	zncHost   = "api-lp1.znc.srv.nintendo.net"
)
