package fixture

import "testing"

func TestDynamicConfigIsExplicitAndStrictDefaultUnchanged(t *testing.T) {
	c, err := NewDynamic("10.254.0.1", "")
	if err != nil {
		t.Fatal(err)
	}
	if !c.Dynamic || len(c.Identities) != 0 || c.NATCheck.Enabled || c.SecurePort != DefaultSecurePort || c.SessionStations != "lan" {
		t.Fatalf("unexpected dynamic config: %+v", c)
	}
	c.Identities = []Identity{{Username: "1", PID: 1}}
	if c.Validate() == nil {
		t.Fatal("dynamic mode accepted static identities")
	}
	c.Identities = nil
	c.NATCheck = NATCheck{Enabled: true, Addrs: []string{"10.254.0.1"}, Ports: []int{10025}}
	if c.Validate() == nil {
		t.Fatal("dynamic mode accepted host NAT-check sockets")
	}
	if _, err := NewDynamic("8.8.8.8", ""); err == nil {
		t.Fatal("dynamic mode accepted a public advertise address")
	}
	// The env fixture never enables dynamic mode and still requires two identities.
	env := map[string]string{"NSOLAN_ADVERTISE_HOST": "192.168.10.20", "NSOLAN_IDENTITIES": "49153"}
	if _, err := FromEnv(func(k string) string { return env[k] }); err == nil {
		t.Fatal("strict fixture accepted one identity")
	}
	env["NSOLAN_IDENTITIES"] = "49153,49154"
	strict, err := FromEnv(func(k string) string { return env[k] })
	if err != nil || strict.Dynamic {
		t.Fatalf("strict fixture changed: dynamic=%v err=%v", strict != nil && strict.Dynamic, err)
	}
}
