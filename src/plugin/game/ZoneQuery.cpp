// ZoneQuery.cpp - the zone-loaded query (Phase 1 spawn parity) and the sector
// mappings behind it (engine::cellProbe), quarantined in its own TU because
// kenshi/ZoneManager.h redefines ParticlePool (also defined by
// kenshi/CombatClass.h, which EngineInternal.cpp needs) - the two vendored
// headers cannot share a translation unit.
//
// Why the query exists: within a locally LOADED world block, every baked
// (shared-save) NPC resolves by hand. So an unresolvable census hand whose
// host-reported position sits in a loaded block is a genuine host RUNTIME
// spawn - safe to proxy-mint at any distance without duplicate risk. This
// generalizes the fixed spawnMintRadius_ gate (a cheap stand-in for "the
// block here is certainly loaded") to the engine's own answer.

// ZoneManager.h pulls boost/thread headers (shared_mutex members), whose
// auto-link pragma demands libboost_thread-*.lib. We only form member-function
// pointers on the class (never instantiate it), so no boost code is generated -
// suppress the auto-link instead of shipping the library.
#define BOOST_ALL_NO_LIB

#include <windows.h>
#include <string.h>             // memset (CellProbe zero-fill)

#include <core/Functions.h>     // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>   // GameWorld::zoneMgr
#include <kenshi/ZoneManager.h> // ZoneManager::_NV_isZoneLoadedT / _NV_isZoneBeingLoadedT

#include "Engine.h"             // coop::engine::CellProbe (POD; pulls no kenshi headers)
#include "../CoopLog.h"         // the sector-call shape tripwire

namespace coop {
namespace engine {

namespace {
// this=RCX, const Ogre::Vector3& = pointer in RDX.
typedef bool (__fastcall* ZoneLoadedFn)(ZoneManager* self, const Ogre::Vector3* pos);
ZoneLoadedFn g_zoneLoadedFn      = 0;
ZoneLoadedFn g_zoneBeingLoadedFn = 0;

// Sector mappings, for cell authority (engine::cellAt) and its measurement
// (engine::cellProbe). Measured 2026-08-03: getMapSector at (-51178, 2655)
// returns (20,32) and the save folder holds zone.20.32.zone - the mapping IS
// the zone-file coordinate.
//
// The hidden return-buffer pointer is spelled out ON PURPOSE. iVector2 declares
// its own constructors, and MSVC returns EVERY class with a non-trivial
// constructor through a caller-provided buffer regardless of size - the "8 bytes
// so it comes back in RAX" rule only holds for trivially copyable PODs. This
// file used to carry the by-value spelling, which looked right and even READ
// right, and was corrupting the engine on every call: spelled by-value, MSVC
// passed OUR buffer in RCX and zm in RDX, while the real member wants `this` in
// RCX and the buffer in RDX. So getMapSector wrote its answer over the first
// qword of the ZoneManager - a class with pure-virtual bases, so that qword is
// its VTABLE POINTER - then returned that same address in RAX, from which we
// copied the correct sector. Right answer, stomped engine object, which is
// exactly why it survived the cell_probe validation.
// Field evidence 2026-08-03: with cellAuth on, both clients faulted at the
// identical instruction inside ZoneManager's own code (kenshi_x64.exe+0xa137b3,
// the 0xA0-0xA1 band these methods live in) reading 0x0000001F00000025 - a
// packed (x,y) sector pair whose y matched each client's own camera cell
// exactly, 31 on the host and 30 on the join.
typedef iVector2* (__fastcall* SectorXZFn)(ZoneManager* self, iVector2* ret,
                                           float x, float z);
SectorXZFn g_mapSectorFn = 0;
SectorXZFn g_subSectorFn = 0;
SectorXZFn g_resCoordFn  = 0;

// One sector call, plus a standing tripwire on the object's first qword. The
// buffer is a bare int pair so we never have to construct an iVector2 (its
// constructors live in the engine); the layout is {int x; int y} at offset 0.
// The tripwire is not paranoia - it is the specific check that would have caught
// the bug above at its first call instead of as an access violation minutes
// later in unrelated engine code, and it is what makes a future re-spelling of
// SectorXZFn fail loudly. Caller is inside SEH.
bool sectorCall(SectorXZFn fn, ZoneManager* zm, float x, float z,
                int* outX, int* outY) {
    if (!fn || !zm) return false;
    void* head = *(void**)zm;              // vptr; never a legal write target
    int ret[2] = { 0, 0 };
    fn(zm, (iVector2*)ret, x, z);
    if (*(void**)zm != head) {
        static bool told = false;
        if (!told) {
            told = true;
            coop::logLine("[cell] SHAPE getMapSector wrote through the "
                          "ZoneManager instead of the return buffer - the "
                          "calling convention is wrong (see SectorXZFn); "
                          "cell authority disabled to stop the corruption");
        }
        return false;
    }
    if (outX) *outX = ret[0];
    if (outY) *outY = ret[1];
    return true;
}

// getZoneBoundsT is deliberately NOT bound. It would hand back a cell's rect in
// one call, but it returns a 16-byte class through a hidden buffer pointer, and
// nothing in the headers says whether that pointer or `this` takes RCX. Both
// spellings were tried on 2026-08-03: the by-value typedef produced four
// identical junk floats and the explicit pointer form failed the "does the rect
// contain the point I asked about" check, i.e. neither convention matched.
//
// It is not needed. Walking getMapSector until the coord changes measures the
// same geometry using only an int pair in RAX, and the cell_probe scenario did
// exactly that: 4608 x 4608 u cells on an origin of -147456 (= -32 cells), with
// the predicted coord matching the engine's on both axes. A rect is a
// convenience; a wrong rect used to decide what is inside a cell is a bug.
} // namespace

// Called from engine::resolve(). Resolved via the _NV_ non-virtual aliases
// (concrete RVAs; the virtual names resolve to vtable thunks).
void resolveZoneQuery() {
    g_zoneLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneLoadedT);
    g_zoneBeingLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneBeingLoadedT);

    // Non-virtual, so the member pointer is the real address (the RootObjectFactory
    // ::createItem precedent). getMapSector is overloaded, so it needs a typed
    // member pointer to pick the (float,float) one.
    iVector2 (ZoneManager::*pMap)(float, float) const = &ZoneManager::getMapSector;
    g_mapSectorFn = (SectorXZFn)KenshiLib::GetRealAddress(pMap);
    g_subSectorFn = (SectorXZFn)KenshiLib::GetRealAddress(&ZoneManager::getSubMapSector);
    g_resCoordFn  = (SectorXZFn)KenshiLib::GetRealAddress(
        &ZoneManager::getZoneMapFromResolutionCoord);

}

bool isZoneLoadedAt(GameWorld* gw, float x, float y, float z) {
    if (!gw || !g_zoneLoadedFn) return false;
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        Ogre::Vector3 p(x, y, z);
        if (!g_zoneLoadedFn(zm, &p)) return false;
        // A block MID-LOAD hasn't materialized its baked NPCs yet - treating it
        // as loaded would far-mint a duplicate of a body about to appear.
        if (g_zoneBeingLoadedFn && g_zoneBeingLoadedFn(zm, &p)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool cellAt(GameWorld* gw, float x, float z, int* outCx, int* outCz) {
    if (!gw || !g_mapSectorFn) return false;
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        return sectorCall(g_mapSectorFn, zm, x, z, outCx, outCz);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool cellProbe(GameWorld* gw, float x, float y, float z, CellProbe* out) {
    if (!gw || !out) return false;
    memset(out, 0, sizeof(*out));
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        if (sectorCall(g_mapSectorFn, zm, x, z, &out->mapX, &out->mapY))
            out->haveMap = 1;
        if (sectorCall(g_subSectorFn, zm, x, z, &out->subX, &out->subY))
            out->haveSub = 1;
        if (sectorCall(g_resCoordFn, zm, x, z, &out->resX, &out->resY))
            out->haveRes = 1;
        (void)y;   // the sector grid is 2D; y is accepted only to match callers
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace engine
} // namespace coop
