package initshape

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
)

// The fixed fixture has stable configured PIDs. Automatic coordinator leases
// do not: do not attach this store to dynamic mode without profile ownership.
// One companion owns a state file. It is not a multi-writer database.
type fighterState struct {
	Version int             `json:"version"`
	Records []storedFighter `json:"records"`
}
type storedFighter struct {
	PID    uint64 `json:"pid"`
	Record string `json:"record"`
}

const maxFighterStateBytes = 2 * 1024 * 1024

// LoadFighterState is called before registering handlers. Seeds set the first
// and third rating scalars (A/C) together in an existing report. Observed 13.0.5
// reports have equal A/C. A C-only seed changed the display but failed the live
// crossing trial; paired A/C seeding passed. The formula is not decoded. Keep B, time and
// flags intact; an unknown fighter is an error, not an invented record.
// Seeds apply at startup while configured; remove them for normal persistence.
func (r *Responder) LoadFighterState(path string, allowed map[uint64]bool, seeds map[uint64]map[byte]uint32) error {
	if !r.opt.FighterStats || path == "" || !filepath.IsAbs(path) {
		return errors.New("fighter state needs enabled stats and an absolute path")
	}
	players := map[uint64]map[byte][fighterRecordSize]byte{}
	f, err := os.Open(path)
	if err == nil {
		data, readErr := io.ReadAll(io.LimitReader(f, maxFighterStateBytes+1))
		closeErr := f.Close()
		if readErr == nil {
			readErr = closeErr
		}
		if readErr != nil || len(data) > maxFighterStateBytes {
			return errors.New("fighter state cannot be read within size limit")
		}
		var state fighterState
		dec := json.NewDecoder(bytes.NewReader(data))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&state); err != nil {
			return fmt.Errorf("fighter state: %w", err)
		}
		if dec.Decode(new(any)) != io.EOF || state.Version != 1 || len(state.Records) > maxFighterPlayers*100 {
			return errors.New("invalid fighter state version, length or trailing data")
		}
		for _, entry := range state.Records {
			b, err := hex.DecodeString(entry.Record)
			if err != nil || !validFighterRecord(b) || entry.PID == 0 || !allowed[entry.PID] {
				return errors.New("fighter state contains an invalid record or unconfigured PID")
			}
			if players[entry.PID] == nil {
				players[entry.PID] = map[byte][fighterRecordSize]byte{}
			}
			if _, exists := players[entry.PID][b[5]]; exists {
				return errors.New("fighter state contains a duplicate PID/fighter")
			}
			var record [fighterRecordSize]byte
			copy(record[:], b)
			players[entry.PID][b[5]] = record
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("open fighter state: %w", err)
	}
	for pid, fighters := range seeds {
		for key, rating := range fighters {
			record, exists := players[pid][key]
			if !exists || key >= fighterCount || !allowed[pid] {
				return fmt.Errorf("fighter seed needs an existing report for PID %d fighter %d", pid, key)
			}
			binary.LittleEndian.PutUint32(record[6:10], rating)
			binary.LittleEndian.PutUint32(record[14:18], rating)
			players[pid][key] = record
		}
	}
	// Persist before publishing. A failed startup never exposes partial seeds.
	if err := writeFighterState(path, players); err != nil {
		return err
	}
	r.fighters.mu.Lock()
	defer r.fighters.mu.Unlock()
	r.fighters.players, r.fighters.path = players, path
	return nil
}

func writeFighterState(path string, players map[uint64]map[byte][fighterRecordSize]byte) error {
	state := fighterState{Version: 1, Records: []storedFighter{}}
	pids := make([]uint64, 0, len(players))
	for pid := range players {
		pids = append(pids, pid)
	}
	sort.Slice(pids, func(i, j int) bool { return pids[i] < pids[j] })
	for _, pid := range pids {
		for key := 0; key <= 255; key++ {
			if record, ok := players[pid][byte(key)]; ok {
				state.Records = append(state.Records, storedFighter{pid, hex.EncodeToString(record[:])})
			}
		}
	}
	data, err := json.Marshal(state)
	if err != nil {
		return err
	}
	// A temporary sibling plus rename avoids leaving half a JSON file after a
	// process interruption. A successful report is acknowledged only after this.
	f, err := os.CreateTemp(filepath.Dir(path), ".fighter-state-*")
	if err != nil {
		return fmt.Errorf("create fighter state: %w", err)
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(data); err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err == nil {
		err = os.Rename(f.Name(), path)
	}
	if err != nil {
		return fmt.Errorf("commit fighter state: %w", err)
	}
	return nil
}
