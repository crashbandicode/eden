package fixture

import (
	"strings"
	"testing"
)

func env(m map[string]string) Getenv { return func(k string) string { return m[k] } }

func TestFromEnvDefaults(t *testing.T) {
	c, err := FromEnv(env(map[string]string{"NSOLAN_ADVERTISE_HOST": "192.168.50.10"}))
	if err != nil {
		t.Fatal(err)
	}
	if c.BindHost != "192.168.50.10" || c.AuthPort != 443 || c.SecurePort != 60005 {
		t.Fatalf("defaults: %+v", c)
	}
	if len(c.Identities) != 2 || c.Identities[0].PID != 49153 || c.Identities[1].PID != 49154 ||
		c.Identities[0].Username != "49153" || c.Identities[1].Username != "49154" {
		t.Fatalf("identities: %+v", c.Identities)
	}
	if c.UtilityIntSettings != 431 || c.SessionStations != "lan" || c.AccessKey != "9587602b" || c.NexVersion != 40000 {
		t.Fatalf("game constants: %+v", c)
	}
	if !c.NATCheck.Enabled || len(c.NATCheck.Addrs) != 1 || c.NATCheck.Addrs[0] != "192.168.50.10" ||
		len(c.NATCheck.Ports) != 2 || c.NATCheck.Ports[0] != 10025 || c.NATCheck.Ports[1] != 10125 {
		t.Fatalf("natcheck: %+v", c.NATCheck)
	}
}

// Fail closed: public addresses and hostnames are refused everywhere a
// destination can be configured, so the fixture cannot be pointed at a
// production service by mistake.
func TestFromEnvRejectsNonPrivateDestinations(t *testing.T) {
	cases := []map[string]string{
		{},
		{"NSOLAN_ADVERTISE_HOST": "8.8.8.8"},
		{"NSOLAN_ADVERTISE_HOST": "nncs1-lp1.n.n.srv.nintendo.net"},
		{"NSOLAN_ADVERTISE_HOST": "192.168.1.5", "NSOLAN_BIND_HOST": "0.0.0.0"},
		{"NSOLAN_ADVERTISE_HOST": "192.168.1.5", "NSOLAN_NATCHECK_ADDRS": "192.168.1.5,203.0.113.9"},
		{"NSOLAN_ADVERTISE_HOST": "::1"},
	}
	for i, m := range cases {
		if _, err := FromEnv(env(m)); err == nil {
			t.Errorf("case %d %v: expected rejection", i, m)
		}
	}
}

func TestFromEnvRejectsBadIdentities(t *testing.T) {
	for _, spec := range []string{"49153", "49153,49153", "0,1", "a=1,b=1", "x,y", "1,2,3", "=5,6"} {
		if _, err := FromEnv(env(map[string]string{"NSOLAN_ADVERTISE_HOST": "10.0.0.2", "NSOLAN_IDENTITIES": spec})); err == nil {
			t.Errorf("identities %q: expected rejection", spec)
		}
	}
}

func TestParseIdentitiesNamedForm(t *testing.T) {
	ids, err := ParseIdentities("slotA=49153, slotB=49154")
	if err != nil || len(ids) != 2 || ids[0].Username != "slotA" || ids[1].PID != 49154 {
		t.Fatalf("named form: %v %+v", err, ids)
	}
}

func TestFromEnvRejectsBadPortsAndModes(t *testing.T) {
	base := func(extra map[string]string) map[string]string {
		m := map[string]string{"NSOLAN_ADVERTISE_HOST": "10.1.2.3"}
		for k, v := range extra {
			m[k] = v
		}
		return m
	}
	bad := []map[string]string{
		base(map[string]string{"NSOLAN_AUTH_PORT": "0"}),
		base(map[string]string{"NSOLAN_AUTH_PORT": "70000"}),
		base(map[string]string{"NSOLAN_AUTH_PORT": "60005"}),
		base(map[string]string{"NSOLAN_SESSION_STATIONS": "nat"}),
		base(map[string]string{"NSOLAN_UTILITY_INT_SETTINGS": "-1"}),
		base(map[string]string{"NSOLAN_UTILITY_INT_VALUES": "a=b"}),
		base(map[string]string{"NSOLAN_NATCHECK_PORTS": "10025,x"}),
	}
	for i, m := range bad {
		if _, err := FromEnv(env(m)); err == nil {
			t.Errorf("case %d: expected error", i)
		}
	}
	c, err := FromEnv(env(base(map[string]string{"NSOLAN_UTILITY_INT_VALUES": "21=60, 3=5", "NSOLAN_NATCHECK": "0", "NSOLAN_AUTH_PORT": "8443"})))
	if err != nil {
		t.Fatal(err)
	}
	if c.UtilityIntValues[21] != 60 || c.UtilityIntValues[3] != 5 || c.NATCheck.Enabled || c.AuthPort != 8443 {
		t.Fatalf("values: %+v", c)
	}
}

func TestIsPrivateIPv4(t *testing.T) {
	yes := []string{"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.0.1", "127.0.0.1", "169.254.1.1", "100.64.0.1", "100.127.255.255"}
	no := []string{"172.32.0.1", "100.128.0.1", "8.8.8.8", "1.1.1.1", "::1", "fd00::1", "localhost", ""}
	for _, s := range yes {
		if !IsPrivateIPv4(s) {
			t.Errorf("%s should be private", s)
		}
	}
	for _, s := range no {
		if IsPrivateIPv4(s) {
			t.Errorf("%s should not be private", s)
		}
	}
}

func TestValidateErrorMentionsFailClosed(t *testing.T) {
	_, err := FromEnv(env(map[string]string{"NSOLAN_ADVERTISE_HOST": "1.2.3.4"}))
	if err == nil || !strings.Contains(err.Error(), "fail closed") {
		t.Fatalf("want fail-closed error, got %v", err)
	}
}
