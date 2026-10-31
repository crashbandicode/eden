package fixture

import (
	"path/filepath"
	"testing"
)

func TestFighterStatsConfig(t *testing.T) {
	for _, spec := range []string{"49153:7:10000001", "49153:7:0,49154:98:4294967295"} {
		cfg, err := FromEnv(func(k string) string {
			return map[string]string{"NSOLAN_ADVERTISE_HOST": "192.168.50.10", "NSOLAN_FIGHTER_STATS": "1", "NSOLAN_FIGHTER_STATE_FILE": filepath.Join(t.TempDir(), "ratings.json"), "NSOLAN_FIGHTER_SEEDS": spec}[k]
		})
		if err != nil || cfg == nil || !cfg.FighterStats || len(cfg.FighterSeeds) == 0 {
			t.Fatalf("valid config: %v", err)
		}
	}
	for _, spec := range []string{"0:7:1", "49155:7:1", "49153:99:1", "49153:7:4294967296", "49153:7:-1", "49153:7", "49153:7:1,49153:7:2"} {
		_, err := FromEnv(func(k string) string {
			return map[string]string{"NSOLAN_ADVERTISE_HOST": "192.168.50.10", "NSOLAN_FIGHTER_STATS": "1", "NSOLAN_FIGHTER_STATE_FILE": filepath.Join(t.TempDir(), "ratings.json"), "NSOLAN_FIGHTER_SEEDS": spec}[k]
		})
		if err == nil {
			t.Fatalf("accepted invalid seed %q", spec)
		}
	}
	cfg, err := NewDynamic("10.252.0.1", "")
	if err != nil {
		t.Fatal(err)
	}
	cfg.FighterStats = true
	if cfg.Validate() == nil {
		t.Fatal("enabled persistent PID data on dynamic leases")
	}
}
