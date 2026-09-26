// SPDX-License-Identifier: MIT
//
// Copyright (c) 2021-2022, Fewnity - Grégory Machefer
//
// This file is part of Counter Strike Nintendo DS Multiplayer Edition (CS:DS)

#include "main.h"
#include "grenade.h"
#include "collisions.h"
#include "ui.h"
#include "map.h"
#include "draw3d.h"
#include "party.h"
#include "player.h"
#include "character_anim.h"
#include "tombstone.h"
#include "death_view.h"
#include "lobby.h"

static bool visibleFromCamera(int zone, int x, int z);

// Where culling looks from (f32): the camera player's model, or the death view's camera.
static int cullX, cullZ;

int t1x = 0;
int t1z = 0;
int t2x = 0;
int t2z = 0;

//map UI
int MapImgToLoad;

//Currently loaded texture
int CurrentTexture = 1;

// Text material
NE_Material *TextMaterial = NULL;
// All palettes
NE_Palette *Palettes[19]; // 0 MapUI, 1 Map atlas, 2 text, 3 player, 4 gun sprite, 5 map point

// All materials used for the top screen
NE_Material *TopScreenSpritesMaterials[6];

// All materials used for the bottom screen
NE_Material *BottomScreenSpritesMaterials[9];

void initGraphics()
{
    // create Materials
    GroundMaterial = NE_MaterialCreate();
    GeneralMaterial = NE_MaterialCreate();
    PlayerMaterial = NE_MaterialCreate();
    PlayerMaterialTerrorist = NE_MaterialCreate();
    PlayerShadowMaterial = NE_MaterialCreate();
    TopScreenSpritesMaterials[0] = NE_MaterialCreate();
    TopScreenSpritesMaterials[2] = NE_MaterialCreate();
    TopScreenSpritesMaterials[3] = NE_MaterialCreate();
    TopScreenSpritesMaterials[4] = NE_MaterialCreate();
    TopScreenSpritesMaterials[5] = NE_MaterialCreate();

    BottomScreenSpritesMaterials[0] = NE_MaterialCreate();
    BottomScreenSpritesMaterials[2] = NE_MaterialCreate();
    BottomScreenSpritesMaterials[3] = NE_MaterialCreate();
    BottomScreenSpritesMaterials[4] = NE_MaterialCreate();
    BottomScreenSpritesMaterials[5] = NE_MaterialCreate();

    // Create palettes
    Palettes[0] = NE_PaletteCreate();
    Palettes[2] = NE_PaletteCreate();
    Palettes[1] = NE_PaletteCreate();
    Palettes[3] = NE_PaletteCreate();
    Palettes[5] = NE_PaletteCreate();
    Palettes[6] = NE_PaletteCreate();
    Palettes[7] = NE_PaletteCreate();
    Palettes[8] = NE_PaletteCreate();
    Palettes[9] = NE_PaletteCreate();
    Palettes[11] = NE_PaletteCreate();
    Palettes[12] = NE_PaletteCreate();
    Palettes[13] = NE_PaletteCreate();
    Palettes[14] = NE_PaletteCreate();
    Palettes[15] = NE_PaletteCreate();
    Palettes[16] = NE_PaletteCreate();
    Palettes[17] = NE_PaletteCreate();
    Palettes[18] = NE_PaletteCreate();

    // Load .bin textures

    // Load font
    TextMaterial = NE_MaterialCreate();
    NE_MaterialTexLoadBMPtoRGB256(TextMaterial, Palettes[2], (void *)text_bmp_bin, true); // Load bmp font format

    // Create font
    NE_TextInit(0,            // Font slot
                TextMaterial, // Image
                8, 8);        // Size of one character (x, y)

    NE_MaterialTexLoadBMPtoRGB256(PlayerMaterial, Palettes[3], (void *)tex_CtSkin_bin, 0);
    NE_MaterialTexLoadBMPtoRGB256(PlayerMaterialTerrorist, Palettes[13], (void *)tex_TSkin_bin, 0);

    NE_MaterialTexLoadBMPtoRGB256(GroundMaterial, Palettes[1], (void *)texMap_Dust2_bin, 1);
    // NE_MaterialTexClone(GroundMaterial, GeneralMaterial);
    NE_MaterialTexLoadBMPtoRGB256(GeneralMaterial, Palettes[18], (void *)tex_General_bin, 0);

    NE_MaterialTexLoadBMPtoRGB256(BottomScreenSpritesMaterials[0], Palettes[9], (void *)QuitButton_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(BottomScreenSpritesMaterials[2], Palettes[5], (void *)MapPointUI_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[5], Palettes[16], (void *)bomb_logo_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(BottomScreenSpritesMaterials[4], Palettes[7], (void *)CheckMark_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(BottomScreenSpritesMaterials[5], Palettes[6], (void *)WhiteScareRounded_bin, 1);

    // Load .bmp textures top screen
    NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[0], Palettes[8], (void *)crosshair2_bin, 1);

    NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[2], Palettes[11], (void *)muzzle_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[3], Palettes[12], (void *)scopeImage_bin, 1);
    NE_MaterialTexLoadBMPtoRGB256(PlayerShadowMaterial, Palettes[15], (void *)player_shadow_bin, 1);

    // Create crosshair sprite
    TopScreenSprites[0] = NE_SpriteCreate();
    NE_SpriteSetPos(TopScreenSprites[0], ScreenCenterWidth - 20 / 2, ScreenCenterHeight - 20 / 2);
    NE_SpriteSetSize(TopScreenSprites[0], 20, 20);
    NE_SpriteSetPriority(TopScreenSprites[0], 10);
    NE_SpriteSetMaterial(TopScreenSprites[0], TopScreenSpritesMaterials[0]);

    // Create map sprite
    TopScreenSprites[1] = NE_SpriteCreate();
    NE_SpriteSetSize(TopScreenSprites[1], 170, 177);
    NE_SpriteSetPriority(TopScreenSprites[1], 2);
    NE_SpriteSetMaterial(TopScreenSprites[1], TopScreenSpritesMaterials[4]);

    // Create quit button
    BottomScreenSprites[0] = NE_SpriteCreate();
    NE_SpriteSetSize(BottomScreenSprites[0], 20, 20);
    NE_SpriteSetPriority(BottomScreenSprites[0], 0);
    NE_SpriteSetMaterial(BottomScreenSprites[0], BottomScreenSpritesMaterials[0]);
    NE_SpriteSetPos(BottomScreenSprites[0], 236, 2);
    NE_SpriteVisible(BottomScreenSprites[0], false);

    // Create models
    for (int i = 7; i < 7 + 4; i++)
    {
        if (i == 8) // Model 8 is unused
            continue;
        
        // if (i == 9) // Model 9 is unused
        //     continue;

        Model[i] = NE_ModelCreate(NE_Static);
        NE_ModelSetMaterial(Model[i], GeneralMaterial);
        NE_ModelScaleI(Model[i], 4096, 4096, 4096);
        NE_ModelSetCoord(Model[i], 0, 1.5 + 0.8, 0);
        Model[i]->rx = 128;
        Model[i]->ry = 256;
    }

    // Load bomb model
    NE_ModelLoadStaticMesh(Model[7], (u32 *)bomb_bin);

    // Load flash models
    for (int i = 0; i < FLASH_MODELS_COUNT; i++)
    {
        flashModels[i] = NE_ModelCreate(NE_Static);

        if (i == 0)
            NE_ModelLoadStaticMesh(flashModels[i], (u32 *)plane_bin);
        else
            NE_ModelClone(flashModels[i],  // Destination
                          flashModels[0]); // Source model

        NE_ModelSetMaterial(flashModels[i], TopScreenSpritesMaterials[2]);
        NE_ModelScaleI(flashModels[i], 4096, 4096, 4096);
    }

    createPlayerShadow();

    // Load explision effect model
    NE_ModelLoadStaticMesh(Model[10], (u32 *)explosion_bin);
    Model[10]->rx = 0;
    Model[10]->ry = 256;
    NE_ModelScaleI(Model[10], 0, 0, 0);
    
}

// Create texture map sprite
void MapImgToLoadFunc()
{
	NE_PaletteDelete(Palettes[17]);
    NE_MaterialDelete(TopScreenSpritesMaterials[4]);
    TopScreenSpritesMaterials[4] = NE_MaterialCreate();
    Palettes[17] = NE_PaletteCreate();

    if(MapImgToLoad == 0)
	{
		NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[4], Palettes[17], (void *)MapUI_Dust2_bin, 0);
	}
    else if(MapImgToLoad == 6)
	{
		NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[4], Palettes[17], (void *)MapUI_Mirage_bin, 0);
	}
	else
	{
		NE_MaterialTexLoadBMPtoRGB256(TopScreenSpritesMaterials[4], Palettes[17], (void *)MapUI_General_bin, 0);
	}

    // Reuse the minimap sprite created by initGraphics().
	NE_SpriteSetSize(TopScreenSprites[1], 170, 177);
	NE_SpriteSetPriority(TopScreenSprites[1], 2);
	NE_SpriteSetMaterial(TopScreenSprites[1], TopScreenSpritesMaterials[4]);
}

/**
 * @brief Create shadow for each players
 *
 */
void createPlayerShadow()
{
    // Player shadow
    for (int i = 1; i < MaxPlayer; i++)
    {
        AllPlayers[i].PlayerShadow = NE_ModelCreate(NE_Static);
        NE_ModelScaleI(AllPlayers[i].PlayerShadow, 4096 * 2, 4096 * 2, 4096 * 2);
        NE_ModelSetMaterial(AllPlayers[i].PlayerShadow, PlayerShadowMaterial);
        NE_ModelLoadStaticMesh(AllPlayers[i].PlayerShadow, (u32 *)plane_bin);
        NE_ModelSetCoord(AllPlayers[i].PlayerShadow, 4, -0.845, -13);
    }
}

/**
 * @brief Enanble or disable two screens refresh mode
 *
 * @param value
 */
void SetTwoScreenMode(bool value)
{
    if (AlwaysUpdateBottomScreen && !value && UpdateBottomScreenFrameCount == 0)
    {
        NeedChangeScreen = true;
    }
    else if (!AlwaysUpdateBottomScreen && value)
    {
        NeedChangeScreen = true;
    }
}

/**
 * @brief Draw top screen 3D then UI
 *
 */
void Draw3DScene(void)
{
    // Quitting a match deletes every player mid-frame (see GameLoop), and that frame is
    // still rendered. Everything below culls against the camera player's model, so draw
    // nothing for it; the menu scene takes over on the next frame.
    if (AllPlayers[CurrentCameraPlayer].PlayerModel == NULL)
        return;

    // Set camera for drawing
    NE_CameraUse(Camera);

    // Pose every character for this frame, visible or not, so each stays in step, and mark
    // where anyone has just died.
    CharacterAnim_UpdatePlayers();
    Tombstone_Update();

    // Reset polygons Alpha/Light/Effect
    NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);

    Map *map = &allMaps[currentMap];

    // render 3D if map insn't at screen
    if (!isShowingMap)
    {
        // Culling looks from the camera player, unless the local player is watching its own
        // death from outside.
        cullX = AllPlayers[CurrentCameraPlayer].PlayerModel->x;
        cullZ = AllPlayers[CurrentCameraPlayer].PlayerModel->z;
        DeathView_CullOrigin(&cullX, &cullZ);

        // Field of view end coordinates
        t1x = (xWithoutYForOcclusionSide1 * 500 + cullX / 4096.0) * 8192.0;
        t1z = (zWithoutYForOcclusionSide1 * 500 + cullZ / 4096.0) * 8192.0;
        t2x = (xWithoutYForOcclusionSide2 * 500 + cullX / 4096.0) * 8192.0;
        t2z = (zWithoutYForOcclusionSide2 * 500 + cullZ / 4096.0) * 8192.0;

        // Draw map
        for (int i = 0; i < map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].ZoneCount; i++)
        {
            bool inFov = false; // Is the map part in the field of view of the player?

            // Force to render the map part where the player is
            if (checkZoneForOcclusion(&map->AllOcclusionZone[map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].visibleMapPart[i]], cullX, cullZ))
                inFov = true;
            else
            {
                // Check if the map part is in the field of view of the player
                for (int i2 = 0; i2 < 4; i2++)
                {
                    if (PointInTriangleInt(map->AllOcclusionZone[map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].visibleMapPart[i]].anglesInt[i2].x, map->AllOcclusionZone[map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].visibleMapPart[i]].anglesInt[i2].y, cullX, cullZ, t1x, t1z, t2x, t2z))
                    {
                        inFov = true;
                        break;
                    }
                }
            }

            // Render map model if needed
            if (inFov)
            {
                GroundMaterial->diffuse = RGB15(0, 0, 0);
                GroundMaterial->emission = RGB15(11, 11, 11);
                GroundMaterial->specular = RGB15(7, 7, 7);

                // if (!map->models[map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].visibleMapPart[i]].shadowed) // Set the model light like normal
                // {
                //     GroundMaterial->diffuse = RGB15(0, 0, 0);
                //     GroundMaterial->emission = RGB15(11, 11, 11);
                //     GroundMaterial->specular = RGB15(7, 7, 7);
                // }
                // else // Set the model light like shadowed
                // {
                //     GroundMaterial->diffuse = RGB15(1, 1, 1);
                //     GroundMaterial->emission = RGB15(3, 3, 3);
                //     GroundMaterial->specular = RGB15(3, 3, 3);
                // }
                NE_ModelDraw(map->models[map->AllZones[AllPlayers[CurrentCameraPlayer].CurrentOcclusionZone].visibleMapPart[i]].Model);
            }
        }

        NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);

        // Draw grenades and their effects
        for (int i = 0; i < GrenadeCount; i++)
        {
            if (grenades[i] != NULL)
            {
                //  Draw grenade
                if (grenades[i]->isVisible)
                {
                    // Grenade clipping
                    bool inFov = PointInTriangleInt(grenades[i]->Model->x, grenades[i]->Model->z, AllPlayers[CurrentCameraPlayer].PlayerModel->x, AllPlayers[CurrentCameraPlayer].PlayerModel->z, t1x, t1z, t2x, t2z);

                    // Draw grenade if in field of view
                    if (inFov)
                    {
                        NE_PolyFormat(30, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
                        NE_ModelDraw(grenades[i]->Model);
                    }
                }

                // Alpha 0 is wireframe mode, put to 1 to see the grenade
                if (grenades[i]->effectAlpha <= 0)
                    grenades[i]->effectAlpha = 1;

                // Set polygons alpha
                NE_PolyFormat(grenades[i]->effectAlpha, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
                // Draw effect if timer effect is on
                if (grenades[i]->EffectTimer != 0)
                {
                    if ((grenades[i]->GrenadeType == SMOKE && isInFullSmoke) || grenades[i]->GrenadeType == FLASH)
                        continue;
                    NE_ModelDraw(grenades[i]->EffectModel);
                }
            }
        }
        isInFullSmoke = false;

        // Reset polygons Alpha/Light/Effect
        NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);

        // If bomb is planted, draw bomb
        if (BombPlanted || bombDropped)
            NE_ModelDraw(Model[7]);

        // Show wall bullet hit flash
        for (int i = 0; i < FLASH_MODELS_COUNT; i++)
        {
            if (ShowWallHitFlash[i] != 0)
            {
                ShowWallHitFlash[i]--;
                NE_ModelDraw(flashModels[i]);
            }
        }

        // Draw bomb explosion
        if (IsExplode)
        {
            // Calculate explosion effect alpha
            int ExplosionAlpha = 31 - (int)(BombExplosionScale / 1.2);
            if (ExplosionAlpha < 0)
            {
                ExplosionAlpha = 0;
                IsExplode = false;
            }
            // Set polygons alpha
            NE_PolyFormat(ExplosionAlpha, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);

            // Draw explosion
            NE_ModelDraw(Model[10]);
        }

        Tombstone_DrawAll(visibleFromCamera);
        DrawPlayers();
    }

    // Draw UI
    drawTopScreenUI();
}

/**
 * @brief Whether something in map zone `zone` at (x, z) (f32) is worth drawing: the camera
 * stands in a map part visible from that zone, and the point is in the view wedge
 *
 */
static bool visibleFromCamera(int zone, int x, int z)
{
    Map *map = &allMaps[currentMap];
    for (int i = 0; i < map->AllZones[zone].ZoneCount; i++)
    {
        if (checkZoneForOcclusion(&map->AllOcclusionZone[map->AllZones[zone].visibleMapPart[i]], cullX, cullZ))
            return PointInTriangleInt(x, z, cullX, cullZ, t1x, t1z, t2x, t2z);
    }
    return false;
}

/**
 * @brief Draws players
 *
 */
void DrawPlayers()
{
    // Upstream skipped every player whenever the bottom screen was refreshing, which is
    // any frame in a NE_ProcessDual batch. startChangeMenu() queues 8 of those, so every
    // menu interaction -- and every kill and network event, which also queue 8 -- made
    // the players vanish for about an eighth of a second.

    // for each players. The local player's body is seen only once it is dead: in its own
    // death view, or from the eyes of whoever it spectates. Fading bodies are translucent, and
    // the hardware draws translucent polygons in the order they arrive, without depth among
    // themselves: they go after the rest, farthest first.
    bool deathView = DeathView_Active();
    bool localBody = deathView || (localPlayer->IsDead && CurrentCameraPlayer != 0);
    int fading[MaxPlayer], fadingCount = 0;
    int64_t fadingDistance[MaxPlayer];
    for (int playerIndex = localBody ? 0 : 1; playerIndex < MaxPlayer; playerIndex++)
    {
        Player *player = &AllPlayers[playerIndex];
        // Check if he is in game and if the camera is not on this player. The dead fall,
        // then fade out over a tombstone; that needs the animated rig.
        bool drawnDead = AllPlayers[playerIndex].IsDead && CharacterAnim_Ready();
        bool looking = CurrentCameraPlayer != playerIndex || (playerIndex == 0 && deathView);
        if (AllPlayers[playerIndex].Id == UNUSED || (AllPlayers[playerIndex].IsDead && !drawnDead) ||
            AllPlayers[playerIndex].PlayerModel == NULL || !looking ||
            !visibleFromCamera(player->CurrentOcclusionZone, player->PlayerModel->x, player->PlayerModel->z))
            continue;

        // A body that has faded away is not drawn at all (alpha 0 would draw it as wireframe).
        int alpha = drawnDead ? CharacterAnim_BodyAlpha(playerIndex) : 31;
        if (alpha == 0)
            continue;
        if (alpha < 31)
        {
            int64_t dx = player->PlayerModel->x - cullX, dz = player->PlayerModel->z - cullZ;
            int n = fadingCount++;
            // Insertion sort, farthest first.
            while (n > 0 && fadingDistance[n - 1] < dx * dx + dz * dz)
            {
                fading[n] = fading[n - 1];
                fadingDistance[n] = fadingDistance[n - 1];
                n--;
            }
            fading[n] = playerIndex;
            fadingDistance[n] = dx * dx + dz * dz;
            continue;
        }

        NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
        // Draw player's skin
        CharacterAnim_DrawPlayer(playerIndex);

        // Draw player's shadow
        if (!drawnDead && (player->isAi || fabs(player->position.y - player->lerpDestination.y) < 0.05))
        {
            NE_PolyFormat(15, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
            NE_ModelDraw(AllPlayers[playerIndex].PlayerShadow);
        }
    }

    // Each fading body has a polygon ID of its own: a translucent pixel is not drawn over
    // another left by the same ID.
    for (int i = 0; i < fadingCount; i++)
    {
        int playerIndex = fading[i];
        NE_PolyFormat(CharacterAnim_BodyAlpha(playerIndex), BODY_FADE_POLY_ID + playerIndex, NE_LIGHT_0, NE_CULL_BACK,
                      NE_MODULATION);
        CharacterAnim_DrawPlayer(playerIndex);
    }
}

/**
 * @brief NE_2DViewInit, but with every screen row where it belongs
 *
 * NE_2DViewInit builds its projection with glOrthof32, whose fixed-point division truncates
 * 2/192 to 42/4096 where it should be 42.67/4096. Every 2D y coordinate is drawn at 63/64
 * of its value: a quad to y=192 stops at row 189, and a 192-row texture loses three rows.
 * The existing menus were laid out around that (hence ScreenHeightFixed), so this is a
 * separate view for artwork that has to land on exact pixels.
 *
 * The DS truncates when it maps clip space to screen rows, so y's factor is rounded away
 * from zero: every integer coordinate then lands just past its row boundary, never short.
 * z keeps NE_2DViewInit's meaning, so priorities work the same in both views.
 */
void Init2DViewPixelExact(void)
{
    NE_2DViewInit();

    MATRIX_CONTROL = GL_PROJECTION;
    MATRIX_LOAD4x4 = 131072; // x: 0..256 -> -1..1; 2/256 * 4096 * 4096, exact
    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = 0;

    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = -174763; // y: 0..192 -> 1..-1; 2/192 * 4096 * 4096 = 174762.67
    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = 0;

    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = inttof32(1);
    MATRIX_LOAD4x4 = 0;

    MATRIX_LOAD4x4 = inttof32(-1);
    MATRIX_LOAD4x4 = inttof32(1);
    MATRIX_LOAD4x4 = 0;
    MATRIX_LOAD4x4 = inttof32(1);

    // Vertices go straight to the projection: no modelview scale.
    MATRIX_CONTROL = GL_MODELVIEW;
    MATRIX_IDENTITY = 0;
}

/**
 * @brief Draw game top screen when a game is not launched
 *
 */
void Draw3DSceneNotInGame(void)
{
    // The main menu's artwork covers the whole top screen: skip the 3D scene under it. The
    // lobby draws its own scene.
    if (drawMainMenuTopScreen() || drawLobbyTopScreen())
        return;

    // Set camera for drawing
    NE_CameraUse(Camera);

    // Reset polygons Alpha/Light/Effect
    NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);

    Map *map = &allMaps[currentMap];

    // Draw map
    if (currentMap == DUST2)
    {
        GroundMaterial->diffuse = RGB15(0, 0, 0);
        GroundMaterial->emission = RGB15(14, 14, 14);
        GroundMaterial->specular = RGB15(10, 10, 10);
        NE_ModelDraw(map->models[2].Model);
        NE_ModelDraw(map->models[3].Model);

        // if (!map->models[2].shadowed)
        // {
        //         GroundMaterial->diffuse = RGB15(0, 0, 0);
        //         GroundMaterial->emission = RGB15(14, 14, 14);
        //         GroundMaterial->specular = RGB15(10, 10, 10);
        //         NE_ModelDraw(map->models[2].Model);
        // }

        // if (!map->models[3].shadowed)
        // {
        //         GroundMaterial->diffuse = RGB15(0, 0, 0);
        //         GroundMaterial->emission = RGB15(14, 14, 14);
        //         GroundMaterial->specular = RGB15(10, 10, 10);
        //         NE_ModelDraw(map->models[3].Model);
        // }
    }
    else if (currentMap == TUTORIAL)
    {
        GroundMaterial->diffuse = RGB15(0, 0, 0);
        GroundMaterial->emission = RGB15(14, 14, 14);
        GroundMaterial->specular = RGB15(10, 10, 10);
        NE_ModelDraw(map->models[0].Model);

        // for (int i = 0; i < 2; i++)
        // {
        //     // Set the model light like normal
        //     if (!map->models[i].shadowed)
        //     {
        //         GroundMaterial->diffuse = RGB15(0, 0, 0);
        //         GroundMaterial->emission = RGB15(14, 14, 14);
        //         GroundMaterial->specular = RGB15(10, 10, 10);
        //     }
        //     else // Set the model light like shadowed
        //     {
        //         GroundMaterial->diffuse = RGB15(4, 4, 4);
        //         GroundMaterial->emission = RGB15(0, 0, 0);
        //         GroundMaterial->specular = RGB15(1, 1, 1);
        //     }
        //     NE_ModelDraw(map->models[i].Model);
        // }
    }
    else if (currentMap == DUST2_2x2)
    {
        GroundMaterial->diffuse = RGB15(0, 0, 0);
        GroundMaterial->emission = RGB15(14, 14, 14);
        GroundMaterial->specular = RGB15(10, 10, 10);
        NE_ModelDraw(map->models[0].Model);
        NE_ModelDraw(map->models[2].Model);
    }
    else if (currentMap == AIM_MAP)
    {
        for (int i = 0; i < 3; i++)
        {
            GroundMaterial->diffuse = RGB15(0, 0, 0);
            GroundMaterial->emission = RGB15(14, 14, 14);
            GroundMaterial->specular = RGB15(10, 10, 10);
            NE_ModelDraw(map->models[i].Model);
        }
    }
    else if (currentMap == B2000)
    {
        for (int i = 0; i < 2; i++)
        {
            GroundMaterial->diffuse = RGB15(0, 0, 0);
            GroundMaterial->emission = RGB15(14, 14, 14);
            GroundMaterial->specular = RGB15(10, 10, 10);
            NE_ModelDraw(map->models[i].Model);
        }
    }
    else if (currentMap == FYSNOW)
    {
        for (int i = 0; i < 2; i++)
        {
            GroundMaterial->diffuse = RGB15(0, 0, 0);
            GroundMaterial->emission = RGB15(14, 14, 14);
            GroundMaterial->specular = RGB15(10, 10, 10);
            NE_ModelDraw(map->models[i].Model);
        }
    }
    else if (currentMap == MIRAGE)
    {
        GroundMaterial->diffuse = RGB15(0, 0, 0);
        GroundMaterial->emission = RGB15(14, 14, 14);
        GroundMaterial->specular = RGB15(10, 10, 10);
        NE_ModelDraw(map->models[0].Model);
        NE_ModelDraw(map->models[1].Model);
    }

    NE_2DViewInit();
    // Draw keyboard input screen if needed
    if (isShowingKeyBoard)
    {
        drawKeyboardInput();
    }
}
