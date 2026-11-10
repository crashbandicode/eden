package nex

import "testing"

func sessionPartRequest(s *Settings, p *UpdateMatchmakeSessionParam) *RMCMessage {
	body := NewStreamOut(s)
	body.Add(p)
	return NewRMCRequest(s, ProtocolMatchmakeExtension, MethodUpdateMatchmakeSessionPart, 9, body.Bytes())
}

// Smash's Quickplay rematch re-entry: A hosted a gathering, B joined, the match started
// (CloseParticipation), B declined the rematch and left, A answered Yes and reopened the
// gathering with UpdateMatchmakeSessionPart (OpenParticipation, bit 0x2). A new player C's
// auto-matchmake must then join A's gathering instead of opening its own.
func TestSessionPartReopensParticipationForRematchReentry(t *testing.T) {
	s := testSettings()
	ep := NewEndpoint(s)
	mm := NewMatchmaking()
	ext := mm.ExtensionHandler()
	if mm.SessionPartPersists {
		t.Fatal("the default must stay without Animal Crossing's SessionPartPersists")
	}

	connA := newTestConn(ep, 3100, "88.0.1.1:1")
	stA := NewStationURL("prudp")
	stA.Set("address", "88.0.1.1")
	stA.SetInt("RVCID", int(connA.ID))
	connA.SetStations([]*StationURL{stA})
	connC := newTestConn(ep, 3300, "88.0.1.3:1")

	autoReq := func() *RMCMessage {
		p := &AutoMatchmakeParam{Session: MatchmakeSession{
			Gathering: Gathering{MaxParticipants: 2}, GameMode: 3, OpenParticipation: true,
			Attribs: []uint32{0, 0, 0, 0, 0, 0},
		}}
		body := NewStreamOut(s)
		body.Add(p)
		return NewRMCRequest(s, ProtocolMatchmakeExtension, MethodAutoMatchmakeWithParamPostpone, 1, body.Bytes())
	}

	var sessA MatchmakeSession
	NewStreamIn(ext(connA, autoReq()).Body, s).Extract(&sessA)
	gid := sessA.ID
	if sessA.OwnerPID != 3100 || gid == 0 {
		t.Fatalf("A's gathering: %+v", sessA)
	}

	// The match started: the host closed participation (0x6D.0x01). After the match, B
	// declined and left, so only A remains, in a closed gathering.
	closeBody := NewStreamOut(s)
	closeBody.U32(gid)
	if resp := ext(connA, NewRMCRequest(s, ProtocolMatchmakeExtension, MethodCloseParticipation, 2, closeBody.Bytes())); resp.IsError {
		t.Fatalf("CloseParticipation failed: %+v", resp)
	}
	mm.mu.Lock()
	mm.gatherings[gid].participants = []uint64{3100}
	mm.gatherings[gid].session.NumParticipants = 1
	mm.mu.Unlock()

	// While it is closed, C's search opens a gathering of its own (what qcf-srm06 saw).
	var closedTry MatchmakeSession
	NewStreamIn(ext(connC, autoReq()).Body, s).Extract(&closedTry)
	if closedTry.ID == gid {
		t.Fatalf("C joined a closed gathering")
	}
	mm.mu.Lock()
	delete(mm.gatherings, closedTry.ID)
	mm.mu.Unlock()

	// Rematch "Yes": A reopens with 0x2C, OpenParticipation flagged.
	resp := ext(connA, sessionPartRequest(s, &UpdateMatchmakeSessionParam{
		GID: gid, ModificationFlags: modFlagOpenParticipation, OpenParticipation: true,
	}))
	if resp.IsError || len(resp.Body) != 0 {
		t.Fatalf("UpdateMatchmakeSessionPart reply: %+v", resp)
	}
	mm.mu.Lock()
	open := mm.gatherings[gid].session.OpenParticipation
	mm.mu.Unlock()
	if !open {
		t.Fatal("0x2C with OpenParticipation flagged did not reopen the gathering")
	}

	// C's next search joins A's gathering.
	var sessC MatchmakeSession
	NewStreamIn(ext(connC, autoReq()).Body, s).Extract(&sessC)
	if sessC.ID != gid || sessC.OwnerPID != 3100 || sessC.NumParticipants != 2 {
		t.Fatalf("C's session: %+v (want gid=%d owner=3100 participants=2)", sessC, gid)
	}
}

// Only the flagged fields change, only for a member, and an unflagged OpenParticipation
// never closes (or opens) the gathering.
func TestSessionPartAppliesOnlyFlaggedFieldsForMembers(t *testing.T) {
	m := NewMatchmaking()
	sess := &MatchmakeSession{GameMode: 3, Attribs: []uint32{1, 2}, ApplicationData: []byte{7}, OpenParticipation: false}
	sess.ID, sess.OwnerPID, sess.HostPID, sess.MinParticipants, sess.MaxParticipants = 8, 501, 501, 2, 2
	m.gatherings[8] = &gathering{session: sess, participants: []uint64{501, 502}}

	// Nothing flagged: nothing changes, even with values present.
	if _, ok := m.applySessionPartFlags(501, &UpdateMatchmakeSessionParam{GID: 8, OpenParticipation: true,
		Attributes: []uint32{9}, GameMode: 4, MaxParticipants: 8}); !ok {
		t.Fatal("member update rejected")
	}
	if sess.OpenParticipation || sess.GameMode != 3 || sess.MaxParticipants != 2 || len(sess.Attribs) != 2 {
		t.Fatalf("unflagged fields changed: %+v", sess)
	}

	// A non-member can't reopen it; an unknown gathering is reported.
	if _, ok := m.applySessionPartFlags(999, &UpdateMatchmakeSessionParam{GID: 8,
		ModificationFlags: modFlagOpenParticipation, OpenParticipation: true}); ok || sess.OpenParticipation {
		t.Fatal("a non-member changed the gathering")
	}
	if _, ok := m.applySessionPartFlags(501, &UpdateMatchmakeSessionParam{GID: 77}); ok {
		t.Fatal("an unknown gathering was reported as updated")
	}

	// A participant (not the owner: the joiner that answered Yes after the host left) may
	// reopen it; attributes and the maximum follow their bits; the codeword path is ACNH's.
	applied, ok := m.applySessionPartFlags(502, &UpdateMatchmakeSessionParam{GID: 8,
		ModificationFlags: modFlagOpenParticipation | modFlagAttributes | modFlagMaxParticipants | modFlagCodeword,
		OpenParticipation: true, Attributes: []uint32{5, 6, 7}, MaxParticipants: 4, Codeword: "NOPE1"})
	if !ok || !sess.OpenParticipation || len(sess.Attribs) != 3 || sess.MaxParticipants != 4 || len(applied) != 3 {
		t.Fatalf("flagged update: ok=%v applied=%v session=%+v", ok, applied, sess)
	}
	if sess.Codeword != "" || len(m.byCode) != 0 {
		t.Fatalf("the default path wrote a codeword: %q %v", sess.Codeword, m.byCode)
	}
}
