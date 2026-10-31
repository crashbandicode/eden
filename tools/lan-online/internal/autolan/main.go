package autolan

import (
	"context"
	"errors"
	"io"
	"log"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

// Main runs `nsolan --auto ...` and returns the process exit code:
// 0 after a normal stop (parent exit or signal), 1 after a reported fatal
// condition (error.json), 2 for invalid arguments or an unusable runtime dir.
// The caller has already discarded standard output.
func Main(args []string) int {
	// The launcher may not drain stderr; logging must never stall the broker.
	sink := newDropWriter(os.Stderr, 1024)
	defer sink.Close()
	log.SetOutput(sink) // the pinned core's own log.Printf lines
	logger := log.New(sink, "", log.Ldate|log.Ltime|log.Lmicroseconds|log.LUTC)
	opt, err := ParseArgs(args)
	if err != nil {
		logger.Printf("[auto] %v", err)
		var se *StatusError
		if errors.As(err, &se) && opt.RuntimeDir != "" && checkPrivateDir(opt.RuntimeDir) == nil {
			_ = writeErrorStatus(opt.RuntimeDir, se)
		}
		return 2
	}
	opt.Logf = logger.Printf
	if err := checkPrivateDir(opt.RuntimeDir); err != nil {
		logger.Printf("[auto] runtime_dir_invalid: %v", err)
		return 2
	}
	// Any file the pinned core writes relative to its working directory stays
	// inside the private runtime directory.
	if err := os.Chdir(opt.RuntimeDir); err != nil {
		logger.Printf("[auto] runtime_dir_invalid: %v", err)
		return 2
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := Run(ctx, opt); err != nil {
		logger.Printf("[auto] stopped: %v", err)
		var se *StatusError
		if errors.As(err, &se) && se.Code == "runtime_dir_invalid" {
			return 2
		}
		return 1
	}
	return 0
}

// dropWriter hands log lines to one background writer and drops lines (counting
// them) when the queue is full, so a blocked stderr pipe cannot block callers.
type dropWriter struct {
	mu      sync.RWMutex
	closed  bool
	ch      chan []byte
	done    chan struct{}
	dropped atomic.Uint64
}

func newDropWriter(w io.Writer, depth int) *dropWriter {
	d := &dropWriter{ch: make(chan []byte, depth), done: make(chan struct{})}
	go func() {
		defer close(d.done)
		for line := range d.ch {
			_, _ = w.Write(line)
		}
	}()
	return d
}

func (d *dropWriter) Write(p []byte) (int, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	if d.closed {
		return len(p), nil
	}
	select {
	case d.ch <- append([]byte(nil), p...):
	default:
		d.dropped.Add(1)
	}
	return len(p), nil
}

// Close flushes queued lines for at most one second; later writes are dropped.
func (d *dropWriter) Close() {
	if n := d.dropped.Load(); n > 0 {
		_, _ = d.Write([]byte("[auto] " + strconv.FormatUint(n, 10) + " log lines dropped (stderr not drained)\n"))
	}
	d.mu.Lock()
	d.closed = true
	close(d.ch)
	d.mu.Unlock()
	select {
	case <-d.done:
	case <-time.After(time.Second):
	}
}
