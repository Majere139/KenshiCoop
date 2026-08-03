// ReplicatorAuthority.cpp - host-authority enforcement on the join (monolith
// split from Replicator.cpp, 2026-07-12): applyNpcCensus (protocol 36 census
// intake), enforceHostAuthority (suppress/park/cull of local copies the host
// does not corroborate, far-mint requests), parkDivergedCopy, and the debug
// marker HUD plumbing (debugMark/pruneDebugMarkers).
//
// Shared hubs: owns censusHands_ + suppressed_; reads proxyByKey_ (minted by
// the spawn TU); writes life_ via lifeSet.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

void Replicator::debugMark(Character* c, int colorId, const char* tag) {
    static int en = -1;
    if (en < 0) {
        const char* e = getenv("KENSHICOOP_DEBUG_MARKERS");
        en = (e && e[0] == '1') ? 1 : 0;
    }
    if (en != 1 || !c) return;
    std::map<Character*, DebugMarker>::iterator it = debugMarkers_.find(c);
    if (it != debugMarkers_.end() && it->second.color == colorId) return;
    char nm[40];
    engine::charName(c, nm, sizeof(nm));
    char cap[64];
    _snprintf(cap, sizeof(cap) - 1, "%s %s", tag, nm);
    cap[sizeof(cap) - 1] = '\0';
    if (it == debugMarkers_.end()) {
        void* l = engine::markerCreate(c, cap, colorId);
        if (l) {
            DebugMarker m; m.label = l; m.color = colorId;
            debugMarkers_[c] = m;
        }
    } else if (engine::markerUpdate(it->second.label, cap, colorId)) {
        it->second.color = colorId;
    }
}

void Replicator::applyNpcCensus(Inbound& in) {
    std::deque<InboundNpcCensus> got;
    in.drainNpcCensus(got);
    if (got.empty()) return;
    // Latest wins (reliable-ordered channel, 1 Hz - normally one pending).
    const InboundNpcCensus& nc = got.back();
    censusHands_.clear();
    censusPos_.clear();
    unsigned int n = (unsigned int)(nc.hands.size() / 5);
    bool havePos = nc.pos.size() >= (size_t)n * 3;
    for (unsigned int i = 0; i < n; ++i) {
        Key k;
        k.t  = nc.hands[i * 5 + 0];
        k.c  = nc.hands[i * 5 + 1];
        k.cs = nc.hands[i * 5 + 2];
        k.i  = nc.hands[i * 5 + 3];
        k.s  = nc.hands[i * 5 + 4];
        censusHands_.insert(k);
        if (havePos) {
            CensusPos cp;
            cp.x = nc.pos[i * 3 + 0];
            cp.y = nc.pos[i * 3 + 1];
            cp.z = nc.pos[i * 3 + 2];
            censusPos_[k] = cp;
        }
    }
    censusRecvMs_ = nowMs();
    static unsigned long logTick = 0;
    if ((censusRecvMs_ - logTick) > 10000) {
        logTick = censusRecvMs_;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[census] recv n=%u culls=%lu",
                  n, censusCulls_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::enforceHostAuthority(GameWorld* gw) {
    if (!gw) return;
    // Hysteresis (step 5, spike 18): a hard streamed/unstreamed edge churned
    // boundary NPCs. Suppress only after a sustained unstreamed run (~1 s), and
    // restore only after a sustained streamed dwell (~2 s), counted in frames.
    const unsigned int SUPPRESS_AFTER_FRAMES = 75;
    const unsigned int RESTORE_AFTER_FRAMES  = 150;

    // Hands the host streamed a fresh sample for this tick = the authoritative set.
    std::set<Key> keep;
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        if (it->second.fresh) keep.insert(it->first);
    }

    // Proxy bodies are EXEMPT from authority judgment (2026-07-11 census-mint
    // fix): a proxy's LOCAL hand never matches its streamed key, so the wide
    // pass saw every far-minted proxy as a census-absent ghost and froze it
    // ~1 s after binding (spawn_far run 124346: all four proxies culled at
    // their mint position, then the drive's teleports no-opped on the frozen
    // bodies forever). Their existence authority is the census entry for the
    // STREAM key that minted them; drive/starve policy is applyTargets'.
    std::set<Character*> proxyChars;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ++it) proxyChars.insert(it->second);
    // Un-hide anything suppressed before it became a proxy / driven body (the
    // mint can land on a body a previous pass already judged).
    for (std::map<Key, Character*>::iterator it = suppressed_.begin();
         it != suppressed_.end(); ) {
        if (proxyChars.find(it->second) != proxyChars.end() ||
            drivenChars_.find(it->second) != drivenChars_.end()) {
            engine::restoreNpc(gw, it->second);
            ++authRestores_;
            { char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] restore NPC hand=%u,%u (proxy/driven exemption; supp=%u)",
                (unsigned)it->first.i, (unsigned)it->first.s,
                (unsigned)suppressed_.size() - 1);
              b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            suppressed_.erase(it++);
        } else ++it;
    }

    // Enumerate the join's local world NPCs (same interest query as the host).
    const unsigned int MAX_NPCS = 256;
    static Character*  chars[MAX_NPCS]; // main-thread only
    static EntityState states[MAX_NPCS];
    bool nearTrunc = false;
    unsigned int n = engine::listNpcs(gw, chars, states, MAX_NPCS, &nearTrunc);

    // Protocol 36 wide-radius existence pass: enumerate out to the census
    // radius so local-only ghosts get culled at render range instead of at the
    // ~200 u stream bubble (the 2026-07-09 field report). Only while the
    // host's census is FRESH - a silent census (host lagging, channel down)
    // DISABLES wide culling rather than mass-suppressing the loaded area.
    static Character*  wChars[NPC_CENSUS_MAX]; // main-thread only
    static EntityState wStates[NPC_CENSUS_MAX];
    unsigned int wn = 0;
    bool wideTrunc = false;
    bool censusFresh = censusRadius_ > 0.0f && censusRecvMs_ != 0 &&
                       (nowMs() - censusRecvMs_) <= 5000;
    if (censusFresh)
        wn = engine::listNpcsWide(gw, censusRadius_, wChars, wStates, NPC_CENSUS_MAX,
                                  &wideTrunc);

    // Phase 0.5: account the time wide culling spends DISABLED. Staleness is a
    // deliberate fail-open (a silent host must not mass-suppress a loaded area),
    // but it is also unbounded ghost accumulation: nothing is judged while it
    // lasts, and a body that drifts outside censusRadius_ before the census
    // returns is never judged again. Host-side zone streaming stalls the main
    // thread, and travel is a continuous zone stream - so the window opens
    // exactly when it does the most damage.
    {
        unsigned long nowF = nowMs();
        if (censusFreshChkMs_ != 0 && !censusFresh)
            censusStaleMs_ += (nowF - censusFreshChkMs_);
        if (censusFreshChkMs_ != 0 && censusFreshPrev_ && !censusFresh) {
            ++censusStaleEdges_;
            char b[144]; _snprintf(b, sizeof(b) - 1,
                "[census] STALE (wide culling disabled; last recv %lums ago, edges=%lu)",
                censusRecvMs_ ? (nowF - censusRecvMs_) : 0ul, censusStaleEdges_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else if (censusFreshChkMs_ != 0 && !censusFreshPrev_ && censusFresh) {
            char b[144]; _snprintf(b, sizeof(b) - 1,
                "[census] fresh again (wide culling re-enabled; staleMs=%lu edges=%lu)",
                censusStaleMs_, censusStaleEdges_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        censusFreshPrev_  = censusFresh;
        censusFreshChkMs_ = nowF;
    }

    if (auditRows_) {
        notePlatoons(gw, states, n, "join");
        notePlatoons(gw, wStates, wn, "join");
    }

    // Attention gate anchors, resolved once for both passes and the audit. A
    // body that no anchor is within attentionRadius_ of is DORMANT: nobody is
    // standing near it and nobody is looking at it, so the host deliberately
    // leaves it out of the census - and census silence over a region the host
    // is not speaking for must not be read as "this body does not exist".
    // That misreading is the whole ghost mechanism: a legitimate local NPC in
    // a place only this client cares about, hidden because the other client
    // never mentioned it.
    float rawAnch[12];
    unsigned int nRawAnch = engine::interestAnchors(gw, rawAnch);
    float attnAnch[12];
    unsigned int nAttnAnch = attentionAnchors(gw, rawAnch, nRawAnch, attnAnch);

    // Prune counters for hands the enumeration no longer sees (left interest),
    // preserving suppressed entries (a hidden body may drop out of the query but
    // must keep its counters so the restore dwell works when it returns).
    std::set<Key> seen;
    for (unsigned int i = 0; i < n; ++i) seen.insert(keyOf(states[i]));
    for (unsigned int i = 0; i < wn; ++i) seen.insert(keyOf(wStates[i]));
    for (std::map<Key, AuthCount>::iterator it = authCount_.begin(); it != authCount_.end(); ) {
        if (seen.find(it->first) == seen.end() &&
            suppressed_.find(it->first) == suppressed_.end()) authCount_.erase(it++);
        else ++it;
    }
    pruneAttention(seen);

    for (unsigned int i = 0; i < n; ++i) {
        // Proxy bodies answer to their streamed key's census entry, not their
        // local hand (which exists on no other client) - never judge them.
        if (proxyChars.find(chars[i]) != proxyChars.end()) continue;
        Key k = keyOf(states[i]);
        // Streamed = the hand is in this tick's fresh set, OR the body pointer is
        // one applyTargets drove this tick. The pointer check covers combat-
        // detached NPCs: detachFromTownAI re-containers the body, so its LOCAL
        // key differs from the streamed key and the hand lookup alone would
        // suppress an actively-driven combatant (crowd copies froze mid-brawl).
        bool streamed = (keep.find(k) != keep.end()) ||
                        (drivenChars_.find(chars[i]) != drivenChars_.end());
        // Pop-out fix (2026-07-11 field report): existence authority is the
        // CENSUS, drive authority is the STREAM. An NPC at the ~200 u stream-
        // bubble boundary flickers in/out of the host's fresh set (the two
        // clients disagree slightly on its position), and the old streamed-only
        // signal hid REAL host-present NPCs ('Saint'/'Kumo' measured churning
        // suppress->restore->suppress every few seconds). While the census is
        // fresh, a census-present NPC is never suppressed - its local AI copy
        // may drift, but it EXISTS; only census-absent ghosts get hidden. With
        // no fresh census (hatch off / host lagging) the legacy streamed-only
        // behavior stands.
        bool exists = streamed ||
                      (censusFresh && censusHands_.find(k) != censusHands_.end());
        std::map<Key, Character*>::iterator s = suppressed_.find(k);
        AuthCount& ac = authCount_[k];
        // Dormant and census-absent: neither client is speaking for this
        // region, so there is nothing to judge. Hold the debounce at zero
        // rather than letting it climb silently - when attention does arrive,
        // the body gets a full SUPPRESS_AFTER_FRAMES to be corroborated
        // instead of being hidden on the first frame someone looks at it.
        if (!exists && !observedAt(k, attnAnch, nAttnAnch,
                                   states[i].x, states[i].y, states[i].z)) {
            ac.unstreamed = 0;
            continue;
        }
        if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
        else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
        if (exists) {
            // Host owns it again: hand it back once the stream has DWELLED (a
            // boundary NPC that flickers into the set for a frame stays hidden).
            if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                engine::restoreNpc(gw, chars[i]);
                suppressed_.erase(s);
                s = suppressed_.end();
                ++authRestores_;
                lifeSet(k, LIFE_RESOLVED, "restore");
                { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                  char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[authority] restore NPC hand=%u,%u name='%s' (supp=%u churn=%lu/%lu)",
                    states[i].hIndex, states[i].hSerial, nm,
                    (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                  b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            if (s == suppressed_.end() && !streamed) {
                // Driven bodies report their tier (and own their marker) from
                // applyTargets; a census-present LOCAL copy is the park-
                // fallback regime.
                lifeSet(k, LIFE_PARKED, "census-local");
                debugMark(chars[i], 2, lifeName(LIFE_PARKED));
                // world_parity 2026-07-17: the bubble used to be a park-free
                // zone outright (run 185524: sub-50 u seat divergence fought
                // the local seat AI every frame). But a census-present body
                // inside the JOIN's bubble that the HOST does not stream
                // (interest sets only partially overlap - the edge class the
                // manual sessions kept catching, Pao at a steady 451 u) had
                // NO reconciliation at all. parkDivergedCopy's 120 u
                // threshold already exempts the seat-schedule class, and the
                // divergence freeze quiets the AI that used to fight the
                // teleport - so the park now runs here with the exact same
                // gates as the wide pass.
                float drift = parkDivergedCopy(chars[i], states[i], k);
                if (censusFreezeAi_ && drift >= 0.0f)
                    censusFreezeDivergedAi(chars[i], k, drift);
            }
            // NOTE: census position parking below the 120 u threshold stays
            // OFF inside the stream bubble (npc_sync regression, run 185524):
            // a bar NPC whose two schedules seat it ~50 u apart is re-placed
            // by its own seat AI the same frame, so the park never sticks and the fight
            // wrecked tracking/march. Inside the bubble the stream owns
            // position truth; parking is a WIDE-pass render-range tool.
        } else {
            // Host neither streams nor lists it (census-absent ghost): after
            // the debounce, hide + freeze so the local AI can't run a divergent
            // copy on top of the host-driven world.
            if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                // Phase 2 hardening: only RECORD the suppression when the engine
                // call actually landed. A faulted hide used to be booked as done,
                // leaving the body visible forever with no evidence - the silent
                // version of the join-only-enemies field report. On failure the
                // unstreamed streak keeps climbing, so this retries every frame;
                // log the miss once at the threshold crossing.
                if (engine::suppressNpc(gw, chars[i])) {
                    suppressed_[k] = chars[i];
                    ++authSuppresses_;
                    lifeSet(k, LIFE_CULLED, "suppress");
                    debugMark(chars[i], 1, lifeName(LIFE_CULLED));
                    { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress NPC hand=%u,%u name='%s' (streamed=%u local=%u supp=%u churn=%lu/%lu)",
                        states[i].hIndex, states[i].hSerial, nm, (unsigned)keep.size(), n,
                        (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress MISS hand=%u,%u (engine call failed; retrying)",
                        states[i].hIndex, states[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Wide pass (protocol 36): an NPC beyond the stream bubble is never in
    // this tick's fresh set, so its authority signal is EXISTENCE (its hand in
    // the host's census) rather than streaming. NPCs the near pass already
    // judged are skipped by pointer (its streamed logic is authoritative
    // inside the bubble), as is anything applyTargets drove this tick. Same
    // hysteresis counters so a census-boundary NPC doesn't churn.
    if (censusFresh && wn > 0) {
        unsigned long nowR = nowMs(); // recently-driven grace reference
        std::set<Character*> nearSet;
        for (unsigned int i = 0; i < n; ++i) nearSet.insert(chars[i]);
        for (unsigned int i = 0; i < wn; ++i) {
            if (nearSet.find(wChars[i]) != nearSet.end()) continue;
            if (proxyChars.find(wChars[i]) != proxyChars.end()) continue;
            // Phase 2 mid-band tier: a DRIVEN body used to skip this pass
            // entirely - correct for parking/suppression (the stream owns its
            // position, and hiding a driven body is self-defeating), but it
            // also skipped RESTORE: a suppressed NPC whose hand starts
            // arriving on the mid tier is driven every tick (drivenChars_)
            // yet stays hidden+frozen forever - walk orders no-op on a body
            // removed from the update list, the permanent-zombie shape of
            // the old boundary flicker. Let a driven body reach the restore
            // branch (same dwell), and skip only park/suppress for it.
            // "Driven" includes a grace window (drivenSeen_): a mid-tier
            // body's samples ride the round-robin, and a rotation hiccup
            // must not let the cull streak run while the body is between
            // samples (run 103044: 6 cull/restore cycles per hand).
            bool driven = drivenChars_.find(wChars[i]) != drivenChars_.end();
            if (!driven) {
                std::map<Character*, unsigned long>::iterator ds =
                    drivenSeen_.find(wChars[i]);
                driven = ds != drivenSeen_.end() && (nowR - ds->second) < 8000;
            }
            Key k = keyOf(wStates[i]);
            if (driven && suppressed_.find(k) == suppressed_.end()) continue;
            bool exists = censusHands_.find(k) != censusHands_.end() ||
                          keep.find(k) != keep.end() || driven;
            std::map<Key, Character*>::iterator s = suppressed_.find(k);
            AuthCount& ac = authCount_[k];
            // Dormancy, same as the near pass - and this is where it matters
            // most. The wide pass reaches out to censusRadius_ (2000 u), far
            // past anywhere either player is looking, so most of what it used
            // to cull sat in regions nobody was watching at all.
            if (!exists && !observedAt(k, attnAnch, nAttnAnch,
                                       wStates[i].x, wStates[i].y, wStates[i].z)) {
                ac.unstreamed = 0;
                continue;
            }
            if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
            else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
            if (exists) {
                if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                    engine::restoreNpc(gw, wChars[i]);
                    suppressed_.erase(s);
                    s = suppressed_.end();
                    ++authRestores_;
                    lifeSet(k, LIFE_RESOLVED, "restore-wide");
                    debugMark(wChars[i], 2, lifeName(LIFE_PARKED));
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[census] restore NPC hand=%u,%u name='%s' (supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                }
                // v38 parking, wide-radius flavor (this is where the pack-
                // hidden class lives: census-present wilderness NPCs far
                // outside the stream bubble, each side simulating its own).
                // Driven bodies excluded: the mid/near stream owns them.
                if (s == suppressed_.end() && !driven) {
                    lifeSet(k, LIFE_PARKED, "census-wide");
                    float drift = parkDivergedCopy(wChars[i], wStates[i], k);
                    // Census-band AI freeze: quiesce a diverging body's local AI
                    // so it can't flee/aggro the join's guards (the mining-slave
                    // cascade). drift < 0 = no census row / parking off -> skip.
                    if (censusFreezeAi_ && drift >= 0.0f)
                        censusFreezeDivergedAi(wChars[i], k, drift);
                }
            } else if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                if (engine::suppressNpc(gw, wChars[i])) {
                    suppressed_[k] = wChars[i];
                    ++authSuppresses_;
                    ++censusCulls_;
                    lifeSet(k, LIFE_CULLED, "cull-wide");
                    debugMark(wChars[i], 1, lifeName(LIFE_CULLED));
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull NPC hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                        "(census=%u wide=%u supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        wStates[i].x, wStates[i].y, wStates[i].z,
                        (unsigned)censusHands_.size(), wn,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull MISS hand=%u,%u (engine call failed; retrying)",
                        wStates[i].hIndex, wStates[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Phase 2 hardening: RE-ASSERT the hide on a ~2 s cadence. suppressNpc is a
    // one-shot (remove-from-update + clearGoals + setVisible(false)), but the
    // engine can undo it on its own: an ambush squad's dialog/combat package
    // re-tasks the body and zone streaming can re-add it to the update list -
    // exactly the field report ("join-only enemies started dialog then attacked
    // and stayed visible"). The call is idempotent, so re-applying to a body the
    // engine never touched is a no-op; keys the host started streaming again are
    // skipped (the restore dwell owns those).
    //
    // Lifetime guard (2026-07-11 join crash): the engine owns every suppressed
    // body and can despawn it at any time - the 17:53 session held 93 hidden
    // wildlife bodies through a zone stream, and the dump shows the engine's
    // own sensory update walking a freed body. Before ANY touch, prove each
    // entry live with a hand round-trip: SEH-read the pointer's CURRENT hand
    // and resolve it back - the same pointer proves the body is alive (this
    // survives engine re-containering, which only changes the hand); anything
    // else means despawned, and the entry + marker + counters are dropped
    // without touching the pointer. A live body whose hand CHANGED (combat
    // detach re-containered it while hidden) migrates its entry to the new key
    // so the hide keeps re-asserting - the old key would never resolve again.
    unsigned long now = nowMs();
    if ((now - authReassertMs_) >= 2000) {
        authReassertMs_ = now;
        unsigned int pruned = 0;
        std::map<Key, Character*> migrated;
        for (std::map<Key, Character*>::iterator it = suppressed_.begin();
             it != suppressed_.end(); ) {
            unsigned int h[5];
            Character* live = 0;
            if (engine::readHand(it->second, h))
                live = engine::resolveCharByHand(h[0], h[1], h[2], h[3], h[4]);
            if (live != it->second) {
                std::map<Character*, DebugMarker>::iterator mi =
                    debugMarkers_.find(it->second);
                if (mi != debugMarkers_.end()) {
                    engine::markerDestroy(mi->second.label);
                    debugMarkers_.erase(mi);
                }
                authCount_.erase(it->first);
                ++pruned; ++authPruned_;
                suppressed_.erase(it++);
                continue;
            }
            Key ck; ck.i = h[0]; ck.s = h[1]; ck.t = h[2];
            ck.c = h[3]; ck.cs = h[4];
            if (ck < it->first || it->first < ck) {
                migrated[ck] = it->second;
                authCount_.erase(it->first);
                suppressed_.erase(it++);
                continue;
            }
            if (keep.find(it->first) != keep.end()) { ++it; continue; }
            // A combat-detached body is driven under a DIFFERENT streamed key
            // (pointer identity survives the re-containering); never re-hide a
            // body applyTargets drove this tick.
            if (drivenChars_.find(it->second) != drivenChars_.end()) { ++it; continue; }
            engine::suppressNpc(gw, it->second);
            ++it;
        }
        for (std::map<Key, Character*>::iterator it = migrated.begin();
             it != migrated.end(); ++it) {
            if (suppressed_.find(it->first) != suppressed_.end()) continue;
            suppressed_[it->first] = it->second;
            if (keep.find(it->first) == keep.end() &&
                drivenChars_.find(it->second) == drivenChars_.end())
                engine::suppressNpc(gw, it->second);
        }
        if (pruned > 0) {
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] pruned %u stale suppressed entries (despawned; supp=%u total=%lu)",
                pruned, (unsigned)suppressed_.size(), authPruned_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Marker map hygiene on the same cadence: drop labels whose Character*
        // wasn't vouched live this pass (this tick's enumerations, driven and
        // proxy sets, plus the just-validated suppressed bodies). A pruned
        // body that comes back into judgment simply gets a fresh label.
        if (!debugMarkers_.empty()) {
            std::set<Character*> vouched;
            for (unsigned int i = 0; i < n; ++i)  vouched.insert(chars[i]);
            for (unsigned int i = 0; i < wn; ++i) vouched.insert(wChars[i]);
            vouched.insert(drivenChars_.begin(), drivenChars_.end());
            vouched.insert(proxyChars.begin(), proxyChars.end());
            for (std::map<Key, Character*>::iterator it = suppressed_.begin();
                 it != suppressed_.end(); ++it) vouched.insert(it->second);
            pruneDebugMarkers(vouched);
        }
    }

    // Existence-audit probe (pack-hidden investigation, 2026-07-11): a 5 s
    // census of what THIS client's world holds vs what the host vouches for,
    // classifying every enumerated NPC into exactly one bucket:
    //   drv   - streamed/driven this tick (host actively drives it)
    //   cen   - census-present, unstreamed (legit local-sim copy, host has it)
    //   hid   - booked suppressed (we hid it)
    //   dorm  - census-absent and unobserved (attention gate): no anchor is
    //           within attentionRadius_, so the host is not speaking for this
    //           region and we are not judging it. Deliberately NOT a ghost -
    //           these are the bodies the gate exists to leave alone.
    //   ghost - census-absent, observed, NOT suppressed (the visible-on-join-
    //           only class: either inside the suppress debounce, judged only
    //           while the census was stale, or escaping judgment entirely)
    // ghost is the bucket the field reports live in - it should only ever be
    // transient (one debounce, ~1 s). Test-ExistenceParity gates on it.
    // KENSHICOOP_DEBUG_CENSUS=1 additionally dumps a row per ghost.
    static unsigned long auditMs = 0; // main-thread only
    if ((now - auditMs) >= 5000) {
        auditMs = now;
        unsigned int cDrv = 0, cCen = 0, cHid = 0, cGhost = 0, cDorm = 0;
        // Safety check on the attention gate. The design rests on a squad
        // never going dormant just because no camera is on it: every squad-tab
        // LEADER is an interest anchor, so bodies beside a leader are observed
        // by construction. What is NOT covered by construction is a squad
        // member that walked away from its leader - nothing anchors it.
        // dormPc counts dormant bodies within attentionRadius_ of ANY player
        // character, i.e. bodies standing next to somebody's squad that we
        // stopped judging anyway. It must read 0.
        const unsigned int MAX_PC = 32;
        static EntityState pcStates[MAX_PC];  // main-thread only
        unsigned int nPc = 0;
        if (attentionRadius_ > 0.0f)
            nPc = engine::captureSquad(gw, /*leaderOnly*/ false, pcStates, MAX_PC);
        unsigned int cDormPc = 0;
        static int dumpGhost = -1;
        if (dumpGhost < 0) {
            const char* e = getenv("KENSHICOOP_DEBUG_CENSUS");
            dumpGhost = (e && e[0] == '1') ? 1 : 0;
        }
        std::set<Character*> counted;
        std::set<Key> emittedKeys; // world_parity: hands already dumped
        unsigned int ghostRows = 0;
        // Phase 0.5 horizon probe: how far out do ghosts actually sit? Anything
        // past censusRadius_ is never enumerated at all, so it cannot appear in
        // ANY bucket here - the ghost count silently stops at the horizon while
        // the player keeps seeing bodies past it. If ghostMax rides up against
        // the radius (and ghostEdge is non-zero), the cull horizon is the
        // binding constraint and KENSHICOOP_CENSUS_RADIUS is the lever; if
        // ghosts cluster close in, the cause is debounce/staleness/caps instead.
        // Distances are measured against the RAW anchors - how far out ghosts
        // sit, regardless of who can speak for the region - while the
        // classification below re-uses the vetoed set the passes judged with.
        const float* anch = rawAnch;
        unsigned int nAnch = nRawAnch;
        float ghostMaxD = -1.0f;
        unsigned int ghostEdge = 0;
        const float EDGE_BAND = censusRadius_ * 0.8f;
        for (int pass = 0; pass < 2; ++pass) {
            unsigned int cnt = (pass == 0) ? n : wn;
            Character**  cs  = (pass == 0) ? chars : wChars;
            EntityState* sts = (pass == 0) ? states : wStates;
            for (unsigned int i = 0; i < cnt; ++i) {
                if (!counted.insert(cs[i]).second) continue;
                Key k = keyOf(sts[i]);
                const char* cls;
                if (proxyChars.find(cs[i]) != proxyChars.end() ||
                    keep.find(k) != keep.end() ||
                    drivenChars_.find(cs[i]) != drivenChars_.end()) {
                    cls = "drv"; ++cDrv;
                } else if (suppressed_.find(k) != suppressed_.end()) {
                    cls = "hid"; ++cHid;
                } else if (censusFresh &&
                           censusHands_.find(k) != censusHands_.end()) {
                    cls = "cen"; ++cCen;
                } else if (!observedAt(k, attnAnch, nAttnAnch,
                                       sts[i].x, sts[i].y, sts[i].z)) {
                    cls = "dorm"; ++cDorm;
                    for (unsigned int p = 0; p < nPc; ++p) {
                        if (dist3(sts[i].x, sts[i].y, sts[i].z, pcStates[p].x,
                                  pcStates[p].y, pcStates[p].z)
                                <= attentionRadius_) {
                            ++cDormPc; break;
                        }
                    }
                } else {
                    cls = "ghost"; ++cGhost;
                    float nearest = -1.0f;
                    for (unsigned int a = 0; a < nAnch; ++a) {
                        float d = dist3(sts[i].x, sts[i].y, sts[i].z,
                                        anch[a * 3 + 0], anch[a * 3 + 1],
                                        anch[a * 3 + 2]);
                        if (nearest < 0.0f || d < nearest) nearest = d;
                    }
                    if (nearest > ghostMaxD) ghostMaxD = nearest;
                    if (EDGE_BAND > 0.0f && nearest >= EDGE_BAND) ++ghostEdge;
                    if (dumpGhost == 1 && ghostRows < 10) {
                        ++ghostRows;
                        char nm[48]; engine::charName(cs[i], nm, sizeof(nm));
                        char r[192]; _snprintf(r, sizeof(r) - 1,
                            "[audit] ghost hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                            "unstreak=%u",
                            sts[i].hIndex, sts[i].hSerial, nm,
                            sts[i].x, sts[i].y, sts[i].z, authCount_[k].unstreamed);
                        r[sizeof(r) - 1] = '\0'; coop::logLine(r);
                    }
                }
                // travel_parity worldstate rows (join side): one row per
                // enumerated NPC with its authority class, same schema as the
                // host's census dump so the oracle can cross-match by hand.
                if (auditRows_) { emittedKeys.insert(k); emitWnpcRow(cs[i], sts[i], cls); }
            }
        }
        if (auditRows_) {
            // world_parity third pass: a driven body that is locally IN
            // FURNITURE (bed/cage/chained at the join's fixture) drops out of
            // the spatial character query, so the two passes above never list
            // it and the parity oracle scored it "missing" (guard escorting
            // the arrested PC: 16/25 samples absent while rendering fine).
            // The drive targets are keyed by hand; emit any that resolve but
            // were not enumerated. captureNpcByHand's resolve round-trip is
            // the liveness proof; player-squad members are covered by
            // emitPcRows below.
            for (std::map<Key, Driven>::iterator ti = targets_.begin();
                 ti != targets_.end(); ++ti) {
                const Key& tk = ti->first;
                if (emittedKeys.find(tk) != emittedKeys.end()) continue;
                EntityState ts;
                if (!engine::captureNpcByHand(gw, tk.i, tk.s, tk.t, tk.c,
                                              tk.cs, &ts)) continue;
                Character* tc = engine::resolveCharByHand(tk.i, tk.s, tk.t,
                                                          tk.c, tk.cs);
                if (!tc) continue;
                // Row keyed by the STREAMED hand (what the host dumps): a
                // combat-detached body's local handle differs, and captureOne
                // read that local one - overwrite so the oracle can pair it.
                ts.hIndex = tk.i; ts.hSerial = tk.s; ts.hType = tk.t;
                ts.hContainer = tk.c; ts.hContainerSerial = tk.cs;
                emittedKeys.insert(tk);
                if (counted.insert(tc).second) ++cDrv;
                emitWnpcRow(tc, ts, "drv");
            }
            // ...and the same for every census-vouched hand (a host-side
            // barracks of SLEEPING guards is contained in beds on the join
            // too - resolving fine, rendering fine, invisible to the spatial
            // query, and not driven so the targets_ pass can't list them
            // either; a re-containered ex-slave answers to a DIFFERENT local
            // hand, so its enumerated row can't pair with the host's - emit
            // it under the census hand too). The resolve round-trip IS the
            // existence answer the parity oracle wants; a hand that fails it
            // here is genuinely absent from this client.
            for (std::set<Key>::iterator ci = censusHands_.begin();
                 ci != censusHands_.end(); ++ci) {
                if (emittedKeys.find(*ci) != emittedKeys.end()) continue;
                EntityState ts;
                if (!engine::captureNpcByHand(gw, ci->i, ci->s, ci->t, ci->c,
                                              ci->cs, &ts)) continue;
                Character* tc = engine::resolveCharByHand(ci->i, ci->s, ci->t,
                                                          ci->c, ci->cs);
                if (!tc) continue;
                ts.hIndex = ci->i; ts.hSerial = ci->s; ts.hType = ci->t;
                ts.hContainer = ci->c; ts.hContainerSerial = ci->cs;
                emittedKeys.insert(*ci);
                bool sup = suppressed_.find(*ci) != suppressed_.end();
                if (counted.insert(tc).second) { if (sup) ++cHid; else ++cCen; }
                emitWnpcRow(tc, ts, sup ? "hid" : "cen");
            }
            // world_parity: PC rows on the join too (the peer-driven copies) -
            // the host/join cls=pc pairs are what the PC position gate judges.
            emitPcRows(gw);
            // Phase 0.5 fields are APPENDED: Get-WorldRows' regex stops at
            // ghost= and tolerates any trailing text (fresh= already rides
            // past it), so the oracle contract is unchanged.
            char w[224]; _snprintf(w, sizeof(w) - 1,
                "SCENARIO WORLD n=%u cls=join drv=%u cen=%u hid=%u ghost=%u fresh=%d "
                "cap=%d ghostMax=%.0f ghostEdge=%u dorm=%u",
                (unsigned)counted.size(), cDrv, cCen, cHid, cGhost,
                censusFresh ? 1 : 0, (nearTrunc || wideTrunc) ? 1 : 0,
                ghostMaxD, ghostEdge, cDorm);
            w[sizeof(w) - 1] = '\0'; coop::logLine(w);
        }
        // Phase 0.5 fields appended after parks= (existing prefix untouched):
        //   nearCap/wideCap - the enumeration hit its buffer and judged an
        //                     INCOMPLETE world; absence stopped meaning absent
        //   staleMs/edges   - how long wide culling has been disabled, and how
        //                     often it dropped out (each edge leaks ghosts)
        //   ghostMax/Edge   - how far ghosts reach, and how many sit in the
        //                     outer 20% band next to the un-enumerable horizon
        //   dorm/attnR      - bodies the attention gate left alone, and the
        //                     radius it used (0 = gate off, so dorm is always
        //                     0 and every bucket reads as it did before)
        //   dormPc/pcs      - gate safety violation count: dormant bodies
        //                     within attnR of a player character (must be 0),
        //                     over the pcs squad members it was measured
        //                     against - pcs=0 makes the zero vacuous
        char b[352]; _snprintf(b, sizeof(b) - 1,
            "[audit] exist near=%u wide=%u drv=%u cen=%u hid=%u ghost=%u "
            "supp=%u census=%u fresh=%d parks=%lu "
            "nearCap=%d wideCap=%d staleMs=%lu edges=%lu ghostMax=%.0f ghostEdge=%u "
            "dorm=%u attnR=%.0f dormPc=%u pcs=%u",
            n, wn, cDrv, cCen, cHid, cGhost,
            (unsigned)suppressed_.size(), (unsigned)censusHands_.size(),
            censusFresh ? 1 : 0, censusParks_,
            nearTrunc ? 1 : 0, wideTrunc ? 1 : 0,
            censusStaleMs_, censusStaleEdges_, ghostMaxD, ghostEdge,
            cDorm, attentionRadius_, cDormPc, nPc);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Phase 3 lifecycle upkeep: prune left-interest records + self-audit
    // mintable hands stuck in DISCOVERED (the invisible-raid failure).
    lifeSweep(gw, now);
    measureAttach(now);
}

// Phase C capability veto: drop any anchor whose zone this engine has not
// loaded, so we never claim existence-authority over ground we cannot see.
//
// The first two raw anchors are the squad-tab LEADERS (interestCenters fills
// them before the camera anchors and only ever drops camera ones), which is the
// safety property the gate rests on - an unwatched squad still anchors its own
// surroundings. The veto can in principle remove a leader too, but only when
// its zone is unloaded here, and an unloaded zone holds no enumerable bodies to
// go dormant. Measured: hostZoneUnloaded=0 at 5,200 u separation, and dormPc=0
// (dormant bodies next to a player character) across every gated run.
//
// THROTTLED, and not for CPU cost. `isZoneLoadedAt` reaches into ZoneManager,
// whose members are boost shared_mutex-protected and written by the engine's own
// zone-loader thread. Before this veto the query ran ~1 Hz from the census
// diagnostics; the authority pass runs every tick, so calling it here put
// hundreds of acquisitions per second of main-thread traffic onto a lock the
// loader thread holds while streaming. Nothing measured proves that stalls the
// game loop - the stall signature in the logs predates this code by six weeks -
// but the trade is one-sided: the answer only changes as zones stream, on a
// multi-second scale, so a cached verdict costs nothing and a permanently
// wedged main thread costs the session. Re-query on the interval, or at once if
// an anchor jumped (a camera cut or teleport can cross a zone edge instantly).
unsigned int Replicator::attentionAnchors(GameWorld* gw, const float* raw,
                                          unsigned int nRaw, float* out) {
    if (!raw || !out) return 0;
    const unsigned long ATTN_VETO_MS  = 500;
    const float         ATTN_VETO_JUMP = 100.0f;
    unsigned long nowV = nowMs();
    bool reuse = (attnVetoMs_ != 0) && ((nowV - attnVetoMs_) < ATTN_VETO_MS)
                 && (attnVetoRawN_ == nRaw);
    for (unsigned int a = 0; reuse && a < nRaw; ++a)
        if (dist3(raw[a * 3 + 0], raw[a * 3 + 1], raw[a * 3 + 2],
                  attnVetoRaw_[a * 3 + 0], attnVetoRaw_[a * 3 + 1],
                  attnVetoRaw_[a * 3 + 2]) > ATTN_VETO_JUMP) reuse = false;
    if (reuse) {
        for (unsigned int i = 0; i < attnVetoOutN_ * 3; ++i) out[i] = attnVetoOut_[i];
        return attnVetoOutN_;
    }
    unsigned int nOut = 0;
    for (unsigned int a = 0; a < nRaw && nOut < 4; ++a) {
        const float* p = raw + a * 3;
        if (attentionRadius_ > 0.0f && !engine::isZoneLoadedAt(gw, p[0], p[1], p[2]))
            continue;
        out[nOut * 3 + 0] = p[0];
        out[nOut * 3 + 1] = p[1];
        out[nOut * 3 + 2] = p[2];
        ++nOut;
    }
    attnVetoMs_   = nowV;
    attnVetoRawN_ = nRaw;
    for (unsigned int i = 0; i < nRaw * 3 && i < 12; ++i) attnVetoRaw_[i] = raw[i];
    attnVetoOutN_ = nOut;
    for (unsigned int i = 0; i < nOut * 3; ++i) attnVetoOut_[i] = out[i];
    if (nOut < nRaw) {
        static unsigned long vetoLogMs = 0; // main-thread only
        if (vetoLogMs == 0 || (nowV - vetoLogMs) >= 10000) {
            vetoLogMs = nowV;
            char b[112]; _snprintf(b, sizeof(b) - 1,
                "[attn] anchor veto %u/%u (zone not loaded here)",
                nRaw - nOut, nRaw);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    return nOut;
}

bool Replicator::observedAt(const Key& k, const float* anchors,
                            unsigned int nAnchor, float x, float y, float z) {
    if (attentionRadius_ <= 0.0f) return true;   // gate off
    // No anchors at all means no players in gameplay (interestCenters returns
    // 0 before the squads exist). Fail OPEN - a startup tick must not declare
    // the whole world dormant.
    if (!anchors || nAnchor == 0) return true;
    // Leaving costs 25% more reach than entering. Both clients run this over
    // anchor sets that agree only approximately, so a body parked on the
    // threshold would flip out of phase between them without the deadband.
    const float ATTN_LEAVE_SCALE = 1.25f;
    std::map<Key, bool>::iterator it = attnObs_.find(k);
    bool was = (it != attnObs_.end()) && it->second;
    float r = was ? attentionRadius_ * ATTN_LEAVE_SCALE : attentionRadius_;
    bool now = false;
    for (unsigned int a = 0; a < nAnchor && !now; ++a) {
        float d = dist3(x, y, z, anchors[a * 3 + 0], anchors[a * 3 + 1],
                        anchors[a * 3 + 2]);
        now = (d <= r);
    }
    if (now && !was) ++attnFlips_;   // Phase B: an attach to be paid for
    if (it != attnObs_.end()) it->second = now;
    else                      attnObs_[k] = now;
    return now;
}

void Replicator::measureAttach(unsigned long now) {
    // Phase B, measurement only: what does an attach actually cost? Open a
    // window on the first one and report the suppressions, culls and proxy
    // mints that landed inside it. A window with nothing to show for it stays
    // silent - bodies drift across the threshold constantly as squads walk,
    // and one line per quiet crossing would bury the bursts this exists to
    // find.
    const unsigned long ATTN_WIN_MS = 3000;
    if (attnFlips_ > 0 && attnWinMs_ == 0) {
        attnWinMs_     = now;
        attnBaseSupp_  = authSuppresses_;
        attnBaseCull_  = censusCulls_;
        attnBaseProxy_ = (unsigned int)proxyByKey_.size();
    }
    if (attnWinMs_ == 0 || (now - attnWinMs_) < ATTN_WIN_MS) return;
    unsigned long dSupp = authSuppresses_ - attnBaseSupp_;
    unsigned long dCull = censusCulls_ - attnBaseCull_;
    int dProxy = (int)proxyByKey_.size() - (int)attnBaseProxy_;
    if (attnFlips_ >= 4 || dSupp || dCull || dProxy) {
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[attn] attach bodies=%lu hid=+%lu culled=+%lu minted=%+d winMs=%lu",
            attnFlips_, dSupp, dCull, dProxy, (unsigned long)ATTN_WIN_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    attnFlips_ = 0;
    attnWinMs_ = 0;
}

void Replicator::pruneAttention(const std::set<Key>& seen) {
    for (std::map<Key, bool>::iterator it = attnObs_.begin();
         it != attnObs_.end(); ) {
        if (seen.find(it->first) == seen.end()) attnObs_.erase(it++);
        else ++it;
    }
}

float Replicator::parkDivergedCopy(Character* c, const EntityState& st, const Key& k) {
    // v38 pack-hidden fix: existence culling exempts census-present NPCs, but
    // both clients then run INDEPENDENT sims of the same body - the join's
    // copy can stand somewhere the host's copy isn't (the "pack hidden" save:
    // a pack visible on the join with no host counterpart at that spot).
    // The census now carries the host position per row; reconcile a local
    // copy that drifted past the park distance with a halt+teleport onto the
    // host's spot. 120 u default: ABOVE town-schedule divergence (two sims
    // seating the same bar NPC ~50 u apart - run 185524), so only genuinely
    // divergent wanderers (measured 500-900 u) trip it.
    if (censusParkDist_ <= 0.0f) return -1.0f;
    std::map<Key, CensusPos>::iterator it = censusPos_.find(k);
    if (it == censusPos_.end()) return -1.0f;
    float d = dist3(st.x, st.y, st.z, it->second.x, it->second.y, it->second.z);
    if (d <= censusParkDist_) return d;
    // Per-key cooldown (npc_sync regression, run 185524): the engine's own
    // schedule AI can re-place the body the same frame (a seated copy), so an
    // unthrottled park re-teleported every frame and wrecked tracking. One
    // park per key per cooldown bounds the fight; a free wanderer sticks on
    // the first try.
    unsigned long nowP = nowMs();
    // A FROZEN body (AI suspended for repeat divergence) reparks on a 1 s
    // cooldown: run 014948 showed a frozen slave still re-pathed to ~600 u
    // between 5 s parks (an in-flight movement goal survives the suspend),
    // so the clamp must out-pace the walk. Unfrozen bodies keep the 5 s
    // cooldown that protects seat-schedule NPCs from park thrash.
    unsigned long cool = censusFrozen_.count(k) ? 1000 : 5000;
    std::map<Key, unsigned long>::iterator pm = parkMs_.find(k);
    if (pm != parkMs_.end() && (nowP - pm->second) < cool) return d;
    parkMs_[k] = nowP;
    // Anchor break (world_parity 2026-07-17): a census copy chained/caged at
    // the WRONG fixture (cross-client furniture identity is unreliable) is
    // position-anchored - every park teleport snapped straight back (Nutto:
    // parks=543, local pos constant at d=381 all run). Release the local
    // furniture first, then park. A chained body is NOT re-chained here:
    // kind-3 re-entry (setChainedMode with the LOCAL slaveOwner) snapped the
    // body straight back to that owner's spot ~500 u away (run 012555:
    // Lungrot re-anchored kind=3 at d~450-520 on EVERY 5 s park, forever).
    // The divergence freeze below keeps the parked body inert instead; the
    // lock-state stream still owns the shackle item itself.
    engine::FurnitureRead lfr;
    bool anchored = engine::readFurniture(c, &lfr) && lfr.valid && lfr.kind != 0;
    if (anchored) {
        engine::applyFurniture(0, c, lfr.furn, lfr.kind, false);
        engine::endAction(c);
        char nm[48]; engine::charName(c, nm, sizeof(nm));
        char b[144]; _snprintf(b, sizeof(b) - 1,
            "[census] park ANCHOR-BREAK hand=%u,%u name='%s' kind=%d d=%.0f",
            k.i, k.s, nm, lfr.kind, d);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    if (engine::park(c, it->second.x, it->second.y, it->second.z, st.heading)) {
        ++censusParks_;
        static unsigned long logTick = 0; // main-thread only, ~4 lines/s
        unsigned long now = nowMs();
        if ((now - logTick) >= 250) {
            logTick = now;
            char nm[48]; engine::charName(c, nm, sizeof(nm));
            char b[192]; _snprintf(b, sizeof(b) - 1,
                "[census] park hand=%u,%u name='%s' d=%.0f local=%.0f,%.0f,%.0f "
                "host=%.0f,%.0f,%.0f (parks=%lu)",
                k.i, k.s, nm, d, st.x, st.y, st.z,
                it->second.x, it->second.y, it->second.z, censusParks_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    return d;
}

// Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI, join only): the position
// park above teleports a diverged census-band body back to the host's spot, but
// its LOCAL AI kept re-deciding to flee/fight (a captive/working slave with no
// supervisor on the join), so it ran off and aggroed the join's guards while the
// host had it working. Suspend its AI while it is diverging. Divergence-gated so
// well-tracking census NPCs (bar-seaters ~50 u apart) keep their local AI: only
// a body that crossed censusParkDist_ is frozen, HELD ~5 s past the last over-
// threshold tick (so the park zeroing its drift can't oscillate it back into
// fleeing), then re-checked (released if it settled, re-frozen if it diverges
// again). Runs AFTER applyTargets' per-tick clearAiSuspend(), so the suspend
// added here stands for the tick; addAiSuspend is a no-op if the AI-suspend
// detour is not installed (KENSHICOOP_AI_SUSPEND=0).
void Replicator::censusFreezeDivergedAi(Character* c, const Key& k, float drift) {
    if (!c) return;
    // 20 s hold (was 5 s): a diverged working slave released after only 5 s
    // below-threshold walked back toward its local job spot / owner and was
    // over the 120 u park line again before the next 5 s park cooldown fired
    // (run 012555: Lungrot oscillated 300-500 u every park, all run). The
    // longer hold keeps a repeat offender inert across several park cycles;
    // a genuinely settled body still releases and never re-arms.
    const unsigned long HOLD_MS = 20000;
    unsigned long now = nowMs();
    bool over = (drift > censusParkDist_); // censusParkDist_ > 0 implied (drift >= 0)
    std::map<Key, unsigned long>::iterator it = censusFrozen_.find(k);
    bool wasFrozen = (it != censusFrozen_.end());
    if (over) {
        censusFrozen_[k] = now;                 // (re)arm / refresh the hold
        if (!wasFrozen) engine::endAction(c);   // drop the in-progress flee/attack once
    } else if (wasFrozen) {
        if ((now - it->second) >= HOLD_MS) {    // settled: release, resume local AI
            censusFrozen_.erase(it);
            return;
        }
    } else {
        return;                                 // never diverged: leave local AI alone
    }
    engine::addAiSuspend(c);                     // quiesce AI decisions this tick
    // A destination committed BEFORE the suspend keeps the body running (run
    // 014948: frozen slave re-pathed ~600 u between parks); kill it per tick.
    engine::haltMovement(c);
    static unsigned long logTick = 0;           // main-thread only, ~4 lines/s
    if ((now - logTick) >= 250) {
        logTick = now;
        char nm[48]; engine::charName(c, nm, sizeof(nm));
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[census] FREEZE hand=%u,%u name='%s' d=%.0f (frozen=%u)",
            k.i, k.s, nm, drift, (unsigned)censusFrozen_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::pruneDebugMarkers(const std::set<Character*>& live) {
    for (std::map<Character*, DebugMarker>::iterator it = debugMarkers_.begin();
         it != debugMarkers_.end(); ) {
        if (live.find(it->first) == live.end()) {
            engine::markerDestroy(it->second.label);
            debugMarkers_.erase(it++);
        } else ++it;
    }
}


} // namespace coop
