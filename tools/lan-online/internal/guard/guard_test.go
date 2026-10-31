// Package guard holds static checks that the backend and its pinned dependency
// contain no outbound network calls and no production service names.
package guard

import (
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func moduleRoot(t *testing.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatal("go.mod not found")
		}
		dir = parent
	}
}

// outbound matches code that would open a client connection or resolve a name.
var outbound = regexp.MustCompile(`\bhttp\.(Get|Post|Head|PostForm|NewRequest|NewRequestWithContext|Client|DefaultClient)\b|\bnet\.(Dial|DialTimeout|DialUDP|DialTCP|Dialer|LookupHost|LookupIP|LookupAddr)\b|\btls\.Dial(WithDialer)?\b|\bDialContext\b|\bwebsocket\.(Dial|DefaultDialer|Dialer)\b|\bgws\.NewClient\b|\bexec\.Command\b`)

// sends matches writes to an explicit destination (raw sends, unconnected
// UDP/Unix datagram writes) and the remaining client constructors.
var sends = regexp.MustCompile(`\bsyscall\.(Sendto|Sendmsg)\b|\.WriteTo(UDP|UDPAddrPort|Unix|MsgUDP|MsgUDPAddrPort|MsgUnix)?\(|\bnet\.(DialUnix|DialIP)\b`)

// production matches hostnames of real services the fixture must never reach.
var production = regexp.MustCompile(`(?i)nintendo\.net|nextendo\.network|nintendo\.com|baas\.|accounts\.nintendo`)

// Reviewed exceptions, each confined to named functions of one non-test file.
var (
	// The automatic runtime's only outbound connections: to the elected coordinator
	// (dialCoordinator) and, on Windows, to a same-host peer broker's data stream
	// (dialLocalPeer). Each must begin with its private-destination check.
	coordinatorDialFile = filepath.Join("internal", "autolan", "dial.go")
	confinedDials       = map[string][]string{coordinatorDialFile: {"dialCoordinator", "dialLocalPeer"}}
	dialChecks          = map[string]string{"dialCoordinator": "coordinatorDestinationAllowed", "dialLocalPeer": "peerDestinationAllowed"}

	// The only explicit-destination sends: the broker's datagram routing (to
	// validated peers and owned clients) in the platform transport files, the
	// TTL-1 discovery beacon, and the fixture NAT-check reply to the probe's own source.
	confinedSends = map[string][]string{
		filepath.Join("internal", "autolan", "transport_linux.go"): {"(*unixgramTransport).sendUnix"},
		filepath.Join("internal", "autolan", "sys_linux.go"):       {"udpSend"},
		filepath.Join("internal", "autolan", "sys_windows.go"):     {"udpSend"},
		filepath.Join("internal", "autolan", "discovery.go"):       {"(*mcastAnnouncer).sendLoop", "(*mcastAnnouncer).Publish"},
		filepath.Join("internal", "natcheck", "responder.go"):      {"(*Responder).serveOne"},
	}

	// The DNS-override host names handed to Eden in ready.json; they map to
	// virtual addresses and are never resolved or dialed.
	titleTableFile = filepath.Join("internal", "autolan", "titles.go")
)

// Test-only allowances: re-executing the test binary itself, and TLS to an
// in-process loopback listener. Explicit-destination sends in tests address
// sockets the tests create themselves and are not restricted.
var testAllowances = []string{"exec.Command(os.Args[0],", `tls.Dial("tcp4", "127.0.0.1:"`}

func goFiles(t *testing.T, root string, includeTests bool) []string {
	t.Helper()
	var out []string
	err := filepath.WalkDir(root, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			if d.Name() == "toolchain" || d.Name() == ".git" {
				return filepath.SkipDir
			}
			return nil
		}
		if !strings.HasSuffix(p, ".go") {
			return nil
		}
		if !includeTests && strings.HasSuffix(p, "_test.go") {
			return nil
		}
		out = append(out, p)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// funcName renders a declaration as "name" or "(*Recv).name"/"(Recv).name".
func funcName(fd *ast.FuncDecl) string {
	if fd.Recv == nil || len(fd.Recv.List) == 0 {
		return fd.Name.Name
	}
	switch r := fd.Recv.List[0].Type.(type) {
	case *ast.StarExpr:
		if id, ok := r.X.(*ast.Ident); ok {
			return "(*" + id.Name + ")." + fd.Name.Name
		}
	case *ast.Ident:
		return "(" + r.Name + ")." + fd.Name.Name
	}
	return "?." + fd.Name.Name
}

// strayMatches reports every match of re in src outside the allowed functions.
func strayMatches(t *testing.T, name string, src []byte, re *regexp.Regexp, allowed []string) []string {
	t.Helper()
	fset := token.NewFileSet()
	file, err := parser.ParseFile(fset, name, src, 0)
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	type span struct {
		name       string
		start, end int
	}
	var spans []span
	for _, d := range file.Decls {
		if fd, ok := d.(*ast.FuncDecl); ok {
			spans = append(spans, span{funcName(fd), fset.Position(fd.Pos()).Offset, fset.Position(fd.End()).Offset})
		}
	}
	var out []string
	for _, m := range re.FindAllIndex(src, -1) {
		owner := ""
		for _, s := range spans {
			if m[0] >= s.start && m[1] <= s.end {
				owner = s.name
			}
		}
		ok := false
		for _, a := range allowed {
			ok = ok || (owner != "" && owner == a)
		}
		if !ok {
			out = append(out, string(src[m[0]:m[1]])+" in "+owner)
		}
	}
	return out
}

// destinationCheckFirst reports whether the named function begins with
// `if !<checker>(...) { ...; return ... }`, checker defaulting to coordinatorDestinationAllowed.
func destinationCheckFirst(t *testing.T, name string, src []byte, fn string) bool {
	checker := dialChecks[fn]
	if checker == "" {
		checker = "coordinatorDestinationAllowed"
	}
	return destinationCheckFirstBy(t, name, src, fn, checker)
}

func destinationCheckFirstBy(t *testing.T, name string, src []byte, fn, checker string) bool {
	t.Helper()
	file, err := parser.ParseFile(token.NewFileSet(), name, src, 0)
	if err != nil {
		t.Fatalf("%s: %v", name, err)
	}
	for _, d := range file.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok || funcName(fd) != fn || fd.Body == nil || len(fd.Body.List) == 0 {
			continue
		}
		is, ok := fd.Body.List[0].(*ast.IfStmt)
		if !ok || is.Init != nil || len(is.Body.List) == 0 {
			return false
		}
		u, ok := is.Cond.(*ast.UnaryExpr)
		if !ok || u.Op != token.NOT {
			return false
		}
		call, ok := u.X.(*ast.CallExpr)
		if !ok {
			return false
		}
		id, ok := call.Fun.(*ast.Ident)
		_, returns := is.Body.List[len(is.Body.List)-1].(*ast.ReturnStmt)
		return ok && id.Name == checker && returns
	}
	return false
}

// Our own packages: no outbound calls and no production hostnames anywhere,
// tests included (a test must not reach a real service either), except the
// reviewed function-scoped exceptions above.
func TestBackendSourcesHaveNoOutboundCallsOrProductionHosts(t *testing.T) {
	root := moduleRoot(t)
	sawDial := false
	for _, dir := range []string{"cmd", "internal"} {
		for _, f := range goFiles(t, filepath.Join(root, dir), true) {
			b, err := os.ReadFile(f)
			if err != nil {
				t.Fatal(err)
			}
			rel, _ := filepath.Rel(root, f)
			if strings.HasSuffix(f, "_test.go") {
				src := strings.ReplaceAll(string(b), "websocket.Dialer", "") // the WSS smoke test dials httptest only
				for _, allowed := range testAllowances {
					src = strings.ReplaceAll(src, allowed, "")
				}
				if m := outbound.FindString(src); m != "" {
					t.Errorf("%s: outbound call %q", f, m)
				}
			} else {
				for _, m := range strayMatches(t, f, b, outbound, confinedDials[rel]) {
					t.Errorf("%s: outbound call %s", f, m)
				}
				for _, m := range strayMatches(t, f, b, sends, confinedSends[rel]) {
					t.Errorf("%s: unreviewed send %s", f, m)
				}
				if rel == coordinatorDialFile {
					sawDial = true
					for _, fn := range confinedDials[rel] {
						if !destinationCheckFirst(t, f, b, fn) {
							t.Errorf("%s: %s must begin with the private-destination check", f, fn)
						}
					}
				}
			}
			if m := production.FindString(string(b)); m != "" && !strings.Contains(f, "config_test.go") && rel != titleTableFile {
				t.Errorf("%s: production host reference %q", f, m)
			}
		}
	}
	if !sawDial {
		t.Error("reviewed coordinator dial file not found")
	}
}

// withDial inserts the dial call at run time, so this checker file itself holds
// no outbound call for the scan above to find.
func withDial(src []byte) []byte {
	return []byte(strings.ReplaceAll(string(src), "DIAL", "net."+"DialTimeout"))
}

// The checker itself: synthetic sources with and without violations.
func TestConfinementCheckerFlagsStrayCalls(t *testing.T) {
	src := []byte(`package x
func (b *Broker) sendUDP() { syscall.Sendto(1, nil, 0, nil) }
func helper() { conn.WriteToUDP(nil, nil); u.WriteToUnix(nil, nil) }
`)
	if got := strayMatches(t, "x.go", src, sends, []string{"(*Broker).sendUDP"}); len(got) != 2 {
		t.Fatalf("expected the two sends in helper to be flagged, got %v", got)
	}
	good := []byte(`package x
func dialCoordinator(a string) (int, error) {
	if !coordinatorDestinationAllowed(a) {
		return 0, errRefused
	}
	return DIAL("unix", a, 0)
}
`)
	good = withDial(good)
	if len(strayMatches(t, "d.go", good, outbound, []string{"dialCoordinator"})) != 0 || !destinationCheckFirst(t, "d.go", good, "dialCoordinator") {
		t.Fatal("reviewed dial shape rejected")
	}
	second := append(append([]byte(nil), good...), withDial([]byte("func other() { DIAL(\"tcp4\", \"x\", 0) }\n"))...)
	if len(strayMatches(t, "d.go", second, outbound, []string{"dialCoordinator"})) != 1 {
		t.Fatal("a second dial path in the reviewed file was not flagged")
	}
	unchecked := []byte(`package x
func dialCoordinator(a string) (int, error) {
	ok := coordinatorDestinationAllowed(a)
	_ = ok
	return DIAL("unix", a, 0)
}
`)
	unchecked = withDial(unchecked)
	if destinationCheckFirst(t, "u.go", unchecked, "dialCoordinator") {
		t.Fatal("a dial without the leading destination check was accepted")
	}
}

// The pinned dependency's non-test sources: no outbound client calls and no
// production hostnames. (The dependency's README mentions its own service; the
// code must not contact it.)
func TestPinnedCoreHasNoOutboundCalls(t *testing.T) {
	root := moduleRoot(t)
	files := goFiles(t, filepath.Join(root, "third_party", "nextendo-nex"), false)
	if len(files) < 40 {
		t.Fatalf("expected the vendored core, found %d files", len(files))
	}
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if m := outbound.FindString(string(b)); m != "" {
			t.Errorf("%s: outbound call %q", f, m)
		}
		if m := production.FindString(string(b)); m != "" {
			t.Errorf("%s: production host reference %q", f, m)
		}
	}
}

// The repository must not carry captured game/server response bytes.
func TestNoCaptureBinariesInRepository(t *testing.T) {
	root := moduleRoot(t)
	err := filepath.WalkDir(root, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() && (d.Name() == "toolchain" || d.Name() == ".git") {
			return filepath.SkipDir
		}
		if !d.IsDir() && strings.HasSuffix(strings.ToLower(d.Name()), ".bin") {
			t.Errorf("binary capture file present: %s", p)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
}
