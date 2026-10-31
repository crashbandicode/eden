package autolan

import (
	"encoding/json"
	"net/netip"
	"os"
	"path/filepath"
	"testing"
)

// TestExportReadyForEdenParser writes, through the production writer
// (writeReady/readyContents), the ready.json a broker publishes for the fixed
// identity that Eden's C++ configuration probe expects in its "automatic" mode
// (tests/nso_lan_fixture/config_probe.cpp: 10.253.1.2, account 49153, auth
// proxy 38443, runtime directory /run/user/1000/eden-lan/i-test). It also
// writes a labelled negative control that differs only by the pre-fix
// allowlist. It runs only when AUTOLAN_READY_EXPORT_DIR is set; the C++ probe
// is run on both outputs separately (cross-language acceptance).
func TestExportReadyForEdenParser(t *testing.T) {
	dir := os.Getenv("AUTOLAN_READY_EXPORT_DIR")
	if dir == "" {
		t.Skip("cross-language export only")
	}
	const probeDir = "/run/user/1000/eden-lan/i-test"
	b := &Broker{titleInfo: supportedTitles["01006A800016E000"], runtimeDir: dir,
		ctlPath: probeDir + "/" + controlSocketName, dataPath: probeDir + "/" + dataSocketName,
		authPort: 38443, securePort: 38444, zncPort: 38445}
	if err := b.setLease(49153, netip.MustParseAddr("10.253.1.2")); err != nil {
		t.Fatal(err)
	}
	if err := b.writeReady(); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, readyFileName))
	if err != nil {
		t.Fatal(err)
	}
	var raw map[string]any
	if err := json.Unmarshal(data, &raw); err != nil {
		t.Fatal(err)
	}
	raw["allowed_ips"] = []string{"10.254.0.1", "10.254.0.2"} // negative control: pre-fix shape
	neg, err := json.Marshal(raw)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "ready-prefix-allowlist.json"), append(neg, '\n'), 0o600); err != nil {
		t.Fatal(err)
	}
}
