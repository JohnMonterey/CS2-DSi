// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Tombstones for the dead: the mesh, the timing of the fade and the rise, and where the
// ground is. tombstone.c places and draws them.

#ifndef TOMBSTONE_CORE_H_ /* Include guard */
#define TOMBSTONE_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * No Nitro Engine, no libnds: this compiles for the host test runner too (make test-anim).
 *
 * Lengths are world units in Q12 (4096 = 1 unit, an NE_Model coordinate; the player's hull
 * is 1.8 units tall). The mesh is in the stone's own space: y up from the ground it stands
 * on, the engraved front facing -z the way a character model faces, +x to its right. So
 * glRotateYi(yaw) with a character's facing turns the engraving the way that character
 * looked. Every polygon is wound counter-clockwise seen from outside, as the rig's are, so
 * back faces cull.
 */

#define TOMB_HALF_WIDTH 1106    // 0.27
#define TOMB_HALF_THICKNESS 287 // 0.07
#define TOMB_SHOULDER 2540      // 0.62: where the sides end and the round top begins
#define TOMB_TOP (TOMB_SHOULDER + TOMB_HALF_WIDTH)
// The mesh's buried bottom. Each stone is drawn with its own depth instead (Tomb_Place): all
// the way below whatever floor lies under it, deeper on a ramp, whose steps can lie well below
// the line players walk on, and shallow on a flat floor, which may be a thin slab over a room.
#define TOMB_SINK 2867 // 0.70
#define TOMB_ARCH_SEGMENTS 6

#define TOMB_MAX_POLYGONS 32

// RGB15, as GFX_COLOR takes it.
#define TOMB_RGB(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

typedef struct
{
    int16_t x, y, z; // Q12, all within the +-8 a GFX_VERTEX16 can carry
    uint16_t color;
} TombVertex;

typedef struct
{
    TombVertex v[4];
    uint8_t count; // 3 or 4
    bool letter;   // part of the engraving, raised on the front face
} TombPolygon;

typedef struct
{
    TombPolygon polygons[TOMB_MAX_POLYGONS]; // the stone's quads, its triangles, then the engraving
    int count;
} TombMesh;

// The stone, engraved "CT" or "T", its colours scaled by the light it stands in (Q12: 4096 in
// the open, less in the map's shadow).
void Tomb_BuildMesh(TombMesh *mesh, bool counterTerrorist, int32_t lightQ12);

// The light in the map's shadow zones, as the game dims the gun there.
#define TOMB_SHADOW_LIGHT 2867 // 0.7

/*
 * Timing, in 60 Hz frames since the death. The body falls (the rig's death pose, 40 frames)
 * solid, then fades, quickly at first: a translucent body is drawn in display-list order
 * rather than nearest first, which shows most while it is nearly opaque. It is gone before
 * the quickest respawn (2 s, in training, deathmatch and gun game) would pop it back up.
 * The stone rises out of the ground once the body is faint, and stands as it vanishes. A
 * stone left from an earlier death sinks back into the ground when the player dies again.
 */
#define TOMB_FADE_START 40
#define TOMB_FADE_FRAMES 75
#define TOMB_RISE_START 76
#define TOMB_RISE_FRAMES 40
#define TOMB_SINK_FRAMES 40

// The stone stands up to this far ahead of the death spot, along the facing, clear of the
// fallen body's feet; but never out of the player's own hull, so not in a wall it stood
// against.
#define TOMB_AHEAD 1024 // 0.25

// The dead body's polygon alpha: 31 opaque down to 1, then 0 once it is gone. Alpha 0 draws
// wireframe on the DS, so 0 means "do not draw it". Negative frames (alive) give 31.
int Tomb_BodyAlpha(int32_t deathFrames);

// Whether the stone shows yet, and how far below its resting place it is (Q12, <= 0), for a
// stone buried `sink` deep: it starts folded down into its buried bottom, top and all, and
// grows out of it (see Tomb_VertexY), so it comes out of the lowest floor under it.
bool Tomb_Visible(int32_t age);
int32_t Tomb_RiseOffset(int32_t age, int32_t sink);

// Where a mesh vertex at height `y` goes for a stone buried `sink` deep and `offset` below
// its resting place: the mesh's bottom at the stone's own depth, and nothing ever below it,
// so a rising or sinking stone never reaches through a floor it clears at rest.
int32_t Tomb_VertexY(int32_t y, int32_t sink, int32_t offset);

// A stone sinking away, `age` frames after it started: how far down it is (Q12, <= 0), and
// whether it has gone. The rise played backwards.
int32_t Tomb_SinkOffset(int32_t age, int32_t sink);
bool Tomb_Sunk(int32_t age);

/*
 * The ground under a point: the highest collision surface there whose top is no more than
 * TOMB_GROUND_REACH above the feet, so a death in mid-air lands the stone on the floor below
 * while a body that sank a little into the floor still finds the floor it was on. Feed it
 * every wall box and ramp of the map, then read the result; with nothing under the point
 * it falls back to the feet.
 */
#define TOMB_GROUND_REACH 1843 // 0.45

typedef struct
{
    int32_t x, z;  // the point, Q12
    int32_t feet;  // Q12
    int32_t ground;
    bool found;
} TombGroundProbe;

void Tomb_ProbeBegin(TombGroundProbe *probe, int32_t x, int32_t z, int32_t feet);
// An axis-aligned box, Q12 bounds.
void Tomb_ProbeBox(TombGroundProbe *probe, int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ, int32_t top);
// A ramp over a rectangle whose surface rises linearly along x (alongX) or z, from yAtMin at
// the rectangle's low edge to yAtMax at its high edge.
void Tomb_ProbeRamp(TombGroundProbe *probe, int32_t minX, int32_t maxX, int32_t minZ, int32_t maxZ, bool alongX,
                    int32_t yAtMin, int32_t yAtMax);
int32_t Tomb_ProbeGround(const TombGroundProbe *probe);

/*
 * Where a stone stands. The map's collision: wall boxes, whose tops are floors, and ramps,
 * whose surface a standing player's feet follow.
 */
typedef struct
{
    int32_t minX, maxX, minZ, maxZ, top;
} TombBox;

typedef struct
{
    int32_t minX, maxX, minZ, maxZ;
    bool alongX;            // the surface rises along x, else along z
    int32_t yAtMin, yAtMax; // at the low and high edge along that axis
} TombRamp;

typedef struct
{
    const TombBox *boxes;
    int boxCount;
    const TombRamp *ramps;
    int rampCount;
} TombMap;

typedef struct
{
    int32_t x, z;   // the stone's centre
    int32_t ground; // the floor it stands on
    int32_t sink;   // how far below that its bottom goes
} TombPlacement;

#define TOMB_HULL_HALF 1434        // 0.35: half a player's hull, either way
#define TOMB_REACH_CORNER 410      // 0.1: a floor under the hull's edge may be this far above the feet
#define TOMB_LEDGE_DROP 2458       // 0.6: a corner over a floor this much lower is over a ledge
#define TOMB_NUDGE_MAX 2580        // 0.63: the most a stone is moved back onto its floor
#define TOMB_SINK_FLAT 614         // 0.15 below a flat floor
#define TOMB_SINK_SLOPE 2662       // 0.65 below a ramp's line

/*
 * The player's hull stood centred on (x, z) with its feet at `feet`, facing `yaw`. Its floor
 * is the highest surface under the centre within `reach` above the feet, or under the hull's
 * footprint within TOMB_REACH_CORNER (the hull stands on anything it overlaps, so a death on
 * the seam at a ramp's top is on the platform, not the floor under the seam). The stone goes
 * a little ahead of the centre, inside the hull; if a corner of it would hang over a drop, it
 * is moved back onto its floor. Its bottom goes TOMB_SINK_FLAT below the lowest floor under
 * it, or TOMB_SINK_SLOPE if that is a ramp. With no floor at all, it stands at the feet.
 */
void Tomb_Place(const TombMap *map, int32_t x, int32_t z, int32_t feet, int32_t reach, int32_t yaw,
                TombPlacement *out);

#endif // TOMBSTONE_CORE_H_
