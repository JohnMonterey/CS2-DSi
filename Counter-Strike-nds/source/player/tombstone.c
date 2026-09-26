// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Tombstones for the dead: placing them and drawing them. See tombstone.h.

#include "tombstone.h"
#include "tombstone_core.h"
#include "character_anim.h"
#include "death_view.h"
#include "main.h"
#include "map.h"
#include "party.h"
#include "network.h"

typedef struct
{
    bool standing;   // a stone stands here, or is about to rise, or is sinking away
    int32_t x, y, z; // Q12: the ground under the death spot
    int32_t sink;    // how far below that its bottom goes
    int32_t yaw;     // ANIM_TURN units: the facing at death
    bool ct;         // engraved CT rather than T
    bool shadowed;   // stands in one of the map's shadow zones
    float angle;     // the player's Angle at death
    int deathFrame;  // frameCount at the death
    int sinkFrame;   // frameCount when it started sinking away
    int zone;        // the map zone it stands in, for culling
} Tombstone;

// What killPlayer() saw: a count of deaths, and the side and facing at the last one. Read by
// the next Tombstone_Update(), which may come a frame or two later, by when the half-time
// side swap may have happened and the dead local player may have looked around.
typedef struct
{
    unsigned serial, placed; // deaths noted, deaths given a stone
    int team;
    float angle;
} DeathNote;

// Per player slot: its stone, the one before it (sinking away), and its last death.
static Tombstone stones[MaxPlayer];
static Tombstone leaving[MaxPlayer];
static DeathNote notes[MaxPlayer];

// CT and T, in the open and in shadow.
static TombMesh meshes[2][2];
static bool meshesBuilt = false;

// The map's collision in the stone placer's terms: wall boxes and ramps, in f32. Wall boxes
// are kept in a doubled scale (CreateWall stores the position times 8192 and adds the half
// size at 4096), A the high side and B the low one. Ramps are read as CheckStairs reads them:
// side A is the low edge, and a standing player's centre follows the line, so its feet are
// 0.9 below it.
#define MAX_BOXES 256
#define MAX_RAMPS 48
static TombBox boxes[MAX_BOXES];
static TombRamp ramps[MAX_RAMPS];

static TombMap collisionMap(void)
{
    TombMap m = {boxes, 0, ramps, 0};
    Wall *walls = getMapWalls();
    int wallsInMap = getMapWallsCount();
    for (int i = 0; walls != NULL && i < wallsInMap && m.boxCount < MAX_BOXES; i++)
    {
        const CollisionBox *box = &walls[i].WallCollisionBox;
        TombBox b = {box->BoxXRangeB >> 1, box->BoxXRangeA >> 1, box->BoxZRangeB >> 1, box->BoxZRangeA >> 1,
                     box->BoxYRangeA >> 1};
        boxes[m.boxCount++] = b;
    }
    const Map *map = &allMaps[currentMap];
    for (int i = 0; map->AllStairs != NULL && i < map->StairsCount && m.rampCount < MAX_RAMPS; i++)
    {
        const Stairs *s = &map->AllStairs[i];
        bool startAtMin = s->direction >= 2;
        int32_t start = (int32_t)(s->startY * 4096.0f) - 3686, end = (int32_t)(s->endY * 4096.0f) - 3686;
        TombRamp r = {(int32_t)(s->xSideA * 4096.0f), (int32_t)(s->xSideB * 4096.0f), (int32_t)(s->zSideA * 4096.0f),
                      (int32_t)(s->zSideB * 4096.0f), s->direction == 1 || s->direction == 3,
                      startAtMin ? start : end, startAtMin ? end : start};
        ramps[m.rampCount++] = r;
    }
    return m;
}

int32_t Tombstone_GroundAt(int32_t x, int32_t z, int32_t feet)
{
    TombGroundProbe probe;
    Tomb_ProbeBegin(&probe, x, z, feet);

    // Wall boxes are kept in a doubled scale (CreateWall stores the position times 8192 and
    // adds the half size at 4096), A the high side and B the low one.
    Wall *walls = getMapWalls();
    int wallsInMap = getMapWallsCount();
    for (int i = 0; walls != NULL && i < wallsInMap; i++)
    {
        const CollisionBox *box = &walls[i].WallCollisionBox;
        Tomb_ProbeBox(&probe, box->BoxXRangeB >> 1, box->BoxXRangeA >> 1, box->BoxZRangeB >> 1,
                      box->BoxZRangeA >> 1, box->BoxYRangeA >> 1);
    }

    // Ramps, as CheckStairs reads them: side A is the low edge, and a standing player's
    // centre follows the line, so its feet are 0.9 below it.
    const Map *map = &allMaps[currentMap];
    for (int i = 0; map->AllStairs != NULL && i < map->StairsCount; i++)
    {
        const Stairs *s = &map->AllStairs[i];
        bool alongX = s->direction == 1 || s->direction == 3;
        bool startAtMin = s->direction >= 2;
        int32_t start = (int32_t)(s->startY * 4096.0f) - 3686, end = (int32_t)(s->endY * 4096.0f) - 3686;
        Tomb_ProbeRamp(&probe, (int32_t)(s->xSideA * 4096.0f), (int32_t)(s->xSideB * 4096.0f),
                       (int32_t)(s->zSideA * 4096.0f), (int32_t)(s->zSideB * 4096.0f), alongX,
                       startAtMin ? start : end, startAtMin ? end : start);
    }
    return Tomb_ProbeGround(&probe);
}

// The map zone a point is in, found the way checkPlayerOcclusionZone finds a player's: the
// player's own zone if the point is in it, else the first that holds it, else the player's.
static int zoneAt(int32_t x, int32_t z, int playerZone)
{
    const Map *map = &allMaps[currentMap];
    float fx = x / 4096.0f, fz = z / 4096.0f;
    int found = -1;
    for (int i = 0; i < map->zonesCount; i++)
    {
        const CollisionBox2D *box = &map->AllZones[i].collisionBox;
        if (fx <= box->BoxXRangeA && fx >= box->BoxXRangeB && fz <= box->BoxZRangeA && fz >= box->BoxZRangeB)
        {
            if (i == playerZone)
                return i;
            if (found < 0)
                found = i;
        }
    }
    return found >= 0 ? found : playerZone;
}

void Tombstone_NoteDeath(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MaxPlayer)
        return;
    DeathNote *note = &notes[playerIndex];
    note->serial++;
    note->team = AllPlayers[playerIndex].Team;
    note->angle = AllPlayers[playerIndex].Angle;
}

// Frames since `frame`; a frame counter reset since then counts as long ago.
static int32_t since(int frame)
{
    int32_t age = frameCount - frame;
    return age < 0 ? 1 << 20 : age;
}

static void place(int index)
{
    Player *player = &AllPlayers[index];
    const DeathNote *note = &notes[index];

    // Only a side has a stone; and AddNewPlayer parks new models far below the map, where a
    // player whose death arrived before its position has nothing to mark.
    if (note->team != TERRORISTS && note->team != COUNTERTERRORISTS)
        return;
    if (player->PlayerModel->y < -50 * 4096)
        return;

    // The stone it had sinks away, unless it had not come up yet.
    Tombstone *stone = &stones[index];
    if (stone->standing && Tomb_Visible(since(stone->deathFrame)))
    {
        leaving[index] = *stone;
        leaving[index].sinkFrame = frameCount;
    }

    // The facing viewers saw: the pose's, still as it was when the player was last alive.
    // Otherwise (no rig) the view's at the moment of death. Player.Angle counts 512 to the
    // turn, glRotateYi 32768.
    if (!CharacterAnim_Facing(index, &stone->yaw))
        stone->yaw = (int32_t)(note->angle * (ANIM_TURN / 512));

    // On what the hull stood on. The local player's hull shrinks when crouched, and its feet
    // are where the physics put them. Everyone else's is the standing 0.9, and follows a
    // snapshot or a waypoint line, which can cut into a floor: search higher for theirs.
    int32_t half = index == 0 ? (int32_t)(player->ySize * 4096.0f) : 3686;
    int32_t reach = index == 0 ? TOMB_GROUND_REACH : 3686;
    TombMap map = collisionMap();
    TombPlacement at;
    Tomb_Place(&map, player->PlayerModel->x, player->PlayerModel->z, player->PlayerModel->y - half, reach, stone->yaw,
               &at);
    stone->x = at.x;
    stone->z = at.z;
    stone->y = at.ground;
    stone->sink = at.sink;
    stone->ct = note->team == COUNTERTERRORISTS;
    stone->shadowed = player->inShadow;
    stone->angle = note->angle;
    stone->deathFrame = frameCount;
    // The stone outlasts the player's stay in this zone, so it keeps its own.
    stone->zone = zoneAt(stone->x, stone->z, player->CurrentOcclusionZone);
    stone->standing = true;
}

void Tombstone_Update(void)
{
    for (int i = 0; i < MaxPlayer; i++)
    {
        Player *player = &AllPlayers[i];
        DeathNote *note = &notes[i];
        if (note->placed == note->serial)
            continue;
        note->placed = note->serial;
        if (player->Id != UNUSED && player->PlayerModel != NULL)
            place(i);
    }
}

void Tombstone_ClearPlayer(int playerIndex)
{
    if (playerIndex < 0 || playerIndex >= MaxPlayer)
        return;
    stones[playerIndex].standing = false;
    leaving[playerIndex].standing = false;
    notes[playerIndex].placed = notes[playerIndex].serial;
}

void Tombstone_ClearAll(void)
{
    for (int i = 0; i < MaxPlayer; i++)
        Tombstone_ClearPlayer(i);
}

bool Tombstone_Spot(int playerIndex, int32_t *x, int32_t *y, int32_t *z, int32_t *yaw, float *angleAtDeath,
                    int32_t *age)
{
    if (playerIndex < 0 || playerIndex >= MaxPlayer || !stones[playerIndex].standing)
        return false;
    const Tombstone *stone = &stones[playerIndex];
    *x = stone->x;
    *y = stone->y;
    *z = stone->z;
    *yaw = stone->yaw;
    *angleAtDeath = stone->angle;
    *age = since(stone->deathFrame);
    return true;
}

static void drawStone(const Tombstone *stone, const TombMesh *mesh, int32_t offset)
{
    MATRIX_PUSH = 0;
    glTranslatef32(stone->x, stone->y, stone->z);
    glRotateYi(stone->yaw);

    // Unlit, untextured, vertex-coloured. The format is latched when a batch begins.
    NE_PolyFormat(31, TOMB_POLY_ID, 0, NE_CULL_BACK, NE_MODULATION);
    GFX_TEX_FORMAT = 0;
    int batch = 0;
    for (int i = 0; i < mesh->count; i++)
    {
        const TombPolygon *p = &mesh->polygons[i];
        if (p->count != batch)
        {
            GFX_BEGIN = p->count == 4 ? GL_QUADS : GL_TRIANGLES;
            batch = p->count;
        }
        for (int k = 0; k < p->count; k++)
        {
            const TombVertex *v = &p->v[k];
            int32_t y = Tomb_VertexY(v->y, stone->sink, offset);
            GFX_COLOR = v->color;
            GFX_VERTEX16 = ((uint32_t)(uint16_t)y << 16) | (uint16_t)v->x;
            GFX_VERTEX16 = (uint16_t)v->z;
        }
    }

    MATRIX_POP = 1;
}

static bool shown(const Tombstone *stone, bool (*visible)(int zone, int x, int z))
{
    return stone->zone >= 0 && stone->zone < allMaps[currentMap].zonesCount && visible(stone->zone, stone->x, stone->z);
}

void Tombstone_DrawAll(bool (*visible)(int zone, int x, int z))
{
    if (!meshesBuilt)
    {
        for (int ct = 0; ct < 2; ct++)
            for (int shadowed = 0; shadowed < 2; shadowed++)
                Tomb_BuildMesh(&meshes[ct][shadowed], ct, shadowed ? TOMB_SHADOW_LIGHT : ANIM_ONE);
        meshesBuilt = true;
    }

    bool drew = false;
    for (int i = 0; i < MaxPlayer; i++)
    {
        Tombstone *old = &leaving[i];
        if (old->standing)
        {
            int32_t age = since(old->sinkFrame);
            if (Tomb_Sunk(age))
                old->standing = false;
            else if (shown(old, visible))
            {
                drawStone(old, &meshes[old->ct][old->shadowed], Tomb_SinkOffset(age, old->sink));
                drew = true;
            }
        }

        const Tombstone *stone = &stones[i];
        if (!stone->standing)
            continue;
        int32_t age = since(stone->deathFrame);
        if (!Tomb_Visible(age))
            continue;
        // A camera following a dead player from its eyes would be inside or on top of its
        // stone; the local player's death view looks at it from outside.
        if (i == CurrentCameraPlayer && AllPlayers[i].IsDead && !(i == 0 && DeathView_Active()))
            continue;
        if (!shown(stone, visible))
            continue;
        drawStone(stone, &meshes[stone->ct][stone->shadowed], Tomb_RiseOffset(age, stone->sink));
        drew = true;
    }
    if (drew)
        NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
}
