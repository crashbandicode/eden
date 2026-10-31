// Package nsoserver wires the pinned Nextendo NEX core into the private LAN
// fixture: one auth (TicketGranting) endpoint and one secure endpoint carrying
// SecureConnection, matchmaking, NAT traversal, ranking, the Smash init-shape
// responder and the Pia type-8 keepalive ACK, both served as PRUDP-Lite over WSS.
package nsoserver

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	nex "github.com/NextendoNetwork/nextendo-nex"
	"github.com/lxzan/gws"

	"edennsolan/backend/internal/fixture"
	"edennsolan/backend/internal/identity"
	"edennsolan/backend/internal/initshape"
	"edennsolan/backend/internal/lanstations"
	"edennsolan/backend/internal/natcheck"
)

// piaKeepalivePacketType is the undocumented PRUDP packet type 8 that Pia 5.19
// sends as a keepalive (empty payload, NeedsAck). It must be ACKed or the game
// treats the connection as dead (upstream SSBU measurement).
const piaKeepalivePacketType uint8 = 8

// Server is the assembled fixture backend.
type Server struct {
	Cfg      *fixture.Config
	Logf     func(format string, args ...any)
	Resolver *identity.Resolver
	Init     *initshape.Responder

	AuthSettings   *nex.Settings
	SecureSettings *nex.Settings
	Auth           *nex.Endpoint
	Secure         *nex.Endpoint
	MM             *nex.Matchmaking
	SecureStation  *nex.StationURL

	// EndpointByPID supplies the automatic runtime's NNCS-observed virtual UDP
	// endpoint for an authenticated PID (dynamic mode only).
	EndpointByPID func(pid uint64) (netip.AddrPort, bool)

	authWS     *nex.Server
	secureWS   *nex.Server
	nat        *natcheck.Responder
	reapPasses atomic.Uint64
}

// New assembles the endpoints. Nothing listens until ListenAndServe.
func New(cfg *fixture.Config, logf func(string, ...any)) (*Server, error) {
	if err := cfg.Validate(); err != nil {
		return nil, err
	}
	if logf == nil {
		logf = func(string, ...any) {}
	}
	var res *identity.Resolver
	if cfg.Dynamic {
		res = identity.NewDynamic()
	} else {
		var err error
		if res, err = identity.New(cfg.Identities); err != nil {
			return nil, err
		}
	}
	res.Logf = logf

	// Upstream knobs the core reads from the environment. OpenParticipation must be
	// on so the second player is matched into the first player's gathering even when
	// the client's own param leaves the flag clear (the core only joins open ones).
	if os.Getenv("NEXTENDO_OPEN_PARTICIPATION") == "" {
		_ = os.Setenv("NEXTENDO_OPEN_PARTICIPATION", "1")
	}

	s := &Server{Cfg: cfg, Logf: logf, Resolver: res}
	s.AuthSettings = nex.NewSwitchSettings(cfg.AccessKey, cfg.NexVersion)
	s.SecureSettings = nex.NewSwitchSettings(cfg.AccessKey, cfg.NexVersion)
	// Secure endpoint negotiates PRUDP minor version 0 so the client presents its
	// Kerberos ticket in CONNECT (upstream SSBU measurement: answering 5 yields an
	// empty CONNECT and an unauthenticated session).
	s.SecureSettings.PrudpMinorVersion = 0

	securePassword, err := randomHex(16)
	if err != nil {
		return nil, err
	}

	s.SecureStation = nex.NewStationURL("prudps")
	s.SecureStation.Set("address", cfg.AdvertiseHost)
	s.SecureStation.SetInt("port", cfg.SecurePort)
	s.SecureStation.SetInt("CID", 1)
	s.SecureStation.SetInt("PID", int(cfg.SecurePID))
	s.SecureStation.SetInt("sid", 1)
	s.SecureStation.SetInt("stream", 10)
	s.SecureStation.SetInt("type", 2)

	// --- auth endpoint ---
	s.Auth = nex.NewEndpoint(s.AuthSettings)
	authCfg := &nex.AuthConfig{
		Settings:         s.AuthSettings,
		SecurePID:        cfg.SecurePID,
		SecurePassword:   securePassword,
		SecureStationURL: s.SecureStation,
		ServerName:       cfg.ServerName,
		SessionKeyLength: 32,
		ResolveUserEx:    res.Resolve,
	}
	s.Auth.Register(nex.ProtocolTicketGranting, authCfg.Handler())
	s.Auth.OnRMC = s.logRMC("auth")

	// --- secure endpoint ---
	s.Secure = nex.NewEndpoint(s.SecureSettings)
	s.Secure.SetSecureAccount(securePassword, cfg.SecurePID)
	s.MM = nex.NewMatchmaking()
	s.MM.Endpoint = s.Secure
	if cfg.Dynamic {
		s.MM.SessionStations = lanstations.PIDBuilder(lanstations.Mode(cfg.SessionStations), logf, func(pid uint64) (netip.AddrPort, bool) {
			if s.EndpointByPID == nil {
				return netip.AddrPort{}, false
			}
			return s.EndpointByPID(pid)
		})
	} else {
		s.MM.SessionStations = lanstations.Builder(lanstations.Mode(cfg.SessionStations), logf, func() []natcheck.Observation {
			if s.nat == nil {
				return nil
			}
			return s.nat.Observations()
		})
	}
	s.Secure.Register(nex.ProtocolSecureConnection, nex.SecureConnectionHandler())
	s.Secure.Register(nex.ProtocolMatchmakeExtension, s.MM.ExtensionHandler())
	s.Secure.Register(nex.ProtocolMatchMaking, s.MM.MatchMakingHandler())
	s.Secure.Register(nex.ProtocolMatchMakingExt, s.MM.MatchMakingExtHandler())
	s.Secure.Register(nex.ProtocolNATTraversal, nex.NATTraversalHandler())
	s.Secure.Register(nex.ProtocolRanking, nex.RankingHandler())
	s.Init = initshape.New(initshape.Options{
		FighterExchange:    cfg.FighterExchange,
		FighterStats:       cfg.FighterStats,
		UtilityIntSettings: cfg.UtilityIntSettings,
		UtilityIntValues:   cfg.UtilityIntValues,
		ReplayDir:          cfg.ReplayDir,
		Logf:               logf,
	})
	if cfg.FighterStateFile != "" {
		allowed := map[uint64]bool{}
		for _, identity := range cfg.Identities {
			allowed[identity.PID] = true
		}
		if err := s.Init.LoadFighterState(cfg.FighterStateFile, allowed, cfg.FighterSeeds); err != nil {
			return nil, err
		}
	}
	s.Init.Register(s.Secure)
	if cfg.ReplayDir != "" {
		logf("[initshape] operator override dir=%s files=%v (methods without a file keep the shape fallback)", cfg.ReplayDir, s.Init.Overrides())
	}
	s.Secure.RegisterCustomPacketHandler(piaKeepalivePacketType, func(c *nex.Connection, p *nex.Packet) {
		if p.HasFlag(nex.FlagNeedACK) {
			c.SendAck(p)
		}
	})
	s.Secure.OnRMC = s.logRMC("secure")
	s.Secure.OnConnect = func(c *nex.Connection) {
		logf("[secure] connected pid=%d rvcid=%d addr=%s", c.PID, c.ID, c.RemoteAddr)
	}
	s.Secure.OnDisconnect = func(c *nex.Connection) {
		logf("[secure] disconnected pid=%d rvcid=%d addr=%s", c.PID, c.ID, c.RemoteAddr)
		s.MM.RemovePlayer(c.PID)
	}

	s.authWS = nex.NewServer(s.Auth)
	s.secureWS = nex.NewServer(s.Secure)

	if cfg.NATCheck.Enabled {
		s.nat = &natcheck.Responder{
			Addrs:           cfg.NATCheck.Addrs,
			Ports:           cfg.NATCheck.Ports,
			ObservationFile: cfg.NATCheck.ObservationFile,
			Logf:            logf,
		}
	}
	return s, nil
}

// AuthHandler and SecureHandler expose the WebSocket upgrade handlers so a test
// or an alternative listener can serve them.
func (s *Server) AuthHandler() http.Handler   { return wsHandler(s.authWS, s.Logf) }
func (s *Server) SecureHandler() http.Handler { return wsHandler(s.secureWS, s.Logf) }

// wsHandler reproduces the core transport's upgrade policy (the Switch offers a
// non-RFC "NEX" subprotocol that strict upgraders reject, hence gws) while
// letting the caller choose the listener and TLS configuration.
func wsHandler(srv *nex.Server, logf func(string, ...any)) http.Handler {
	upgrader := gws.NewUpgrader(srv, &gws.ServerOption{
		ParallelEnabled: false, // PRUDP frames must be processed strictly in order
		Recovery:        gws.Recovery,
		ReadBufferSize:  64 * 1024,
		WriteBufferSize: 64 * 1024,
	})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.EqualFold(r.Header.Get("Upgrade"), "websocket") || r.Header.Get("Sec-WebSocket-Key") != "" {
			socket, err := upgrader.Upgrade(w, r)
			if err != nil {
				logf("[ws] upgrade failed from %s path=%q proto=%q: %v", r.RemoteAddr, r.URL.Path, r.Header.Get("Sec-WebSocket-Protocol"), err)
				return
			}
			go socket.ReadLoop()
			return
		}
		http.NotFound(w, r)
	})
}

// ListenAndServe binds the TLS listeners on BindHost and the NAT-check sockets,
// serves until ctx is done, then shuts down.
func (s *Server) ListenAndServe(ctx context.Context) error {
	cert, err := tls.LoadX509KeyPair(s.Cfg.TLSCert, s.Cfg.TLSKey)
	if err != nil {
		return fmt.Errorf("tls: %w (generate with scripts/make-cert.ps1)", err)
	}
	tlsCfg := ServerTLSConfig(cert)

	authLn, err := net.Listen("tcp4", net.JoinHostPort(s.Cfg.BindHost, fmt.Sprint(s.Cfg.AuthPort)))
	if err != nil {
		return fmt.Errorf("bind auth %s:%d: %w (ports below 1024 need privilege; set NSOLAN_AUTH_PORT=8443 and remap in Eden)", s.Cfg.BindHost, s.Cfg.AuthPort, err)
	}
	defer authLn.Close()
	secureLn, err := net.Listen("tcp4", net.JoinHostPort(s.Cfg.BindHost, fmt.Sprint(s.Cfg.SecurePort)))
	if err != nil {
		return fmt.Errorf("bind secure %s:%d: %w", s.Cfg.BindHost, s.Cfg.SecurePort, err)
	}
	defer secureLn.Close()

	if s.nat != nil {
		if err := s.nat.Listen(); err != nil {
			return err
		}
		s.Logf("[natcheck] responders on %v", s.nat.LocalAddrs())
		go s.nat.Serve(ctx)
	}

	s.Logf("[nsolan] auth wss://%s:%d  secure wss://%s:%d  advertise=%s identities=%v stations=%s",
		s.Cfg.BindHost, s.Cfg.AuthPort, s.Cfg.BindHost, s.Cfg.SecurePort, s.Cfg.AdvertiseHost, s.Cfg.SortedIdentities(), s.Cfg.SessionStations)
	return s.Serve(ctx, authLn, secureLn, tlsCfg)
}

// ServerTLSConfig is the TLS configuration both endpoints are served with.
func ServerTLSConfig(cert tls.Certificate) *tls.Config {
	return &tls.Config{
		Certificates: []tls.Certificate{cert},
		MinVersion:   tls.VersionTLS12,
		NextProtos:   []string{"http/1.1"}, // NEX needs the HTTP/1.1 upgrade; no h2
	}
}

// Serve serves the auth and secure WSS endpoints on caller-supplied listeners
// until ctx is done, then shuts both down. The automatic runtime passes
// listeners whose connections already carry the leased virtual RemoteAddr.
func (s *Server) Serve(ctx context.Context, authLn, secureLn net.Listener, tlsCfg *tls.Config) error {
	authSrv := &http.Server{Handler: s.AuthHandler(), TLSConfig: tlsCfg, TLSNextProto: map[string]func(*http.Server, *tls.Conn, http.Handler){}}
	secureSrv := &http.Server{Handler: s.SecureHandler(), TLSConfig: tlsCfg, TLSNextProto: map[string]func(*http.Server, *tls.Conn, http.Handler){}}

	go s.reapIdle(ctx)

	errc := make(chan error, 2)
	go func() { errc <- authSrv.ServeTLS(authLn, "", "") }()
	go func() { errc <- secureSrv.ServeTLS(secureLn, "", "") }()

	select {
	case <-ctx.Done():
	case err := <-errc:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			return err
		}
	}
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = authSrv.Shutdown(shutdownCtx)
	_ = secureSrv.Shutdown(shutdownCtx)
	return nil
}

// reapIdle evicts dead secure connections for the lifetime of one Serve call.
// The core's StartReaper runs once per process and stays bound to the first
// endpoint forever: a later server (a coordinator elected again in the same
// process) would get no reaper, and the first server would stay reachable. The
// eviction rule and NEXTENDO_REAP_IDLE_SECONDS are the core's own (ReapIdle,
// ReapIdleTimeout); NEXTENDO_REAP_EVERY_SECONDS keeps its meaning (default 60).
func (s *Server) reapIdle(ctx context.Context) {
	maxIdle := nex.ReapIdleTimeout()
	if maxIdle <= 0 {
		s.Logf("[reaper] idle eviction disabled (NEXTENDO_REAP_IDLE_SECONDS=0)")
		return
	}
	every := 60 * time.Second
	if n, err := strconv.Atoi(os.Getenv("NEXTENDO_REAP_EVERY_SECONDS")); err == nil && n > 0 {
		every = time.Duration(n) * time.Second
	}
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			if n := s.Secure.ReapIdle(maxIdle); n > 0 {
				s.Logf("[reaper] evicted %d idle connection(s)", n)
			}
			s.reapPasses.Add(1)
		}
	}
}

// ReapPasses counts completed eviction passes of this server's reaper.
func (s *Server) ReapPasses() uint64 { return s.reapPasses.Load() }

func (s *Server) logRMC(tag string) func(*nex.Connection, *nex.RMCMessage) {
	return func(c *nex.Connection, req *nex.RMCMessage) {
		s.Logf("[%s] pid=%d proto=0x%02x method=%d call=%d bodyLen=%d", tag, c.PID, req.Protocol, req.Method, req.CallID, len(req.Body))
		if diagnostic := rmcDiagnostic(c, req); diagnostic != "" {
			s.Logf("[%s] pid=%d proto=0x%02x method=%d call=%d diagnostic=%s", tag, c.PID, req.Protocol, req.Method, req.CallID, diagnostic)
		}
	}
}

// Only literal IPs and numeric endpoint fields cross the diagnostic boundary.
// Never format a StationURL, request body, or decoder error into the log.
func diagnosticStation(raw string) map[string]any {
	station := nex.ParseStationURL(raw)
	fields := make(map[string]any)
	if ip := net.ParseIP(station.Get("address")); ip != nil {
		fields["address"] = ip.String()
	}
	for _, field := range []struct {
		name string
		bits int
	}{{"port", 16}, {"CID", 32}, {"RVCID", 32}, {"type", 32}, {"PID", 64}} {
		if value, err := strconv.ParseUint(station.Get(field.name), 10, field.bits); err == nil {
			fields[field.name] = value
		}
	}
	return fields
}

func rmcDiagnostic(c *nex.Connection, req *nex.RMCMessage) string {
	in := nex.NewStreamIn(req.Body, c.Settings)
	var fields map[string]any
	switch {
	case req.Protocol == nex.ProtocolMatchMaking && (req.Method == nex.MethodUpdateSessionHost || req.Method == nex.MethodUpdateSessionHostV1):
		fields = map[string]any{"gathering_id": in.U32()}
		if req.Method == nex.MethodUpdateSessionHost {
			fields["migrate_owner"] = in.Bool()
		}
	case req.Protocol == nex.ProtocolNATTraversal && req.Method == 3:
		targets, ok := diagnosticStations(in)
		if !ok {
			return "malformed=true"
		}
		fields = map[string]any{"targets": targets, "caller": diagnosticStation(in.String())}
	case req.Protocol == nex.ProtocolSecureConnection && req.Method == 1:
		callers, ok := diagnosticStations(in)
		if !ok {
			return "malformed=true"
		}
		fields = map[string]any{"caller_stations": callers}
	case req.Protocol == nex.ProtocolNATTraversal && (req.Method == 4 || req.Method == 7):
		cid, result := in.U32(), in.U8()
		if result > 1 {
			return "malformed=true"
		}
		fields = map[string]any{"cid": cid, "result": result == 1}
		if req.Method == 7 {
			fields["detail"] = in.S32()
		}
		fields["rtt"] = in.U32()
	case req.Protocol == nex.ProtocolNATTraversal && req.Method == 5:
		fields = map[string]any{"nat_mapping": in.U32(), "nat_filtering": in.U32(), "rtt": in.U32()}
	default:
		return ""
	}
	if in.Err() != nil || in.Remaining() != 0 {
		return "malformed=true"
	}
	encoded, _ := json.Marshal(fields) // These fixed maps contain only JSON-supported values.
	return string(encoded)
}

func diagnosticStations(in *nex.StreamIn) ([]map[string]any, bool) {
	count := in.U32()
	// Diagnostics remain bounded even for a malformed or oversized station list.
	if in.Err() != nil || count > 32 || uint64(count)*2 > uint64(in.Remaining()) {
		return nil, false
	}
	stations := make([]map[string]any, 0, count)
	for range count {
		stations = append(stations, diagnosticStation(in.String()))
	}
	return stations, in.Err() == nil
}

func randomHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}
