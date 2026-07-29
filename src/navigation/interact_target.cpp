#include "navigation/interact_target.h"
#include "navigation/nav_rva.h"
#include "navigation/entity_scan.h"
#include "navigation/player_state.h"
#include "core/hooks.h"
#include "core/mem_read.h"
#include "core/logger.h"
#include "core/phyre_types.h"
#include "speech/speech.h"
#include "speech/phrasebook.h"

#include <cmath>
#include <cstdio>

using namespace MemRead;

namespace InteractTarget {

namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

// Resolve (container, slot) the way FUN_00268d10 does: the container's entries array, then the
// object pointer at entries+0x08+slot*8. Same walk EntityDiag already does for its [c:i] lines.
void* ResolveSlot(int32_t container, int32_t slot) {
    if (container < 0 || slot < 0) return nullptr;
    if (static_cast<uint32_t>(container) >= NavRva::HANDLE_TABLE_CONTAINERS) return nullptr;

    void* base = Hooks::ResolveRva(NavRva::HANDLE_TABLE_BASE);
    if (!base) return nullptr;
    void* table = static_cast<char*>(base) +
                  static_cast<size_t>(container) * NavRva::HANDLE_TABLE_STRIDE;

    uint8_t active = 0;
    if (!SafeReadU8(table, NavRva::TBL_ACTIVE_OFF, &active) || (active & 1) == 0) return nullptr;
    void* entries = PtrAt(table, NavRva::TBL_ENTRIES_OFF);
    if (!entries) return nullptr;
    uint32_t count = 0;
    if (!SafeReadU32(entries, NavRva::ENTRIES_COUNT_OFF, &count)) return nullptr;
    if (static_cast<uint32_t>(slot) >= count) return nullptr;

    return PtrAt(entries, NavRva::ENTRIES_SLOT0_OFF + static_cast<uint32_t>(slot) * 8);
}

float WrapPi(float a) {
    while (a < -kPi) a += kTau;
    while (a >  kPi) a -= kTau;
    return a;
}

} // namespace

uint8_t ObjectClass(void* sceneObj) {
    uint8_t b = 0;
    if (!sceneObj || !SafeReadU8(sceneObj, NavRva::SCENEOBJ_TYPE_BYTE, &b)) return 0;
    return static_cast<uint8_t>(b >> NavRva::SCENEOBJ_CLASS_SHIFT);
}

namespace {

// The target's position AS THE GATES SEE IT -- a replica of the engine's own getter FUN_002646c0.
//
// Class 3 (characters) adds xform[0x40..0x48] when the byte at xform+0x107 has bit 0 set, BEFORE
// both the distance gate and the band test. Class 1 (gimmicks) uses the plain position. The engine
// returns (0,0,0) for any other class; we return the raw position instead, because a caller wanting
// a position is better served by an approximate one than by the origin of the map.
void ReadGatePos(void* sceneObj, void* xform, float& x, float& y, float& z) {
    x = y = z = 0.0f;
    SafeReadF32(xform, NavRva::XFORM_POS_X, &x);
    SafeReadF32(xform, NavRva::XFORM_POS_Y, &y);
    SafeReadF32(xform, NavRva::XFORM_POS_Z, &z);

    if (ObjectClass(sceneObj) != NavRva::SCENEOBJ_CLASS_CHAR) return;

    uint8_t off = 0;
    if (!SafeReadU8(xform, NavRva::XFORM_POS_OFFSET_FLAG, &off) || (off & 1) == 0) return;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    SafeReadF32(xform, NavRva::XFORM_POS_OFFSET_X, &ox);
    SafeReadF32(xform, NavRva::XFORM_POS_OFFSET_Y, &oy);
    SafeReadF32(xform, NavRva::XFORM_POS_OFFSET_Z, &oz);
    x += ox; y += oy; z += oz;
}

// FUN_003da730 + FUN_003a1d30: the radius of `shape`'s ellipse along the direction (dx,dz) from it to
// the other party. Degenerate axes fall back to 0 rather than dividing by zero -- a shape with a zero
// semi-axis contributes no reach, which is what the engine's own `<= 0` early-out amounts to.
float EllipseRadius(void* xform, uint32_t shapeOff, float dx, float dz) {
    float a = 0.0f, b = 0.0f, yaw = 0.0f;
    SafeReadF32(xform, shapeOff + NavRva::SHAPE_SEMI_A, &a);
    SafeReadF32(xform, shapeOff + NavRva::SHAPE_SEMI_B, &b);
    SafeReadF32(xform, shapeOff + NavRva::SHAPE_YAW,    &yaw);
    if (!(a > 1e-6f) || !(b > 1e-6f)) return 0.0f;

    const float c = std::cos(-yaw), s = std::sin(-yaw);
    const float w = c * dz - s * dx;
    const float u = s * dz + c * dx;
    const float u2 = u * u, w2 = w * w;
    if (u2 + w2 <= 0.0f) return 0.0f;                     // coincident points -- engine returns 0
    const float den = u2 / (a * a) + w2 / (b * b);
    if (!(den > 1e-12f)) return 0.0f;
    const float r2 = (u2 + w2) / den;
    return std::sqrt(r2 < 0.0f ? -r2 : r2);               // FUN_003a1d30 is sqrtf(fabs(x))
}

// Direction-independent lower bound on one shape's contribution: the ellipse is never smaller than its
// short semi-axis, so min(A,B) is reachable from every approach angle.
float EllipseMin(void* xform, uint32_t shapeOff) {
    float a = 0.0f, b = 0.0f;
    SafeReadF32(xform, shapeOff + NavRva::SHAPE_SEMI_A, &a);
    SafeReadF32(xform, shapeOff + NavRva::SHAPE_SEMI_B, &b);
    if (!(a > 1e-6f) || !(b > 1e-6f)) return 0.0f;
    return a < b ? a : b;
}

} // namespace

Chosen Read() {
    Chosen c;
    uint8_t have = 0;
    SafeReadU8(Hooks::ResolveRva(NavRva::INTERACT_HAVE), 0, &have);

    int32_t cont = -1, slot = -1, mode = 0;
    uint32_t uc = 0, us = 0, um = 0;
    if (SafeReadU32(Hooks::ResolveRva(NavRva::INTERACT_CONT), 0, &uc)) cont = static_cast<int32_t>(uc);
    if (SafeReadU32(Hooks::ResolveRva(NavRva::INTERACT_SLOT), 0, &us)) slot = static_cast<int32_t>(us);
    if (SafeReadU32(Hooks::ResolveRva(NavRva::INTERACT_MODE), 0, &um)) mode = static_cast<int32_t>(um);
    SafeReadF32(Hooks::ResolveRva(NavRva::INTERACT_SCORE), 0, &c.score);

    c.container = cont;
    c.slot      = slot;
    c.mode      = mode;

    // FUN_00268650 gates on BOTH ids being >= 0; DAT_0209a2aa is the scorer's own "found one" flag.
    // Require all three -- the globals are reset per frame, so a stale read cannot survive, but a
    // torn mid-frame read could otherwise show a slot with no owner.
    if (!have || cont < 0 || slot < 0) return c;

    c.sceneObj = ResolveSlot(cont, slot);
    if (!c.sceneObj) return c;

    c.label = EntityScan::ResolveObjectName(c.sceneObj);
    c.valid = true;
    return c;
}

void SpeakCurrent() {
    const Chosen c = Read();

    // SILENT on nothing-in-reach: that is the normal state while walking around, and it is also the
    // answer to "why did Confirm do nothing" -- the log carries the detail, speech does not.
    if (!c.valid || c.label.empty()) {
        char m[160];
        snprintf(m, sizeof(m), "; field SILENT: valid=%d cont=%d slot=%d mode=%d obj=%p label=%s",
                 c.valid ? 1 : 0, c.container, c.slot, c.mode, c.sceneObj,
                 c.label.empty() ? "<empty>" : "<set>");
        Log::Write("INTERACT", m);
        return;
    }

    // The verb is the engine's own mode word, not a guess about what the object is.
    std::wstring say = std::wstring(Phrase::Get((c.mode == NavRva::INTERACT_MODE_TALK)
                                                    ? Phrase::Id::Talk : Phrase::Id::Action)) + L": ";
    say += c.label;
    Speech::Output(say, /*interrupt=*/true);
}

void LogChosen() {
    const Chosen c = Read();
    char nlabel[64] = {};
    for (size_t k = 0; k < c.label.size() && k < 63; ++k)
        nlabel[k] = (c.label[k] < 128) ? static_cast<char>(c.label[k]) : '?';

    char m[256];
    snprintf(m, sizeof(m),
             "==== game's chosen interaction target: valid=%d cont=%d slot=%d mode=%d(%s) "
             "score=%.3f obj=%p \"%s\" ====",
             c.valid ? 1 : 0, c.container, c.slot, c.mode,
             (c.mode == NavRva::INTERACT_MODE_TALK) ? "talk" :
             (c.mode == NavRva::INTERACT_MODE_ACTION) ? "action" : "?",
             c.score, c.sceneObj, nlabel);
    Log::Write("INTERACT", m);
}

// Replica of the three geometric gates in FUN_0025bad0, for ONE candidate. Diagnostic only -- it
// exists to answer "which gate rejects this NPC", so it logs the RAW inputs next to each verdict:
// if an offset here is wrong, the raw values make that obvious instead of quietly producing a
// plausible-looking FAIL.
//
// NOT a replacement for the engine's own answer -- LogChosen() above is ground truth. In particular
// the DISTANCE gate is only approximated: FUN_003da5a0 subtracts four extents, two of which come
// from direction-dependent shape queries we do not replicate. What is exact and important is that
// it uses sqrt(dx^2 + dz^2) -- interaction range is a CYLINDER, Y is excluded entirely.
Band ReadBandFor(void* sceneObj) {
    Band b;
    if (!sceneObj) return b;
    void* leader = PlayerState::ReadLeaderSceneObject();
    if (!leader) return b;
    void* pn = PtrAt(leader,   NavRva::SCENEOBJ_XFORM_PTR);
    void* tn = PtrAt(sceneObj, NavRva::SCENEOBJ_XFORM_PTR);
    if (!pn || !tn) return b;

    // CLASS 1 (gimmicks) has an entirely different band layout, and reading class 3's offsets on one
    // yields plausible-looking floats that are simply wrong. FUN_0025be50 rejects unless
    //     (node.y - node[+0x6C]) - playerPad*playerScale <= playerY <= node.y + node[+0x68]
    // with no band-scale multiplier, skipped when the byte at node+0x5C is non-zero.
    if (ObjectClass(sceneObj) == NavRva::SCENEOBJ_CLASS_VOLUME) {
        uint8_t vskip = 0;
        SafeReadU8(tn, NavRva::XFORM_V_SKIP_BAND, &vskip);
        if (vskip != 0) {
            b.valid = true; b.unconstrained = true;
            b.lo = -1e9f; b.hi = 1e9f;
            return b;
        }
        float ty = 0.0f, up = 0.0f, down = 0.0f, pPad = 0.0f, pScale = 0.0f;
        SafeReadF32(tn, NavRva::XFORM_POS_Y,             &ty);
        SafeReadF32(tn, NavRva::XFORM_V_BAND_UP,         &up);
        SafeReadF32(tn, NavRva::XFORM_V_BAND_DOWN,       &down);
        SafeReadF32(pn, NavRva::XFORM_BAND_PLAYER_PAD,   &pPad);
        SafeReadF32(pn, NavRva::XFORM_BAND_SCALE,        &pScale);
        b.lo = (ty - down) - pPad * pScale;
        b.hi = ty + up;
        if (b.hi < b.lo) return b;
        b.valid = true;
        return b;
    }

    uint8_t skip = 0;
    SafeReadU8(tn, NavRva::XFORM_SKIP_HEIGHT_BAND, &skip);
    if (skip != 0) {                      // engine skips the band test -> it constrains nothing
        b.valid = true; b.unconstrained = true;
        b.lo = -1e9f; b.hi = 1e9f;
        return b;
    }

    float ty = 0.0f;
    SafeReadF32(tn, NavRva::XFORM_POS_Y, &ty);
    float addLo = 0.0f, addHi = 0.0f;
    if (void* bs = PtrAt(sceneObj, NavRva::SCENEOBJ_BAND_STRUCT)) {
        SafeReadF32(bs, NavRva::BAND_STRUCT_LO, &addLo);
        SafeReadF32(bs, NavRva::BAND_STRUCT_HI, &addHi);
    }
    float tScale = 0, tUp = 0, tDown = 0, pPad = 0, pScale = 0;
    SafeReadF32(tn, NavRva::XFORM_BAND_SCALE,      &tScale);
    SafeReadF32(tn, NavRva::XFORM_BAND_UP,         &tUp);
    SafeReadF32(tn, NavRva::XFORM_BAND_DOWN,       &tDown);
    SafeReadF32(pn, NavRva::XFORM_BAND_PLAYER_PAD, &pPad);
    SafeReadF32(pn, NavRva::XFORM_BAND_SCALE,      &pScale);

    const float centre = addLo + addHi + ty;
    b.lo = (centre - tScale * tDown) - pPad * pScale;
    b.hi = centre + tScale * tUp;
    if (b.hi < b.lo) return b;            // torn read -> report invalid rather than an inverted band
    b.valid = true;
    return b;
}

Reach ReadReachFor(void* sceneObj) {
    Reach r;
    if (!sceneObj) return r;
    void* leader = PlayerState::ReadLeaderSceneObject();
    if (!leader) return r;
    void* pn = PtrAt(leader,   NavRva::SCENEOBJ_XFORM_PTR);
    void* tn = PtrAt(sceneObj, NavRva::SCENEOBJ_XFORM_PTR);
    if (!pn || !tn) return r;

    float px, py, pz, tx, ty, tz;
    ReadGatePos(leader, pn, px, py, pz);
    ReadGatePos(sceneObj, tn, tx, ty, tz);
    (void)py; (void)ty;                               // Y is excluded: the gate is a CYLINDER

    const float dx = tx - px, dz = tz - pz;
    r.dist2D = std::sqrt(dx * dx + dz * dz);

    // Argument order matches the two FUN_003da730 calls: the player's ellipse is evaluated toward the
    // target, the target's toward the player. Same magnitude either way for a circle, not for an
    // ellipse -- so the sign of the delta matters and is kept.
    r.ellipsePlayer = EllipseRadius(pn, NavRva::XFORM_PLAYER_SHAPE,  dx,  dz);
    r.ellipseTarget = EllipseRadius(tn, NavRva::XFORM_TARGET_SHAPE, -dx, -dz);
    SafeReadF32(pn, NavRva::XFORM_PLAYER_SHAPE + NavRva::SHAPE_EXTRA_RADIUS, &r.extraPlayer);
    SafeReadF32(tn, NavRva::XFORM_TARGET_SHAPE + NavRva::SHAPE_EXTRA_RADIUS, &r.extraTarget);

    r.radius    = r.ellipsePlayer + r.extraPlayer + r.ellipseTarget + r.extraTarget;
    r.radiusMin = EllipseMin(pn, NavRva::XFORM_PLAYER_SHAPE) + r.extraPlayer
                + EllipseMin(tn, NavRva::XFORM_TARGET_SHAPE) + r.extraTarget;
    r.passes    = (r.dist2D < r.radius);
    r.valid     = true;

    // The engine's own answer for the same quantity -- but only under TWO conditions, and the second
    // one is easy to miss.
    //
    // 1. This object must be the one the engine chose. The score global is a single minimised slot,
    //    so reading it for anything else attributes another candidate's number to this one.
    // 2. It must be a CLASS-3 target. `DAT_0209a2b0` mixes units between the two scorers:
    //    FUN_0025bad0 stores `2*dist2D - reach` (hence the identity), while FUN_0025be50 stores a
    //    plain distance measure from FUN_003a1960. Applying the identity to a class-1 winner
    //    produces a confident, meaningless number -- exactly the kind of thing that gets promoted to
    //    fact because it printed next to the word CONFIRMED.
    const Chosen c = Read();
    if (c.valid && c.sceneObj == sceneObj &&
        ObjectClass(sceneObj) == NavRva::SCENEOBJ_CLASS_CHAR) {
        r.haveMeasured = true;
        r.measured     = 2.0f * r.dist2D - c.score;
    }
    return r;
}

void LogGatesFor(void* sceneObj, const char* label) {
    if (!sceneObj) return;
    void* leader = PlayerState::ReadLeaderSceneObject();
    if (!leader) return;
    void* pn = PtrAt(leader,   NavRva::SCENEOBJ_XFORM_PTR);
    void* tn = PtrAt(sceneObj, NavRva::SCENEOBJ_XFORM_PTR);
    if (!pn || !tn) return;

    // Through the class-aware getter, so the logged gates are the ones the engine actually applied
    // rather than class-3 arithmetic pointed at a class-1 object.
    float px = 0, py = 0, pz = 0, tx = 0, ty = 0, tz = 0;
    ReadGatePos(leader,   pn, px, py, pz);
    ReadGatePos(sceneObj, tn, tx, ty, tz);
    const uint8_t cls = ObjectClass(sceneObj);

    const float dx = tx - px, dz = tz - pz;
    const float dist2D = std::sqrt(dx * dx + dz * dz);

    // ---- vertical band ----------------------------------------------------------------------
    uint8_t skipBand = 0;
    SafeReadU8(tn, NavRva::XFORM_SKIP_HEIGHT_BAND, &skipBand);

    float bandLoAdd = 0.0f, bandHiAdd = 0.0f;
    void* bandStruct = PtrAt(sceneObj, NavRva::SCENEOBJ_BAND_STRUCT);
    if (bandStruct) {
        SafeReadF32(bandStruct, NavRva::BAND_STRUCT_LO, &bandLoAdd);
        SafeReadF32(bandStruct, NavRva::BAND_STRUCT_HI, &bandHiAdd);
    }
    float tScale = 0, tUp = 0, tDown = 0, pPad = 0, pScale = 0;
    SafeReadF32(tn, NavRva::XFORM_BAND_SCALE,      &tScale);
    SafeReadF32(tn, NavRva::XFORM_BAND_UP,         &tUp);
    SafeReadF32(tn, NavRva::XFORM_BAND_DOWN,       &tDown);
    SafeReadF32(pn, NavRva::XFORM_BAND_PLAYER_PAD, &pPad);
    SafeReadF32(pn, NavRva::XFORM_BAND_SCALE,      &pScale);

    const float centre = bandLoAdd + bandHiAdd + ty;
    const float lo = (centre - tScale * tDown) - pPad * pScale;
    const float hi = centre + tScale * tUp;
    const bool bandOk = (skipBand != 0) || !(py < lo || hi < py);

    // ---- facing cone ------------------------------------------------------------------------
    float yaw = 0, halfAngle = 0;
    SafeReadF32(pn, NavRva::XFORM_FACE_YAW_INTERACT, &yaw);
    SafeReadF32(tn, NavRva::XFORM_CONE_HALF_ANGLE,   &halfAngle);
    const float bearing = std::atan2(dx, dz);          // FUN_003a1bb0: atan2(tx-px, tz-pz)
    const float delta   = std::fabs(WrapPi(bearing - yaw));
    const bool coneOk   = delta < halfAngle;

    char m[400];
    snprintf(m, sizeof(m),
             "  gates \"%s\" class=%u: dist2D=%.2f (Y EXCLUDED) | band %s py=%.2f in [%.2f,%.2f] "
             "skip=%u centre=%.2f sc=%.2f up=%.2f dn=%.2f pad=%.2f | cone %s |d|=%.3f half=%.3f yaw=%.3f",
             label ? label : "?", cls, dist2D,
             bandOk ? "PASS" : "FAIL", py, lo, hi, skipBand, centre, tScale, tUp, tDown, pPad,
             coneOk ? "PASS" : "FAIL", delta, halfAngle, yaw);
    Log::Write("INTERACT", m);
}

} // namespace InteractTarget
