// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Tombstones for the dead. See tombstone_core.h.

#include "tombstone_core.h"
#include "character_anim_core.h"

#include <string.h>

// The engraving: raised block letters on the front face, below the round top. They stand
// clear of the face by more than the depth buffer can tell apart out to about 10 units (a
// step there is about 3e-4 * d^2 units with the game's projection); further off they are a
// couple of pixels high. An equal depth test in the face's plane does not work: the large
// face and the small letters do not interpolate depth closely enough.
// At 0.28 high with a 0.075 stroke they read to about 9 units off.
#define LETTER_HEIGHT 1147 // 0.28
#define LETTER_STROKE 307  // 0.075
#define LETTER_C_WIDTH 737 // 0.18
#define LETTER_T_WIDTH 819 // 0.20
#define LETTER_GAP 184     // 0.045
#define LETTER_BOTTOM 1188 // 0.29, so the top is at 0.57
#define LETTER_LIFT 205    // 0.05

// How far below its resting place a stone buried `sink` deep starts: its top at its bottom.
#define RISE_DEPTH(sink) (TOMB_TOP + (sink))

static int32_t mulQ12(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 12);
}

// The light the stone stands in (Q12, 1 = full); set while a mesh is built.
static int32_t light = ANIM_ONE;

static uint16_t gray(int level)
{
    level = (level * light + ANIM_ONE / 2) >> 12;
    if (level < 0)
        level = 0;
    if (level > 30)
        level = 30;
    return TOMB_RGB(level, level, level + 1); // a touch cool, like stone
}

// A level blended between `low` at the bottom of the buried part and `high` at the shoulder,
// so the stone darkens toward the ground it stands in.
static uint16_t shade(int32_t y, int low, int high)
{
    int32_t t = (y + TOMB_SINK) * 16 / (TOMB_SHOULDER + TOMB_SINK);
    if (t > 16)
        t = 16;
    return gray(low + (high - low) * t / 16);
}

// Adds a flat polygon, reversing its corners if needed so it winds counter-clockwise seen
// from the side `outward` points to.
static void addPolygon(TombMesh *mesh, const TombVertex *corners, int count, const int32_t outward[3], bool letter)
{
    if (mesh->count >= TOMB_MAX_POLYGONS)
        return;
    int64_t ux = corners[1].x - corners[0].x, uy = corners[1].y - corners[0].y, uz = corners[1].z - corners[0].z;
    int64_t vx = corners[2].x - corners[0].x, vy = corners[2].y - corners[0].y, vz = corners[2].z - corners[0].z;
    int64_t nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    bool flip = nx * outward[0] + ny * outward[1] + nz * outward[2] < 0;

    TombPolygon *p = &mesh->polygons[mesh->count++];
    p->count = (uint8_t)count;
    p->letter = letter;
    for (int k = 0; k < count; k++)
        p->v[k] = corners[flip ? count - 1 - k : k];
}

static TombVertex vertex(int32_t x, int32_t y, int32_t z, uint16_t color)
{
    TombVertex v = {(int16_t)x, (int16_t)y, (int16_t)z, color};
    return v;
}

// A rectangle of the engraving, in the reader's coordinates: u to the reader's right (-x,
// since the reader faces the stone's front from -z), v up.
static void addLetterBar(TombMesh *mesh, int32_t u0, int32_t v0, int32_t u1, int32_t v1)
{
    static const int32_t front[3] = {0, 0, -1};
    int32_t z = -TOMB_HALF_THICKNESS - LETTER_LIFT;
    uint16_t c = gray(6);
    TombVertex q[4] = {vertex(-u0, v0, z, c), vertex(-u1, v0, z, c), vertex(-u1, v1, z, c), vertex(-u0, v1, z, c)};
    addPolygon(mesh, q, 4, front, true);
}

static void addLetterC(TombMesh *mesh, int32_t u)
{
    int32_t v0 = LETTER_BOTTOM, v1 = LETTER_BOTTOM + LETTER_HEIGHT, s = LETTER_STROKE;
    addLetterBar(mesh, u, v0, u + s, v1);
    addLetterBar(mesh, u + s, v1 - s, u + LETTER_C_WIDTH, v1);
    addLetterBar(mesh, u + s, v0, u + LETTER_C_WIDTH, v0 + s);
}

static void addLetterT(TombMesh *mesh, int32_t u)
{
    int32_t v0 = LETTER_BOTTOM, v1 = LETTER_BOTTOM + LETTER_HEIGHT, s = LETTER_STROKE;
    int32_t mid = u + LETTER_T_WIDTH / 2;
    addLetterBar(mesh, u, v1 - s, u + LETTER_T_WIDTH, v1);
    addLetterBar(mesh, mid - s / 2, v0, mid + s - s / 2, v1 - s);
}

void Tomb_BuildMesh(TombMesh *mesh, bool counterTerrorist, int32_t lightQ12)
{
    memset(mesh, 0, sizeof(*mesh));
    light = lightQ12;
    const int32_t W = TOMB_HALF_WIDTH, D = TOMB_HALF_THICKNESS, H = TOMB_SHOULDER, S = TOMB_SINK;

    // The round top's outline, from the right shoulder over to the left one.
    int32_t ax[TOMB_ARCH_SEGMENTS + 1], ay[TOMB_ARCH_SEGMENTS + 1];
    for (int k = 0; k <= TOMB_ARCH_SEGMENTS; k++)
    {
        int32_t angle = k * (ANIM_TURN / 2) / TOMB_ARCH_SEGMENTS;
        ax[k] = mulQ12(W, AnimCos(angle));
        ay[k] = H + mulQ12(W, AnimSin(angle));
    }

    // Front and back: the upright part, then the round top as trapezoids closing in from the
    // shoulders to a triangle at the crown. The first trapezoid's base is the upright part's
    // top edge exactly, so no vertex sits in the middle of another polygon's edge (the DS can
    // show a crack there).
    for (int side = 0; side < 2; side++)
    {
        int32_t z = side == 0 ? -D : D;
        int32_t out[3] = {0, 0, side == 0 ? -1 : 1};
        int low = side == 0 ? 5 : 4, high = side == 0 ? 18 : 15;
        TombVertex rect[4] = {vertex(-W, -S, z, shade(-S, low, high)), vertex(W, -S, z, shade(-S, low, high)),
                              vertex(W, H, z, shade(H, low, high)), vertex(-W, H, z, shade(H, low, high))};
        addPolygon(mesh, rect, 4, out, false);
        uint16_t crown = gray(high + 1);
        int n = TOMB_ARCH_SEGMENTS;
        for (int j = 0; j < n / 2 - 1; j++)
        {
            TombVertex q[4] = {vertex(ax[j], ay[j], z, crown), vertex(ax[j + 1], ay[j + 1], z, crown),
                               vertex(ax[n - j - 1], ay[n - j - 1], z, crown), vertex(ax[n - j], ay[n - j], z, crown)};
            addPolygon(mesh, q, 4, out, false);
        }
        TombVertex t[3] = {vertex(ax[n / 2 - 1], ay[n / 2 - 1], z, crown), vertex(ax[n / 2], ay[n / 2], z, crown),
                           vertex(ax[n / 2 + 1], ay[n / 2 + 1], z, crown)};
        addPolygon(mesh, t, 3, out, false);
    }

    // The sides, the buried bottom, and the round top's band, lighter as it faces up.
    {
        static const int32_t right[3] = {1, 0, 0}, left[3] = {-1, 0, 0}, down[3] = {0, -1, 0};
        TombVertex r[4] = {vertex(W, -S, -D, shade(-S, 4, 16)), vertex(W, -S, D, shade(-S, 4, 16)),
                           vertex(W, H, D, shade(H, 4, 16)), vertex(W, H, -D, shade(H, 4, 16))};
        addPolygon(mesh, r, 4, right, false);
        TombVertex l[4] = {vertex(-W, -S, -D, shade(-S, 4, 16)), vertex(-W, -S, D, shade(-S, 4, 16)),
                           vertex(-W, H, D, shade(H, 4, 16)), vertex(-W, H, -D, shade(H, 4, 16))};
        addPolygon(mesh, l, 4, left, false);
        uint16_t c = gray(4);
        TombVertex b[4] = {vertex(-W, -S, -D, c), vertex(W, -S, -D, c), vertex(W, -S, D, c), vertex(-W, -S, D, c)};
        addPolygon(mesh, b, 4, down, false);
    }
    for (int k = 0; k < TOMB_ARCH_SEGMENTS; k++)
    {
        int32_t mid = (2 * k + 1) * (ANIM_TURN / 4) / TOMB_ARCH_SEGMENTS;
        int32_t out[3] = {AnimCos(mid), AnimSin(mid), 0};
        uint16_t c0 = gray(16 + 4 * (ay[k] - H) / W), c1 = gray(16 + 4 * (ay[k + 1] - H) / W);
        TombVertex q[4] = {vertex(ax[k], ay[k], -D, c0), vertex(ax[k + 1], ay[k + 1], -D, c1),
                           vertex(ax[k + 1], ay[k + 1], D, c1), vertex(ax[k], ay[k], D, c0)};
        addPolygon(mesh, q, 4, out, false);
    }

    // The engraving, centred.
    if (counterTerrorist)
    {
        int32_t u = -(LETTER_C_WIDTH + LETTER_GAP + LETTER_T_WIDTH) / 2;
        addLetterC(mesh, u);
        addLetterT(mesh, u + LETTER_C_WIDTH + LETTER_GAP);
    }
    else
    {
        addLetterT(mesh, -LETTER_T_WIDTH / 2);
    }

    // The stone's quads, then its triangles, then the engraving, so drawing starts as few
    // batches as it can. Static: the ARM9's stack is small.
    static TombPolygon sorted[TOMB_MAX_POLYGONS];
    int n = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < mesh->count; i++)
        {
            const TombPolygon *p = &mesh->polygons[i];
            if ((pass == 0 && !p->letter && p->count == 4) || (pass == 1 && !p->letter && p->count == 3) ||
                (pass == 2 && p->letter))
                sorted[n++] = *p;
        }
    memcpy(mesh->polygons, sorted, sizeof(TombPolygon) * n);
}

int Tomb_BodyAlpha(int32_t deathFrames)
{
    if (deathFrames < TOMB_FADE_START)
        return 31;
    int32_t t = deathFrames - TOMB_FADE_START;
    if (t >= TOMB_FADE_FRAMES)
        return 0;
    // (1 - t)^2: quickly off full strength, then a long faint tail.
    int32_t left = ANIM_ONE - t * ANIM_ONE / TOMB_FADE_FRAMES;
    int alpha = (31 * mulQ12(left, left) + ANIM_ONE / 2) >> 12;
    if (alpha > 30)
        alpha = 30;
    return alpha < 1 ? 1 : alpha;
}

bool Tomb_Visible(int32_t age)
{
    return age >= TOMB_RISE_START;
}

int32_t Tomb_RiseOffset(int32_t age, int32_t sink)
{
    int32_t t = age - TOMB_RISE_START;
    if (t <= 0)
        return -RISE_DEPTH(sink);
    if (t >= TOMB_RISE_FRAMES)
        return 0;
    return -mulQ12(RISE_DEPTH(sink), ANIM_ONE - AnimEaseOut(t * ANIM_ONE / TOMB_RISE_FRAMES));
}

int32_t Tomb_SinkOffset(int32_t age, int32_t sink)
{
    if (age <= 0)
        return 0;
    if (age >= TOMB_SINK_FRAMES)
        return -RISE_DEPTH(sink);
    // Slow to start, then gone: the rise played backwards.
    return -mulQ12(RISE_DEPTH(sink), ANIM_ONE - AnimEaseOut(ANIM_ONE - age * ANIM_ONE / TOMB_SINK_FRAMES));
}

bool Tomb_Sunk(int32_t age)
{
    return age >= TOMB_SINK_FRAMES;
}

int32_t Tomb_VertexY(int32_t y, int32_t sink, int32_t offset)
{
    if (y <= -TOMB_SINK)
        y = -sink;
    y += offset;
    return y < -sink ? -sink : y;
}

void Tomb_ProbeBegin(TombGroundProbe *probe, int32_t x, int32_t z, int32_t feet)
{
    probe->x = x;
    probe->z = z;
    probe->feet = feet;
    probe->ground = feet;
    probe->found = false;
}

static void consider(TombGroundProbe *probe, int32_t top)
{
    if (top > probe->feet + TOMB_GROUND_REACH)
        return; // above the body: a ceiling, or a ledge it stood beside
    if (!probe->found || top > probe->ground)
    {
        probe->ground = top;
        probe->found = true;
    }
}

void Tomb_ProbeBox(TombGroundProbe *probe, int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ, int32_t top)
{
    if (probe->x < minX || probe->x > maxX || probe->z < minZ || probe->z > maxZ)
        return;
    consider(probe, top);
}

void Tomb_ProbeRamp(TombGroundProbe *probe, int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ, bool alongX,
                    int32_t yAtMin, int32_t yAtMax)
{
    if (probe->x < minX || probe->x > maxX || probe->z < minZ || probe->z > maxZ)
        return;
    int32_t lo = alongX ? minX : minZ, hi = alongX ? maxX : maxZ, at = alongX ? probe->x : probe->z;
    int32_t y = yAtMin;
    if (hi > lo)
        y = yAtMin + (int32_t)((int64_t)(yAtMax - yAtMin) * (at - lo) / (hi - lo));
    consider(probe, y);
}

int32_t Tomb_ProbeGround(const TombGroundProbe *probe)
{
    return probe->ground;
}

// ----------------------------------------------------------------------------------------
// Placing a stone
// ----------------------------------------------------------------------------------------

typedef struct
{
    int32_t y;
    bool found, sloped;
    int32_t minX, maxX, minZ, maxZ; // the surface's extent
} Floor;

static int32_t clampI(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int64_t isqrt64(int64_t v)
{
    int64_t r = 0, bit = (int64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit != 0)
    {
        if (v >= r + bit)
        {
            v -= r + bit;
            r = (r >> 1) + bit;
        }
        else
            r >>= 1;
        bit >>= 2;
    }
    return r;
}

// How far in from its floor's edges a stone moved back onto it goes: its footprint's radius,
// sqrt(0.27^2 + 0.07^2) = 0.279, and 0.03 to spare.
#define NUDGE_MARGIN 1266

static int32_t rampY(const TombRamp *r, int32_t x, int32_t z)
{
    int32_t lo = r->alongX ? r->minX : r->minZ, hi = r->alongX ? r->maxX : r->maxZ;
    int32_t at = clampI(r->alongX ? x : z, lo, hi);
    if (hi <= lo)
        return r->yAtMin;
    return r->yAtMin + (int32_t)((int64_t)(r->yAtMax - r->yAtMin) * (at - lo) / (hi - lo));
}

static void offer(Floor *f, int32_t y, bool sloped, int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ)
{
    if (f->found && y <= f->y)
        return;
    f->y = y;
    f->found = true;
    f->sloped = sloped;
    f->minX = minX;
    f->maxX = maxX;
    f->minZ = minZ;
    f->maxZ = maxZ;
}

// The highest floor under the point (x, z) no higher than `limit`; with `half` > 0, under the
// square of that half size around it instead (strictly overlapping, as the physics does).
static Floor floorUnder(const TombMap *map, int32_t x, int32_t z, int32_t half, int32_t limit)
{
    Floor f = {0, false, false, 0, 0, 0, 0};
    for (int i = 0; i < map->boxCount; i++)
    {
        const TombBox *b = &map->boxes[i];
        bool under = half > 0 ? b->minX < x + half && b->maxX > x - half && b->minZ < z + half && b->maxZ > z - half
                              : x >= b->minX && x <= b->maxX && z >= b->minZ && z <= b->maxZ;
        if (under && b->top <= limit)
            offer(&f, b->top, false, b->minX, b->maxX, b->minZ, b->maxZ);
    }
    for (int i = 0; i < map->rampCount; i++)
    {
        const TombRamp *r = &map->ramps[i];
        bool under = half > 0 ? r->minX < x + half && r->maxX > x - half && r->minZ < z + half && r->maxZ > z - half
                              : x >= r->minX && x <= r->maxX && z >= r->minZ && z <= r->maxZ;
        if (!under)
            continue;
        // Under the hull's edge, the ramp is as high as it gets nearest the centre.
        int32_t y = rampY(r, x, z);
        if (y <= limit)
            offer(&f, y, r->yAtMin != r->yAtMax, r->minX, r->maxX, r->minZ, r->maxZ);
    }
    return f;
}

// The stone's four corners around (x, z) at `yaw`.
static void corners(int32_t x, int32_t z, int32_t yaw, int32_t cx[4], int32_t cz[4])
{
    int32_t s = AnimSin(yaw), c = AnimCos(yaw);
    // The stone's +x (its right) is (cos, -sin) in the world, its front (-z) is (-sin, -cos).
    int32_t rx = mulQ12(TOMB_HALF_WIDTH, c), rz = -mulQ12(TOMB_HALF_WIDTH, s);
    int32_t fx = -mulQ12(TOMB_HALF_THICKNESS, s), fz = -mulQ12(TOMB_HALF_THICKNESS, c);
    for (int k = 0; k < 4; k++)
    {
        int32_t a = (k & 1) ? 1 : -1, b = (k & 2) ? 1 : -1;
        cx[k] = x + a * rx + b * fx;
        cz[k] = z + a * rz + b * fz;
    }
}

void Tomb_Place(const TombMap *map, int32_t x, int32_t z, int32_t feet, int32_t reach, int32_t yaw,
                TombPlacement *out)
{
    // What the hull stood on.
    Floor at = floorUnder(map, x, z, 0, feet + reach);
    Floor edge = floorUnder(map, x, z, TOMB_HULL_HALF, feet + TOMB_REACH_CORNER);
    Floor support = at;
    if (edge.found && (!at.found || edge.y > at.y))
        support = edge;
    if (!support.found)
    {
        out->x = x;
        out->z = z;
        out->ground = feet;
        out->sink = TOMB_SINK_SLOPE; // no floor known: bury it as deep as any
        return;
    }

    // A little ahead, the stone's axis-aligned extent kept inside the hull's square.
    int32_t s = AnimSin(yaw), c = AnimCos(yaw);
    int32_t as = s < 0 ? -s : s, ac = c < 0 ? -c : c;
    int32_t extentX = mulQ12(TOMB_HALF_WIDTH, ac) + mulQ12(TOMB_HALF_THICKNESS, as);
    int32_t extentZ = mulQ12(TOMB_HALF_WIDTH, as) + mulQ12(TOMB_HALF_THICKNESS, ac);
    // (Less a few units for the rounding in the sines.)
    int32_t roomX = TOMB_HULL_HALF - extentX - 8, roomZ = TOMB_HULL_HALF - extentZ - 8;
    int32_t sx = x + clampI(-mulQ12(TOMB_AHEAD, s), -roomX, roomX);
    int32_t sz = z + clampI(-mulQ12(TOMB_AHEAD, c), -roomZ, roomZ);

    // A corner over a drop: move the stone back onto the floor it stands on, as far in as
    // its own extent from that floor's edges, or to its middle if the floor is too narrow.
    int32_t cx[4], cz[4];
    corners(sx, sz, yaw, cx, cz);
    bool overhang = false;
    for (int k = 0; k < 4; k++)
    {
        Floor f = floorUnder(map, cx[k], cz[k], 0, support.y + TOMB_REACH_CORNER);
        if (!f.found || f.y < support.y - TOMB_LEDGE_DROP)
            overhang = true;
    }
    if (overhang)
    {
        int32_t margin = NUDGE_MARGIN;
        int32_t tx = support.maxX - support.minX < 2 * margin ? (support.minX + support.maxX) / 2
                                                              : clampI(sx, support.minX + margin, support.maxX - margin);
        int32_t tz = support.maxZ - support.minZ < 2 * margin ? (support.minZ + support.maxZ) / 2
                                                              : clampI(sz, support.minZ + margin, support.maxZ - margin);
        int64_t dx = tx - sx, dz = tz - sz;
        int64_t d2 = dx * dx + dz * dz;
        if (d2 > (int64_t)TOMB_NUDGE_MAX * TOMB_NUDGE_MAX)
        {
            int64_t d = isqrt64(d2);
            dx = dx * TOMB_NUDGE_MAX / d;
            dz = dz * TOMB_NUDGE_MAX / d;
        }
        sx += (int32_t)dx;
        sz += (int32_t)dz;
        corners(sx, sz, yaw, cx, cz);
    }

    // Down past the lowest floor under it that is not beyond a ledge.
    int32_t lowest = support.y;
    bool sloped = support.sloped;
    for (int k = 0; k < 4; k++)
    {
        Floor f = floorUnder(map, cx[k], cz[k], 0, support.y + TOMB_REACH_CORNER);
        if (!f.found || f.y < support.y - TOMB_LEDGE_DROP)
            continue;
        if (f.y < lowest)
            lowest = f.y;
        sloped |= f.sloped;
    }
    out->x = sx;
    out->z = sz;
    out->ground = support.y;
    out->sink = support.y - lowest + (sloped ? TOMB_SINK_SLOPE : TOMB_SINK_FLAT);
}
