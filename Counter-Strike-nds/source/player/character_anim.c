// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Drawing animated characters: the rig from character_anim_core, posed with hardware
// matrices.

#include "character_anim.h"
#include "main.h"
#include "network.h"
#include "dsidev.h"
#include "player_rig_bin.h"

static CharacterRig rig;
static bool rigReady = false;

// Per player. The model pointer tells a state apart from one kept for a player that has
// since been removed and re-added.
static CharacterAnimState states[MaxPlayer];
static NE_Model *stateModel[MaxPlayer];
static RigPose poses[MaxPlayer];

static int lastUpdateFrame = 0;
static bool updatedOnce = false;

void CharacterAnim_Init(void)
{
    rigReady = CharacterRig_Parse(&rig, player_rig_bin, player_rig_bin_size);
    if (!rigReady)
        dsidev_log("player_rig.bin did not parse; drawing the static model");
}

bool CharacterAnim_Ready(void)
{
    return rigReady;
}

const CharacterRig *CharacterAnim_Rig(void)
{
    return rigReady ? &rig : NULL;
}

void CharacterAnim_ResetPlayer(int playerIndex)
{
    if (playerIndex >= 0 && playerIndex < MaxPlayer)
        stateModel[playerIndex] = NULL;
}

// What the game knows about a player, in the core's terms.
static void readInput(const Player *player, int frames, CharacterAnimInput *input)
{
    const NE_Model *model = player->PlayerModel;
    input->x = model->x;
    input->y = model->y;
    input->z = model->z;
    // ry and Angle count 512 to the turn; glRotateYi takes 32768.
    input->yaw = (int32_t)(player->Angle * (ANIM_TURN / 512));
    input->frames = frames;
    input->dead = player->IsDead;

    // Bots look at their target; people send the angle of their camera, level at 128.
    input->aimPitch = 0;
    if (player->isAi)
    {
        if (player->target >= 0 && player->target < MaxPlayer)
        {
            const Player *target = &AllPlayers[player->target];
            if (target->Id != UNUSED && !target->IsDead && target->PlayerModel != NULL)
            {
                int32_t dx = target->PlayerModel->x - model->x;
                int32_t dy = target->PlayerModel->y - model->y;
                int32_t dz = target->PlayerModel->z - model->z;
                int32_t flat = (int32_t)sqrtf((float)dx * dx + (float)dz * dz);
                input->aimPitch = AnimAtan2(dy, flat);
            }
        }
    }
    else
    {
        input->aimPitch = (int32_t)((128 - player->cameraAngle) * (ANIM_TURN / 512));
    }

    // The first-person gun's kick is kept for everyone and decays each tick from
    // GunMaxRecoil down to GunMinRecoil.
    int kick = player->rightGunYRecoil > player->leftGunYRecoil ? player->rightGunYRecoil : player->leftGunYRecoil;
    kick -= GunMinRecoil;
    input->recoil = kick <= 0 ? 0 : kick * ANIM_ONE / (GunMaxRecoil - GunMinRecoil);
}

void CharacterAnim_UpdatePlayers(void)
{
    if (!rigReady)
        return;

    // Frames since the last update: 1 at 60 Hz, 2 in dual-screen mode. frameCount can be
    // reset under us; then count one.
    int frames = frameCount - lastUpdateFrame;
    if (!updatedOnce || frames < 1 || frames > 8)
        frames = 1;
    lastUpdateFrame = frameCount;
    updatedOnce = true;

    for (int i = 1; i < MaxPlayer; i++)
    {
        Player *player = &AllPlayers[i];
        if (player->Id == UNUSED || player->PlayerModel == NULL)
        {
            stateModel[i] = NULL;
            continue;
        }

        CharacterAnimInput input;
        readInput(player, frames, &input);
        if (stateModel[i] != player->PlayerModel)
        {
            // Offset each player's idle so a team does not breathe in step.
            CharacterAnim_Reset(&states[i], &input, (uint32_t)(i * 7919 + player->Id));
            stateModel[i] = player->PlayerModel;
        }
        CharacterAnim_Update(&states[i], &input);
        CharacterAnim_Pose(&states[i], &rig, &poses[i]);
    }
}

void CharacterRig_Draw(const RigPose *pose, NE_Material *material, int x, int y, int z, int yaw,
                       int scaleX, int scaleY, int scaleZ)
{
    MATRIX_PUSH = 0;

    glTranslatef32(x, y, z);
    glRotateYi(yaw);
    MATRIX_SCALE = scaleX;
    MATRIX_SCALE = scaleY;
    MATRIX_SCALE = scaleZ;

    // Each bone: rotate about its pivot (roll, then pitch, then yaw), on top of its parent,
    // and store the result in its stack slot for the display list to restore. The root is
    // never stored; the pelvis builds on it directly.
    int current = RIG_ROOT;
    glTranslatef32(pose->offset[0], pose->offset[1], pose->offset[2]);
    for (int b = 0; b < RIG_BONE_COUNT; b++)
    {
        const RigBoneInfo *bone = &rig.bones[b];
        if (b > 0 && bone->parent != current)
            MATRIX_RESTORE = RIG_FIRST_SLOT + rig.bones[bone->parent].slot;

        const RigBonePose *p = &pose->bones[b];
        if (p->pitch != 0 || p->yaw != 0 || p->roll != 0)
        {
            glTranslatef32(bone->pivot[0], bone->pivot[1], bone->pivot[2]);
            if (p->yaw != 0)
                glRotateYi(p->yaw);
            if (p->pitch != 0)
                glRotateXi(p->pitch);
            if (p->roll != 0)
                glRotateZi(p->roll);
            glTranslatef32(-bone->pivot[0], -bone->pivot[1], -bone->pivot[2]);
        }
        if (bone->slot != RIG_NO_SLOT)
            MATRIX_STORE = RIG_FIRST_SLOT + bone->slot;
        current = b;
    }

    // If the texture pointer is NULL, this sets GFX_TEX_FORMAT to 0 and GFX_COLOR to white.
    NE_MaterialUse(material);
    glCallList(rig.displayList);

    MATRIX_POP = 1;
}

void CharacterAnim_DrawPlayer(int playerIndex)
{
    Player *player = &AllPlayers[playerIndex];
    NE_Model *model = player->PlayerModel;
    if (model == NULL)
        return;

    if (!rigReady || stateModel[playerIndex] != model)
    {
        NE_ModelDraw(model);
        return;
    }

    const CharacterAnimState *state = &states[playerIndex];
    CharacterRig_Draw(&poses[playerIndex], model->texture, model->x, model->y, model->z, state->renderYaw,
                      model->sx, model->sy, model->sz);
}
