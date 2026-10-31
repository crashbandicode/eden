package nsoserver

import (
	"bytes"
	"encoding/json"
	"fmt"
	"reflect"
	"strings"
	"testing"

	nex "github.com/NextendoNetwork/nextendo-nex"
)

func TestDiagnosticStationAllowlist(t *testing.T) {
	raw := "prudp:/address=192.168.0.137;port=1;CID=2;RVCID=3;type=11;PID=18446744073709551615;token=DO_NOT_LOG;key=DO_NOT_LOG;Pa=DO_NOT_LOG;unknown=DO_NOT_LOG"
	got := diagnosticStation(raw)
	want := map[string]any{"address": "192.168.0.137", "port": uint64(1), "CID": uint64(2), "RVCID": uint64(3), "type": uint64(11), "PID": ^uint64(0)}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("allowlisted endpoint fields mismatch: %v", got)
	}
	for _, raw := range []string{
		"not a station DO_NOT_LOG",
		"prudp:/address=DO_NOT_LOG;port=DO_NOT_LOG;CID=DO_NOT_LOG;RVCID=DO_NOT_LOG;type=DO_NOT_LOG;PID=DO_NOT_LOG",
		"prudp:/address=192.168.0.1\nDO_NOT_LOG;port=65536;CID=4294967296;RVCID=-1;type=-1;PID=18446744073709551616",
	} {
		if got := diagnosticStation(raw); len(got) != 0 {
			t.Fatalf("invalid endpoint values admitted: %v", got)
		}
	}
}

func TestRMCDiagnosticStationRequests(t *testing.T) {
	settings := nex.NewSwitchSettings("fixture", 40000)
	station := "prudp:/address=192.168.0.137;port=1;CID=5;RVCID=7;type=11;PID=49153;token=DO_NOT_LOG;key=DO_NOT_LOG;extra=DO_NOT_LOG"
	for _, protocol := range []uint16{nex.ProtocolNATTraversal, nex.ProtocolSecureConnection} {
		t.Run(fmt.Sprint(protocol), func(t *testing.T) {
			out := nex.NewStreamOut(settings)
			out.U32(1)
			out.String(station)
			method := uint32(1)
			if protocol == nex.ProtocolNATTraversal {
				method = 3
				out.String(station)
			}
			body := append([]byte(nil), out.Bytes()...)
			req := &nex.RMCMessage{Protocol: protocol, Method: method, CallID: 9, Body: body}
			conn := &nex.Connection{Settings: settings, PID: 49154}
			var log strings.Builder
			server := &Server{Logf: func(format string, args ...any) { fmt.Fprintf(&log, format+"\n", args...) }}
			server.logRMC("secure")(conn, req)
			for _, forbidden := range []string{"DO_NOT_LOG", "token", "key", "extra", "prudp:/"} {
				if strings.Contains(log.String(), forbidden) {
					t.Fatalf("diagnostic log leaked %q", forbidden)
				}
			}
			for _, expected := range []string{`"address":"192.168.0.137"`, `"port":1`, `"CID":5`, `"RVCID":7`, `"PID":49153`, `"type":11`} {
				if !strings.Contains(log.String(), expected) {
					t.Errorf("missing endpoint field %s", expected)
				}
			}
			if !bytes.Equal(req.Body, out.Bytes()) {
				t.Fatal("diagnostics changed the request body")
			}
			for length := 0; length < len(body); length++ {
				truncated := *req
				truncated.Body = body[:length]
				if got := rmcDiagnostic(conn, &truncated); got != "malformed=true" {
					t.Fatalf("truncated length %d: %s", length, got)
				}
			}
			req.Body = append(body, []byte("DO_NOT_LOG")...)
			if got := rmcDiagnostic(conn, req); got != "malformed=true" {
				t.Fatalf("trailing body should be rejected: %s", got)
			}
		})
	}
}

func TestRMCDiagnosticNATReports(t *testing.T) {
	settings := nex.NewSwitchSettings("fixture", 40000)
	conn := &nex.Connection{Settings: settings}
	for _, method := range []uint32{4, 5, 7} {
		t.Run(fmt.Sprint(method), func(t *testing.T) {
			out := nex.NewStreamOut(settings)
			var want string
			if method == 5 {
				out.U32(2)
				out.U32(3)
				out.U32(45)
				want = `{"nat_mapping":2,"nat_filtering":3,"rtt":45}`
			} else {
				out.U32(23)
				out.Bool(false)
				if method == 7 {
					out.S32(-12)
					want = `{"cid":23,"result":false,"detail":-12,"rtt":45}`
				} else {
					want = `{"cid":23,"result":false,"rtt":45}`
				}
				out.U32(45)
			}
			req := &nex.RMCMessage{Protocol: 3, Method: method, Body: out.Bytes()}
			var gotFields, wantFields map[string]any
			got := rmcDiagnostic(conn, req)
			if json.Unmarshal([]byte(got), &gotFields) != nil || json.Unmarshal([]byte(want), &wantFields) != nil || !reflect.DeepEqual(gotFields, wantFields) {
				t.Fatalf("report fields: %s; want %s", got, want)
			}
			for length := 0; length < len(req.Body); length++ {
				short := *req
				short.Body = req.Body[:length]
				if rmcDiagnostic(conn, &short) != "malformed=true" {
					t.Fatalf("accepted truncated report length %d", length)
				}
			}
		})
	}
}

func TestRMCDiagnosticMalformedAndUnsupported(t *testing.T) {
	conn := &nex.Connection{Settings: nex.NewSwitchSettings("fixture", 40000)}
	for _, method := range []uint32{1, 2, 6, 99} {
		if got := rmcDiagnostic(conn, &nex.RMCMessage{Protocol: 3, Method: method, Body: []byte("DO_NOT_LOG")}); got != "" {
			t.Fatalf("unsupported method %d logged payload: %s", method, got)
		}
	}
	for _, req := range []*nex.RMCMessage{
		{Protocol: 3, Method: 3, Body: []byte{33, 0, 0, 0}},
		{Protocol: 11, Method: 1, Body: []byte{255, 255, 255, 255}},
		{Protocol: 3, Method: 4, Body: []byte{0, 0, 0, 0, 2, 0, 0, 0, 0}},
	} {
		if got := rmcDiagnostic(conn, req); got != "malformed=true" {
			t.Fatalf("malformed request accepted: %s", got)
		}
	}
}

// Exercise the real log boundary: migration metadata must not expose extra
// request bytes or consume/mutate the body later dispatched to matchmaking.
func TestRMCDiagnosticHostMigrationBoundary(t *testing.T) {
	settings := nex.NewSwitchSettings("fixture", 40000)
	conn := &nex.Connection{Settings: settings, PID: 49153}
	for _, method := range []uint32{nex.MethodUpdateSessionHostV1, nex.MethodUpdateSessionHost} {
		for _, migrate := range []bool{false, true} {
			out := nex.NewStreamOut(settings)
			out.U32(17)
			if method == nex.MethodUpdateSessionHost {
				out.Bool(migrate)
			}
			body := append([]byte(nil), out.Bytes()...)
			req := &nex.RMCMessage{Protocol: nex.ProtocolMatchMaking, Method: method, Body: body}
			var log strings.Builder
			s := &Server{Logf: func(format string, args ...any) { fmt.Fprintf(&log, format+"\n", args...) }}
			s.logRMC("secure")(conn, req)
			if !strings.Contains(log.String(), `"gathering_id":17`) || !bytes.Equal(req.Body, out.Bytes()) {
				t.Fatal("migration metadata missing or request mutated")
			}
			if method == nex.MethodUpdateSessionHost && !strings.Contains(log.String(), fmt.Sprintf(`"migrate_owner":%t`, migrate)) {
				t.Fatal("migration flag missing or changed")
			}
			for length := 0; length < len(body); length++ {
				req.Body = body[:length]
				if rmcDiagnostic(conn, req) != "malformed=true" {
					t.Fatal("truncated migration body accepted")
				}
			}
			log.Reset()
			req.Body = append(append([]byte(nil), body...), []byte("DO_NOT_LOG")...)
			s.logRMC("secure")(conn, req)
			if strings.Contains(log.String(), "DO_NOT_LOG") || !strings.Contains(log.String(), "malformed=true") {
				t.Fatal("trailing migration bytes crossed diagnostic boundary")
			}
		}
	}
}
