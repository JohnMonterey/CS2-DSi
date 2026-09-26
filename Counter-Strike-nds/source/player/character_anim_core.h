// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Procedural character animation: the skinned player rig and the poses that drive it.

#ifndef CHARACTER_ANIM_CORE_H_ /* Include guard */
#define CHARACTER_ANIM_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * No Nitro Engine, no libnds: this compiles for the host test runner as well as the DS
 * (make test-anim). character_anim.c turns a pose into hardware matrices.
 *
 * The rig is data/player_rig.bin, made by tools/assets/player_rig.py from the static
 * player model. Every vertex of it belongs to one bone, and its display list restores that
 * bone's matrix from the hardware matrix stack before sending the vertex, so a pose costs
 * one matrix per bone and nothing per vertex. See that script for the file layout.
 *
 * Fixed point throughout, so the host and the console compute the same poses:
 * angles are 32768 per turn (what glRotate*i takes), lengths and blends are Q12.
 * Model space is the mesh's own: y up, the character faces -z, its right is +x.
 */

#define ANIM_TURN 32768
#define ANIM_ONE 4096
#define ANIM_DEG(d) ((int32_t)((d) * ANIM_TURN / 360))

// Hardware matrix stack slots that hold the bone matrices while a character is drawn.
// Must equal FIRST_SLOT in player_rig.py; CharacterRig_Parse() checks it.
#define RIG_FIRST_SLOT 22

enum RigBone
{
    RIG_ROOT = 0, // no vertices; falls and whole-body offsets pivot at the feet
    RIG_PELVIS,
    RIG_CHEST, // torso and shoulders
    RIG_HEAD,
    RIG_ARMS, // forearms, hands and the pistol, pivoting at the shoulder line
    RIG_THIGH_L, // the character's left leg (-x), forward in the rest pose
    RIG_SHIN_L,
    RIG_THIGH_R,
    RIG_SHIN_R,
    RIG_BONE_COUNT
};

#define RIG_NO_SLOT 0xFF

typedef struct
{
    int8_t parent;     // -1 for the root; always an earlier bone
    uint8_t slot;      // matrix stack slot - RIG_FIRST_SLOT, or RIG_NO_SLOT
    int16_t restPitch; // the limb's angle from straight down in the mesh, + forward
    int16_t length;    // to the next joint, Q12 model units (legs only)
    int16_t pivot[3];  // Q12 model units
} RigBoneInfo;

typedef struct
{
    RigBoneInfo bones[RIG_BONE_COUNT];
    const uint32_t *displayList; // glCallList format: a word count, then the commands
    uint32_t displayListWords;   // including the count word
    int32_t restReach;           // the longer leg's reach below the hips in the mesh, Q12
} CharacterRig;

// Checks the header, the bone hierarchy and the display list bounds. The data must stay
// alive (and 4-byte aligned) as long as the rig is used; nothing is copied but the bones.
bool CharacterRig_Parse(CharacterRig *rig, const uint8_t *data, uint32_t size);

/*
 * A bone's transform, applied about its pivot: roll (z) first, then pitch (x), then yaw
 * (y). Pitch + swings a hanging limb forward; yaw + turns toward the character's left.
 */
typedef struct
{
    int16_t pitch, yaw, roll;
} RigBonePose;

typedef struct
{
    RigBonePose bones[RIG_BONE_COUNT];
    int32_t offset[3]; // added to the root, Q12 model units (keeps a planted foot down)
} RigPose;

// What the game knows about a character this frame.
typedef struct
{
    int32_t x, y, z;   // world position, Q12 (an NE_Model's coordinates)
    int32_t yaw;       // facing, ANIM_TURN units; the direction the model's -z points
    int32_t aimPitch;  // look angle, + up
    int32_t frames;    // frames since the previous update, >= 1
    int32_t recoil;    // 0..ANIM_ONE, the weapon's kick right now (it decays on its own)
    bool dead;
} CharacterAnimInput;

// Everything that carries over between frames. Zero it, then CharacterAnim_Reset().
typedef struct
{
    int32_t lastX, lastZ;  // world position at the previous update, Q12
    int32_t speed;         // smoothed ground speed, Q12 world units per frame
    int32_t moveBlend;     // 0 standing .. ANIM_ONE moving
    int32_t runBlend;      // 0 walking .. ANIM_ONE running
    int32_t phase;         // gait cycle, ANIM_TURN per stride pair; wraps
    int32_t hipYaw;        // legs' heading relative to the facing
    int32_t backward;      // 1 while backpedalling (the gait runs in reverse)
    int32_t aimPitch;      // smoothed look angle
    int32_t recoil;        // as last given
    int32_t renderYaw;     // smoothed facing, ANIM_TURN units
    int32_t deathFrames;   // frames since death, -1 while alive
    uint32_t time;         // frames since the reset, for the idle motion
    uint32_t seed;         // per-character offset so a crowd does not breathe in step
} CharacterAnimState;

// Starts from the input's position and facing, standing still.
void CharacterAnim_Reset(CharacterAnimState *state, const CharacterAnimInput *input, uint32_t seed);

// Advances by input->frames. A respawn (dead, then alive) or a jump of more than a few
// units resets the gait and the facing rather than striding and turning through it.
void CharacterAnim_Update(CharacterAnimState *state, const CharacterAnimInput *input);

// The pose for the current state. The rig supplies rest angles and limb lengths.
void CharacterAnim_Pose(const CharacterAnimState *state, const CharacterRig *rig, RigPose *pose);

// Lowers the pistol from the aim to a relaxed low-ready: `lowered` 0 (aiming) to ANIM_ONE.
void CharacterAnim_LowerWeapon(RigPose *pose, int32_t lowered);

// Fixed-point helpers, exposed for the tests and the lobby.
int32_t AnimSin(int32_t angle); // Q12
int32_t AnimCos(int32_t angle); // Q12
int32_t AnimAtan2(int32_t y, int32_t x); // ANIM_TURN units, (-ANIM_TURN/2, ANIM_TURN/2]
int32_t AnimAngleDelta(int32_t from, int32_t to); // shortest signed turn from -> to
int32_t AnimApproach(int32_t current, int32_t target, int32_t rate); // rate Q12 per step
int32_t AnimEaseInOut(int32_t t); // smoothstep, Q12 in and out
int32_t AnimEaseOut(int32_t t);   // 1 - (1 - t)^3, Q12
int32_t AnimClamp(int32_t v, int32_t lo, int32_t hi);

#endif // CHARACTER_ANIM_CORE_H_
