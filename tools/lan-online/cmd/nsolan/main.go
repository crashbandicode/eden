// Command nsolan runs the private LAN NEX fixture for two Eden instances.
//
// Configuration is entirely from NSOLAN_* environment variables (see
// internal/fixture). It binds only private addresses, resolves only the two
// configured identities and never dials out.
//
// Standard output is discarded before the core starts because the pinned core
// prints per-login source keys and ticket bytes there; the fixture's own
// metadata-only log goes to standard error. Set NSOLAN_UPSTREAM_STDOUT=1 to keep
// the core's stdout for local protocol debugging (never in a shared log).
package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"

	"edennsolan/backend/internal/autolan"
	"edennsolan/backend/internal/fixture"
	"edennsolan/backend/internal/nsoserver"
	"edennsolan/backend/internal/quiet"
)

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds | log.LUTC)
	log.SetOutput(os.Stderr)
	if len(os.Args) > 1 {
		// Automatic LAN runtime (launched by Eden). Upstream stdout is always
		// discarded here; NSOLAN_UPSTREAM_STDOUT is ignored in this mode.
		if os.Args[1] != "--auto" {
			log.Fatalf("[nsolan] unknown arguments: use --auto ... or no arguments for the fixture")
		}
		if _, err := quiet.SuppressStdout(true); err != nil {
			log.Fatalf("[nsolan] cannot suppress stdout: %v", err)
		}
		os.Exit(autolan.Main(os.Args[2:]))
	}
	if os.Getenv("NSOLAN_UPSTREAM_STDOUT") != "1" {
		if _, err := quiet.SuppressStdout(true); err != nil {
			log.Fatalf("[nsolan] cannot suppress stdout: %v", err)
		}
		log.Printf("[nsolan] core stdout discarded (NSOLAN_UPSTREAM_STDOUT=1 keeps it)")
	} else {
		log.Printf("[nsolan] WARNING: core stdout kept; it contains per-login keys and tickets")
	}
	cfg, err := fixture.FromEnv(os.Getenv)
	if err != nil {
		log.Fatalf("[nsolan] config: %v", err)
	}
	srv, err := nsoserver.New(cfg, log.Printf)
	if err != nil {
		log.Fatalf("[nsolan] init: %v", err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := srv.ListenAndServe(ctx); err != nil {
		log.Fatalf("[nsolan] serve: %v", err)
	}
	log.Printf("[nsolan] stopped")
}
