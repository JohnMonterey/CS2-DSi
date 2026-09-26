// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// The singleplayer lobby: your character on the top screen, the match setup below.
//
// It replaces the map, mode, bot and team screens with one: pick a mode, a map, a lineup
// and a side, then GO. The top screen is a small studio: a lit floor and backdrop drawn as
// vertex-coloured polygons (no texture memory), the animated player rig from
// character_anim.c walking in and standing there, and the player's name plate.
//
// Everything animates off one frame counter advanced by Lobby_Update() at 60 Hz. Menus
// render the two screens on alternate frames, so each draw only samples that state.

#include "lobby.h"
#include "main.h"
#include "ui.h"
#include "draw3d.h"
#include "party.h"
#include "map.h"
#include "ai.h"
#include "input.h"
#include "font.h"
#include "character_anim.h"
#include "font_cs20_bin.h"
#include "font_cs12_bin.h"

int lobbyPendingTeam = SPECTATOR;

// ----------------------------------------------------------------------------------------
// Choices
// ----------------------------------------------------------------------------------------

typedef struct
{
    int partyMode; // allPartyModes index, as StartSinglePlayer takes it
    const char *name;
} LobbyMode;

static const LobbyMode lobbyModes[] = {
    {1, "Casual"},
    {0, "Competitive"},
    {3, "Deathmatch"},
    {4, "Gun Game"},
};
#define LOBBY_MODE_COUNT ((int)(sizeof(lobbyModes) / sizeof(lobbyModes[0])))
#define TRAINING_MODE 2 // what the training map forces; it adds no bots (main.c)

// amountOfBots counts every slot, the player's included. Without equal teams the player's
// side gives one member to the other (checkStartGameLoop), hence the odd lineups.
typedef struct
{
    int slots;
    bool equal;
    const char *name;
} LobbyLineup;

static const LobbyLineup lobbyLineups[] = {
    {4, false, "1 vs 3"}, {4, true, "2 vs 2"}, {6, false, "2 vs 4"},
    {6, true, "3 vs 3"}, {10, false, "4 vs 6"}, {10, true, "5 vs 5"},
};
#define LOBBY_LINEUP_COUNT ((int)(sizeof(lobbyLineups) / sizeof(lobbyLineups[0])))

enum LobbyRow
{
    ROW_MODE,
    ROW_MAP,
    ROW_BOTS,
    ROW_TEAM,
    ROW_GO, // the GO button, for the d-pad
    ROW_COUNT
};
#define SETTING_ROWS ROW_GO

static const char *const rowLabels[SETTING_ROWS] = {"MODE", "MAP", "BOTS", "TEAM"};

// Kept for the next visit (not saved).
static int chosenMode = 0;
static int chosenLineup = -1; // taken from amountOfBots and equalTeam on the first visit
static int chosenTeam = COUNTERTERRORISTS;

// ----------------------------------------------------------------------------------------
// Layout (bottom screen, pixels) and timing (frames at 60 Hz)
// ----------------------------------------------------------------------------------------

#define HEADER_H 24
#define ROW_Y0 31
#define ROW_STEP 28
#define ROW_H 24
#define ROW_X1 6
#define ROW_X2 250
#define VALUE_X1 70 // the value area, arrows included
#define VALUE_X2 250
#define ARROW_ZONE 30 // touch width of each arrow
#define TEAM_CT_X1 80
#define TEAM_T_X1 166
#define TEAM_SEG_W 78
#define GO_X1 142
#define GO_Y1 146
#define GO_X2 250
#define GO_Y2 184

#define FADE_IN_FRAMES 16
#define ROW_ENTER_DELAY 6
#define ROW_ENTER_STAGGER 4
#define ROW_ENTER_FRAMES 18
#define SLIDE_FRAMES 12
#define PRESS_FRAMES 10
#define SPIN_FRAMES 34
#define TINT_FRAMES 26
#define GO_FRAMES 44
#define GO_RAISE_FRAMES 16
#define GO_FADE_START 16
#define GO_FADE_FRAMES 22

// Colours
#define COLOR_TEXT RGB15(29, 29, 28)
#define COLOR_TEXT_DIM RGB15(16, 17, 19)
#define COLOR_TEXT_OFF RGB15(9, 10, 12)
#define COLOR_PANEL RGB15(4, 5, 7)
#define COLOR_PANEL_EDGE RGB15(7, 8, 11)
#define COLOR_ACCENT RGB15(31, 22, 6)
#define COLOR_CT RGB15(12, 19, 30)
#define COLOR_T RGB15(28, 22, 9)

// Top screen studio, Q12 world units. The character stands at the origin, facing the camera
// from the right third of the frame; the left is for its name plate.
#define CHAR_Y 3031       // feet on y = 0: the rig's soles are 1.45 model units down, at 0.51 scale
#define WALK_FROM (-10240) // -2.5: starts off screen, left
#define WALK_FRAMES 76
#define TURN_START 58
#define TURN_FRAMES 26
#define FACE_ANGLE ANIM_DEG(-24) // mostly toward the camera, a little toward the name plate

// ----------------------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------------------

static struct
{
    bool open;
    bool characterPlaced; // the walk-in has been set up for this visit
    Font big, small;
    NE_Camera *camera;
    CharacterAnimState anim;
    RigPose pose;
    uint32_t time;                        // frames since the lobby opened
    int map;                              // allMaps index
    int focus;                            // enum LobbyRow
    int32_t focusY;                       // Q8 px, eases toward the focused row
    int32_t teamPill;                     // Q8 px, left edge of the team highlight
    int slideDir[SETTING_ROWS];           // -1 or +1: which way the last change went
    int slideTime[SETTING_ROWS];          // frames since that change
    const char *slideFrom[SETTING_ROWS];  // the value shown before it
    int pressTime[SETTING_ROWS][2];       // frames since the left/right control was pressed
    int spinTime;                         // frames into a team-change spin; -1 when still
    int skinTeam;                         // skin on screen; swaps halfway through a spin
    int tintFrom;                         // backdrop team colour being faded from
    int tintTime;                         // frames since the side changed
    int goTime;                           // frames since GO, -1 before
} lobby;

static int wrapIndex(int i, int count)
{
    return (i % count + count) % count;
}

static int32_t progress(int frames, int length)
{
    return AnimClamp(frames * ANIM_ONE / length, 0, ANIM_ONE);
}

static int mix(int a, int b, int32_t t)
{
    return a + (int)(((int64_t)(b - a) * t) >> 12);
}

static bool modeLocked(void)
{
    return allMaps[lobby.map].forcePartyMode != -1;
}

static int effectiveMode(void)
{
    return modeLocked() ? allMaps[lobby.map].forcePartyMode : lobbyModes[chosenMode].partyMode;
}

static bool botsLocked(void)
{
    return effectiveMode() == TRAINING_MODE;
}

static const char *rowValue(int row)
{
    switch (row)
    {
    case ROW_MODE:
        return modeLocked() ? (effectiveMode() == TRAINING_MODE ? "Training" : "Fixed") : lobbyModes[chosenMode].name;
    case ROW_MAP:
        return allMaps[lobby.map].name;
    case ROW_BOTS:
        return botsLocked() ? "No bots" : lobbyLineups[chosenLineup].name;
    default:
        return "";
    }
}

static bool rowLocked(int row)
{
    return (row == ROW_MODE && modeLocked()) || (row == ROW_BOTS && botsLocked());
}

static int rowY(int row)
{
    return ROW_Y0 + row * ROW_STEP;
}

// ----------------------------------------------------------------------------------------
// Changing things
// ----------------------------------------------------------------------------------------

static void changeRow(int row, int dir)
{
    if (row >= SETTING_ROWS || row == ROW_TEAM || rowLocked(row))
        return;

    const char *before[SETTING_ROWS];
    for (int r = 0; r < SETTING_ROWS; r++)
        before[r] = rowValue(r);

    // The map and the lineup go straight into the game's own settings, as the screens this
    // replaces did, so they are there on the next visit and for anything else that reads them.
    if (row == ROW_MODE)
        chosenMode = wrapIndex(chosenMode + dir, LOBBY_MODE_COUNT);
    else if (row == ROW_MAP)
        lobby.map = currentSelectionMap = wrapIndex(lobby.map + dir, MAP_COUNT);
    else if (row == ROW_BOTS)
    {
        chosenLineup = wrapIndex(chosenLineup + dir, LOBBY_LINEUP_COUNT);
        amountOfBots = lobbyLineups[chosenLineup].slots;
        equalTeam = lobbyLineups[chosenLineup].equal;
    }

    // A map can force a mode, and so lock the rows below it: slide whatever changed.
    for (int r = 0; r < SETTING_ROWS; r++)
    {
        if (rowValue(r) != before[r])
        {
            lobby.slideFrom[r] = before[r];
            lobby.slideDir[r] = dir;
            lobby.slideTime[r] = 0;
        }
    }
    lobby.pressTime[row][dir > 0] = 0;
}

static void setTeam(int team)
{
    lobby.pressTime[ROW_TEAM][team == TERRORISTS] = 0;
    if (team == chosenTeam)
        return;
    lobby.tintFrom = chosenTeam;
    lobby.tintTime = 0;
    chosenTeam = team;
    // The character turns right round; the skin changes while its back is to the camera.
    lobby.spinTime = 0;
}

static void pressGo(void)
{
    if (lobby.goTime >= 0)
        return;
    lobby.goTime = 0;
    // Nothing to go back to once the match is on its way; the X would also sit alone on
    // the black screen.
    setQuitButton(false);
}

// What the match start needs, set the way the old map and mode screens set it. The match
// itself starts on the next pass of checkStartGameLoop(), which closes this menu first.
static void startMatch(void)
{
    amountOfBots = lobbyLineups[chosenLineup].slots;
    equalTeam = lobbyLineups[chosenLineup].equal;
    currentSelectionMap = lobby.map;
    MapImgToLoad = lobby.map;
    lobbyPendingTeam = chosenTeam;
    StartSinglePlayer(effectiveMode());
}

// ----------------------------------------------------------------------------------------
// Menu lifecycle
// ----------------------------------------------------------------------------------------

static void drawLobbyMenu();

static void unloadLobbyMenu()
{
    Font_Unload(&lobby.big);
    Font_Unload(&lobby.small);
    if (lobby.camera != NULL)
        NE_CameraDelete(lobby.camera);
    lobby.camera = NULL;
    lobby.open = false;
}

void initLobbyMenu()
{
    SetTwoScreenMode(true); // a match started from here must begin in single-screen mode

    startChangeMenu(LOBBY);

    renderFunction = &drawLobbyMenu;
    lastOpenedMenu = &initMainMenu;
    onCloseMenu = &unloadLobbyMenu;
    haveToCallOnCloseMenu = true;
    setQuitButton(true);
    SetMenuDrawsOwnScreen(true);

    // Input is read by Lobby_Update(), not through AllButtons.
    SetButtonToShow(0);

    if (chosenLineup < 0)
    {
        chosenLineup = 0;
        for (int i = 0; i < LOBBY_LINEUP_COUNT; i++)
            if (lobbyLineups[i].slots == amountOfBots && lobbyLineups[i].equal == equalTeam)
                chosenLineup = i;
        for (int i = 0; i < LOBBY_MODE_COUNT; i++)
            if (lobbyModes[i].partyMode == currentPartyMode)
                chosenMode = i;
    }

    // A few KB of texture VRAM; both fall back to nothing drawn if VRAM is full.
    Font_Load(&lobby.big, font_cs20_bin, font_cs20_bin_size);
    Font_Load(&lobby.small, font_cs12_bin, font_cs12_bin_size);
    lobby.camera = NE_CameraCreate();

    lobby.open = true;
    lobby.characterPlaced = false;
    lobby.time = 0;
    lobby.map = wrapIndex(currentSelectionMap, MAP_COUNT);
    lobby.focus = ROW_GO;
    lobby.focusY = rowY(ROW_GO) << 8;
    lobby.teamPill = (chosenTeam == COUNTERTERRORISTS ? TEAM_CT_X1 : TEAM_T_X1) << 8;
    for (int r = 0; r < SETTING_ROWS; r++)
    {
        lobby.slideTime[r] = SLIDE_FRAMES;
        lobby.slideFrom[r] = NULL;
        lobby.slideDir[r] = 1;
        lobby.pressTime[r][0] = lobby.pressTime[r][1] = PRESS_FRAMES;
    }
    lobby.spinTime = -1;
    lobby.skinTeam = chosenTeam;
    lobby.tintFrom = chosenTeam;
    lobby.tintTime = TINT_FRAMES;
    lobby.goTime = -1;
    lobbyPendingTeam = SPECTATOR;
}

// ----------------------------------------------------------------------------------------
// Per frame
// ----------------------------------------------------------------------------------------

static void updateCharacter(void)
{
    const CharacterRig *rig = CharacterAnim_Rig();
    if (rig == NULL)
        return;

    // Walks in from the left and stops on its mark (smoothstep: speeds up off screen, slows
    // to a stop in view), then turns to the camera. The gait follows the ground covered.
    int32_t walk = AnimEaseInOut(progress(lobby.time, WALK_FRAMES));
    int32_t walkYaw = -ANIM_TURN / 4; // facing +x
    int32_t faceYaw = ANIM_TURN / 2 + FACE_ANGLE;
    int32_t turn = AnimEaseInOut(progress((int)lobby.time - TURN_START, TURN_FRAMES));

    CharacterAnimInput input;
    memset(&input, 0, sizeof(input));
    input.x = mix(WALK_FROM, 0, walk);
    input.y = CHAR_Y;
    input.z = 0;
    input.yaw = walkYaw + (int32_t)(((int64_t)AnimAngleDelta(walkYaw, faceYaw) * turn) >> 12);
    input.frames = 1;
    if (!lobby.characterPlaced)
    {
        // Every visit starts on the walk-in's first frame; without this the character
        // would stride over from wherever the last visit left it.
        CharacterAnim_Reset(&lobby.anim, &input, 11);
        lobby.characterPlaced = true;
    }
    CharacterAnim_Update(&lobby.anim, &input);
    CharacterAnim_Pose(&lobby.anim, rig, &lobby.pose);

    // Pistol held low, raised to the aim when the match is about to start.
    int32_t lowered = ANIM_ONE;
    if (lobby.goTime >= 0)
        lowered = ANIM_ONE - AnimEaseInOut(progress(lobby.goTime, GO_RAISE_FRAMES));
    CharacterAnim_LowerWeapon(&lobby.pose, lowered);
}

static void handleTouch(int x, int y)
{
    if (x >= GO_X1 && x <= GO_X2 && y >= GO_Y1 && y <= GO_Y2)
    {
        lobby.focus = ROW_GO;
        pressGo();
        return;
    }
    for (int row = 0; row < SETTING_ROWS; row++)
    {
        if (y < rowY(row) - 2 || y > rowY(row) + ROW_H + 2)
            continue;
        lobby.focus = row;
        if (row == ROW_TEAM)
        {
            if (x >= TEAM_CT_X1 - 4 && x < TEAM_T_X1 - 2)
                setTeam(COUNTERTERRORISTS);
            else if (x >= TEAM_T_X1 - 2)
                setTeam(TERRORISTS);
        }
        else if (x >= VALUE_X1 && x < VALUE_X1 + ARROW_ZONE)
            changeRow(row, -1);
        else if (x >= VALUE_X1 + ARROW_ZONE)
            changeRow(row, 1); // the right arrow, or the value itself
        return;
    }
}

void Lobby_Update(void)
{
    if (!lobby.open)
        return;

    lobby.time++;
    for (int r = 0; r < SETTING_ROWS; r++)
    {
        if (lobby.slideTime[r] < SLIDE_FRAMES)
            lobby.slideTime[r]++;
        for (int k = 0; k < 2; k++)
            if (lobby.pressTime[r][k] < PRESS_FRAMES)
                lobby.pressTime[r][k]++;
    }
    if (lobby.tintTime < TINT_FRAMES)
        lobby.tintTime++;
    if (lobby.spinTime >= 0)
    {
        lobby.spinTime++;
        if (lobby.spinTime >= SPIN_FRAMES / 2)
            lobby.skinTeam = chosenTeam;
        if (lobby.spinTime >= SPIN_FRAMES)
            lobby.spinTime = -1;
    }

    if (lobby.goTime >= 0)
    {
        // Starting: no more input. The screens fade to black, and the match loads under
        // the last (black) frame.
        lobby.goTime++;
        if (lobby.goTime == GO_FRAMES)
            startMatch();
    }
    else if (lobby.time > ROW_ENTER_DELAY)
    {
        uint32 down = keysdown;
        if (down & KEY_B)
        {
            initMainMenu(); // closes the lobby
            return;
        }
        if (down & KEY_UP)
            lobby.focus = wrapIndex(lobby.focus - 1, ROW_COUNT);
        if (down & KEY_DOWN)
            lobby.focus = wrapIndex(lobby.focus + 1, ROW_COUNT);
        if (down & (KEY_LEFT | KEY_RIGHT))
        {
            int dir = (down & KEY_LEFT) ? -1 : 1;
            if (lobby.focus == ROW_TEAM)
                setTeam(dir < 0 ? COUNTERTERRORISTS : TERRORISTS);
            else
                changeRow(lobby.focus, dir);
        }
        if (down & KEY_A)
        {
            if (lobby.focus == ROW_GO)
                pressGo();
            else if (lobby.focus == ROW_TEAM)
                setTeam(chosenTeam == COUNTERTERRORISTS ? TERRORISTS : COUNTERTERRORISTS);
            else
                changeRow(lobby.focus, 1);
        }
        if (down & KEY_START)
        {
            lobby.focus = ROW_GO;
            pressGo();
        }
        if (down & KEY_TOUCH)
            handleTouch(touch.px, touch.py);
    }

    // Highlights glide to where they belong.
    lobby.focusY = AnimApproach(lobby.focusY, rowY(lobby.focus) << 8, 1229);
    int pillTarget = (chosenTeam == COUNTERTERRORISTS ? TEAM_CT_X1 : TEAM_T_X1) << 8;
    lobby.teamPill = AnimApproach(lobby.teamPill, pillTarget, 1024);

    updateCharacter();
}

// ----------------------------------------------------------------------------------------
// Drawing helpers
// ----------------------------------------------------------------------------------------

static u32 color15(int r, int g, int b)
{
    return RGB15(AnimClamp(r, 0, 31), AnimClamp(g, 0, 31), AnimClamp(b, 0, 31));
}

// Alpha 0 is wireframe on the DS, so a fully faded polygon is not drawn at all.
static bool polyAlpha(int alpha, int id)
{
    if (alpha <= 0)
        return false;
    NE_PolyFormat(alpha > 31 ? 31 : alpha, id, 0, NE_CULL_NONE, 0);
    return true;
}

static void quad2D(int x1, int y1, int x2, int y2, int z, u32 color)
{
    NE_2DDrawQuad(x1, y1, x2, y2, z, color);
}

static void triangle2D(int x1, int y1, int x2, int y2, int x3, int y3, int z, u32 color)
{
    GFX_BEGIN = GL_TRIANGLES;
    GFX_TEX_FORMAT = 0;
    GFX_COLOR = color;
    GFX_VERTEX16 = (y1 << 16) | (x1 & 0xFFFF);
    GFX_VERTEX16 = z;
    GFX_VERTEX_XY = (y2 << 16) | (x2 & 0xFFFF);
    GFX_VERTEX_XY = (y3 << 16) | (x3 & 0xFFFF);
}

static void textCentered(const Font *font, int cx, int y, int z, u32 color, int alpha, const char *text)
{
    Font_DrawAlpha(font, cx - Font_TextWidth(font, text) / 2, y, z, color, alpha, text);
}

// For text fading out underneath other text fading in: see Font_DrawAlphaAlternate.
static void textCenteredUnder(const Font *font, int cx, int y, int z, u32 color, int alpha, const char *text)
{
    Font_DrawAlphaAlternate(font, cx - Font_TextWidth(font, text) / 2, y, z, color, alpha, text);
}

// Whole-screen fade: in when the lobby opens, out to black when a match starts.
static int fadeAlpha(void)
{
    int alpha = 0;
    if (lobby.time < FADE_IN_FRAMES)
        alpha = 31 - (int)lobby.time * 31 / FADE_IN_FRAMES;
    if (lobby.goTime > GO_FADE_START)
    {
        int out = (lobby.goTime - GO_FADE_START) * 31 / GO_FADE_FRAMES;
        if (out > alpha)
            alpha = out;
    }
    return alpha > 31 ? 31 : alpha;
}

static void drawFade(void)
{
    if (polyAlpha(fadeAlpha(), 30))
        quad2D(0, 0, ScreenWidth, ScreenHeight, 1, RGB15(0, 0, 0));
    NE_PolyFormat(31, 0, 0, NE_CULL_NONE, 0);
}

// ----------------------------------------------------------------------------------------
// Top screen
// ----------------------------------------------------------------------------------------

// The studio's light: a warm spot on the floor around the character, and a glow on the
// backdrop behind it in the colour of the chosen side. Floor and backdrop meet along y = 0
// and share this function, so the seam between them does not show.
static int teamTint[2][3] = {{14, 9, 3}, {4, 8, 16}}; // TERRORISTS, COUNTERTERRORISTS

static u32 studioColor(int32_t x, int32_t y, int32_t z, const int tint[3])
{
    // Distances in 1/16 units keep the products small.
    int dx = x >> 8, dy = (y - 5 * 1024) >> 8, dz = (z + 3 * 4096) >> 8;
    int glowD2 = dx * dx + dy * dy + dz * dz;        // from a point behind and above
    int glow = 256 - glowD2 / 12;                    // 0..256
    if (glow < 0)
        glow = 0;
    glow = glow * glow >> 8;

    int sx = (x - lobby.anim.lastX) >> 8, sz = z >> 8;
    int spotD2 = sx * sx + sz * sz + (y >> 8) * (y >> 8) * 4;
    int spot = 256 - spotD2 / 6;                     // pool of light at the feet
    if (spot < 0)
        spot = 0;
    spot = spot * spot >> 8;

    int r = 2 + (tint[0] * glow >> 8) + (13 * spot >> 8);
    int g = 2 + (tint[1] * glow >> 8) + (12 * spot >> 8);
    int b = 3 + (tint[2] * glow >> 8) + (11 * spot >> 8);
    return color15(r, g, b);
}

static void studioVertex(int32_t x, int32_t y, int32_t z, const int tint[3])
{
    GFX_COLOR = studioColor(x, y, z, tint);
    GFX_VERTEX16 = ((uint32_t)(y & 0xFFFF) << 16) | (x & 0xFFFF);
    GFX_VERTEX16 = z & 0xFFFF;
}

#define STUDIO_BACK (-3 * 4096 - 2048) // backdrop plane, z = -3.5
#define STUDIO_FRONT (2 * 4096)
#define STUDIO_SIDE (7 * 4096 + 3072) // 7.75: vertices are 1.3.12, so +-8 at most
#define STUDIO_TOP (5 * 4096)

static void drawStudio(const int tint[3])
{
    NE_PolyFormat(31, 0, 0, NE_CULL_NONE, 0);
    GFX_TEX_FORMAT = 0;

    // Backdrop and floor: grids fine enough for Gouraud shading to carry the glow and the
    // spot without visible facets. About 400 quads in all, well inside the 2048 a frame.
    const int cols = 18, rows = 9;
    for (int j = 0; j < rows; j++)
    {
        int32_t y1 = STUDIO_TOP * j / rows, y2 = STUDIO_TOP * (j + 1) / rows;
        GFX_BEGIN = GL_QUADS;
        for (int i = 0; i < cols; i++)
        {
            int32_t x1 = -STUDIO_SIDE + 2 * STUDIO_SIDE * i / cols;
            int32_t x2 = -STUDIO_SIDE + 2 * STUDIO_SIDE * (i + 1) / cols;
            studioVertex(x1, y2, STUDIO_BACK, tint);
            studioVertex(x1, y1, STUDIO_BACK, tint);
            studioVertex(x2, y1, STUDIO_BACK, tint);
            studioVertex(x2, y2, STUDIO_BACK, tint);
        }
    }

    // Floor.
    const int depth = 13;
    for (int j = 0; j < depth; j++)
    {
        int32_t z1 = STUDIO_BACK + (STUDIO_FRONT - STUDIO_BACK) * j / depth;
        int32_t z2 = STUDIO_BACK + (STUDIO_FRONT - STUDIO_BACK) * (j + 1) / depth;
        GFX_BEGIN = GL_QUADS;
        for (int i = 0; i < cols; i++)
        {
            int32_t x1 = -STUDIO_SIDE + 2 * STUDIO_SIDE * i / cols;
            int32_t x2 = -STUDIO_SIDE + 2 * STUDIO_SIDE * (i + 1) / cols;
            studioVertex(x1, 0, z1, tint);
            studioVertex(x1, 0, z2, tint);
            studioVertex(x2, 0, z2, tint);
            studioVertex(x2, 0, z1, tint);
        }
    }

    // A soft contact shadow: a fan from a dark centre out to the floor's own colour.
    const int segments = 16;
    int32_t cx = lobby.anim.lastX, cz = 0;
    u32 floorCentre = studioColor(cx, 0, cz, tint);
    u32 dark = RGB15((floorCentre & 31) / 3, ((floorCentre >> 5) & 31) / 3, ((floorCentre >> 10) & 31) / 3);
    GFX_BEGIN = GL_TRIANGLES;
    for (int s = 0; s < segments; s++)
    {
        int32_t a1 = s * ANIM_TURN / segments, a2 = (s + 1) * ANIM_TURN / segments;
        int32_t x1 = cx + (AnimCos(a1) * 1843 >> 12), z1 = cz + (AnimSin(a1) * 1229 >> 12);
        int32_t x2 = cx + (AnimCos(a2) * 1843 >> 12), z2 = cz + (AnimSin(a2) * 1229 >> 12);
        GFX_COLOR = dark;
        GFX_VERTEX16 = (16 << 16) | (cx & 0xFFFF); // 16 = just above the floor
        GFX_VERTEX16 = cz & 0xFFFF;
        GFX_COLOR = studioColor(x1, 0, z1, tint);
        GFX_VERTEX16 = (16 << 16) | (x1 & 0xFFFF);
        GFX_VERTEX16 = z1 & 0xFFFF;
        GFX_COLOR = studioColor(x2, 0, z2, tint);
        GFX_VERTEX16 = (16 << 16) | (x2 & 0xFFFF);
        GFX_VERTEX16 = z2 & 0xFFFF;
    }
}

// Dust drifting up through the light.
static void drawDust(void)
{
    const int count = 14;
    for (int i = 0; i < count; i++)
    {
        uint32_t h = (uint32_t)i * 2654435761u;
        int32_t x = (int32_t)(h % 9000) - 4800;
        int32_t z = (int32_t)((h >> 8) % 7000) - 4200;
        int period = 520 + (int)((h >> 16) % 400);
        int t = (int)((lobby.time + (h >> 4)) % (uint32_t)period);
        int32_t y = t * 9000 / period;                         // rises 2.2 units
        x += AnimSin((int32_t)((lobby.time * 40 + h) & (ANIM_TURN - 1))) >> 4; // and wanders
        int alpha = 12 - abs(t * 24 / period - 12);             // fades in and out
        if (!polyAlpha(alpha, 8))
            continue;
        const int32_t s = 60;
        GFX_BEGIN = GL_QUADS;
        GFX_TEX_FORMAT = 0;
        GFX_COLOR = RGB15(30, 28, 24);
        GFX_VERTEX16 = ((uint32_t)((y + s) & 0xFFFF) << 16) | ((x - s) & 0xFFFF);
        GFX_VERTEX16 = z & 0xFFFF;
        GFX_VERTEX16 = ((uint32_t)((y - s) & 0xFFFF) << 16) | ((x - s) & 0xFFFF);
        GFX_VERTEX16 = z & 0xFFFF;
        GFX_VERTEX16 = ((uint32_t)((y - s) & 0xFFFF) << 16) | ((x + s) & 0xFFFF);
        GFX_VERTEX16 = z & 0xFFFF;
        GFX_VERTEX16 = ((uint32_t)((y + s) & 0xFFFF) << 16) | ((x + s) & 0xFFFF);
        GFX_VERTEX16 = z & 0xFFFF;
    }
}

static void drawNamePlate(void)
{
    int32_t in = AnimEaseOut(progress((int)lobby.time - 20, 24));
    int alpha = mix(0, 31, in);
    int x = mix(-24, 12, in);
    u32 teamColor = chosenTeam == COUNTERTERRORISTS ? COLOR_CT : COLOR_T;
    // No hyphen: the CS font draws '-' as the logo.
    const char *side = chosenTeam == COUNTERTERRORISTS ? "COUNTER TERRORIST" : "TERRORIST";

    // The side label fades out, then the new one in, when it changes.
    int32_t tint = progress(lobby.tintTime, TINT_FRAMES);
    if (tint < ANIM_ONE / 2)
    {
        const char *previous = lobby.tintFrom == COUNTERTERRORISTS ? "COUNTER TERRORIST" : "TERRORIST";
        u32 previousColor = lobby.tintFrom == COUNTERTERRORISTS ? COLOR_CT : COLOR_T;
        Font_DrawAlpha(&lobby.small, x, 14, 4, previousColor, mix(alpha, 0, tint * 2), previous);
    }
    else
        Font_DrawAlpha(&lobby.small, x, 14, 3, teamColor, mix(0, alpha, tint * 2 - ANIM_ONE), side);
    Font_DrawAlpha(&lobby.big, x - 1, 24, 3, COLOR_TEXT, alpha, localPlayer->name);

    int underline = mix(0, 44, AnimEaseOut(progress((int)lobby.time - 28, 20)));
    if (underline > 0 && polyAlpha(alpha, 0))
        quad2D(x, 48, x + underline, 50, 5, teamColor);

    // The match being set up, bottom left.
    char summary[48];
    snprintf(summary, sizeof(summary), "%s  /  %s", rowValue(ROW_MODE), rowValue(ROW_MAP));
    Font_DrawAlpha(&lobby.small, x, 168, 3, COLOR_TEXT_DIM, alpha, summary);
    NE_PolyFormat(31, 0, 0, NE_CULL_NONE, 0);
}

bool drawLobbyTopScreen(void)
{
    if (currentMenu != LOBBY || !lobby.open || lobby.camera == NULL)
        return false;

    // Camera: a slow drift, and a push in when the match starts.
    int32_t push = lobby.goTime >= 0 ? AnimEaseInOut(progress(lobby.goTime, GO_FRAMES)) : 0;
    int32_t driftX = AnimSin((int32_t)(lobby.time * 71 & (ANIM_TURN - 1))) * 160 >> 12;
    int32_t driftY = AnimSin((int32_t)(lobby.time * 53 & (ANIM_TURN - 1))) * 90 >> 12;
    int32_t camZ = 9216 - (push * 1600 >> 12); // 2.25 units back
    NE_CameraSetI(lobby.camera,
                  -1640 + driftX, 3900 + driftY, camZ,   // from
                  -2250, 3150, 0,                        // to: left of the character
                  0, inttof32(1), 0);
    NE_CameraUse(lobby.camera);

    // The backdrop's colour follows the side, fading across when it changes.
    int32_t t = AnimEaseInOut(progress(lobby.tintTime, TINT_FRAMES));
    const int *from = teamTint[lobby.tintFrom == COUNTERTERRORISTS];
    const int *to = teamTint[chosenTeam == COUNTERTERRORISTS];
    int tint[3] = {mix(from[0], to[0], t), mix(from[1], to[1], t), mix(from[2], to[2], t)};
    drawStudio(tint);

    // The character, turned right round by a side change.
    if (CharacterAnim_Rig() != NULL)
    {
        int32_t spin = lobby.spinTime >= 0 ? AnimEaseInOut(progress(lobby.spinTime, SPIN_FRAMES)) * (ANIM_TURN >> 12) : 0;
        NE_Material *skin = lobby.skinTeam == COUNTERTERRORISTS ? PlayerMaterial : PlayerMaterialTerrorist;
        NE_PolyFormat(31, 0, NE_LIGHT_0, NE_CULL_BACK, NE_MODULATION);
        CharacterRig_Draw(&lobby.pose, skin, lobby.anim.lastX, CHAR_Y, lobby.anim.lastZ,
                          lobby.anim.renderYaw + spin, 2048, 2090, 2048);
    }

    drawDust();

    Init2DViewPixelExact();
    drawNamePlate();
    drawFade();
    return true;
}

// ----------------------------------------------------------------------------------------
// Bottom screen
// ----------------------------------------------------------------------------------------

static void drawArrow(int x, int y, int dir, u32 color)
{
    // A small chevron-like triangle pointing left (dir < 0) or right.
    if (dir < 0)
        triangle2D(x + 5, y - 5, x - 1, y, x + 5, y + 5, 12, color);
    else
        triangle2D(x - 5, y - 5, x - 5, y + 5, x + 1, y, 12, color);
}

// Values are set large, unless one would reach the arrows (the longer map names).
static const Font *valueFont(const char *text)
{
    int room = (VALUE_X2 - 12 - 8) - (VALUE_X1 + 10 + 8);
    return Font_TextWidth(&lobby.big, text) <= room ? &lobby.big : &lobby.small;
}

static void drawRow(int row, int enterOffset, int alpha)
{
    int y = rowY(row);
    int x1 = ROW_X1 + enterOffset, x2 = ROW_X2 + enterOffset;
    bool locked = rowLocked(row);

    if (polyAlpha(alpha, 10 + row))
    {
        quad2D(x1, y, x2, y + ROW_H, 60, COLOR_PANEL);
        quad2D(x1, y + ROW_H, x2, y + ROW_H + 1, 60, COLOR_PANEL_EDGE);
    }

    Font_DrawAlpha(&lobby.small, x1 + 9, y + 6, 20, COLOR_TEXT_DIM, alpha, rowLabels[row]);

    if (row == ROW_TEAM)
    {
        int pill = (lobby.teamPill >> 8) + enterOffset;
        u32 pillColor = chosenTeam == COUNTERTERRORISTS ? COLOR_CT : COLOR_T;
        if (polyAlpha(alpha, 20))
            quad2D(pill, y + 3, pill + TEAM_SEG_W, y + ROW_H - 3, 40, pillColor);
        for (int side = 0; side < 2; side++)
        {
            int team = side == 0 ? COUNTERTERRORISTS : TERRORISTS;
            int sx = (side == 0 ? TEAM_CT_X1 : TEAM_T_X1) + enterOffset;
            bool chosen = team == chosenTeam;
            u32 color = chosen ? RGB15(3, 3, 4) : COLOR_TEXT_DIM;
            // A press flashes the segment.
            int press = lobby.pressTime[ROW_TEAM][side];
            if (press < PRESS_FRAMES && polyAlpha((PRESS_FRAMES - press) * 2, 24 + side))
                quad2D(sx, y + 3, sx + TEAM_SEG_W, y + ROW_H - 3, 35, RGB15(31, 31, 31));
            textCentered(&lobby.big, sx + TEAM_SEG_W / 2, y - 1, 20, color, alpha, side == 0 ? "CT" : "T");
        }
        return;
    }

    int left = VALUE_X1 + enterOffset, right = VALUE_X2 + enterOffset;
    int leftArrow = left + 10, rightArrow = right - 12;
    int centre = (leftArrow + rightArrow) / 2;
    u32 valueColor = locked ? COLOR_TEXT_DIM : COLOR_TEXT;

    // A new value slides in from the side it was asked for; the old one slides out.
    int32_t s = AnimEaseOut(progress(lobby.slideTime[row], SLIDE_FRAMES));
    const char *value = rowValue(row);
    const Font *font = valueFont(value);
    int valueY = font == &lobby.big ? y - 1 : y + 6;
    if (s < ANIM_ONE && lobby.slideFrom[row] != NULL)
    {
        const char *old = lobby.slideFrom[row];
        const Font *oldFont = valueFont(old);
        int oldY = oldFont == &lobby.big ? y - 1 : y + 6;
        textCenteredUnder(oldFont, centre - lobby.slideDir[row] * mix(0, 22, s), oldY, 22, COLOR_TEXT_DIM,
                          mix(alpha, 0, s), old);
    }
    textCentered(font, centre + lobby.slideDir[row] * mix(22, 0, s), valueY, 20, valueColor,
                 mix(0, alpha, s), value);

    if (!locked && polyAlpha(alpha, 0))
    {
        for (int k = 0; k < 2; k++)
        {
            int press = lobby.pressTime[row][k];
            u32 color = press < PRESS_FRAMES ? color15(mix(31, 16, press * ANIM_ONE / PRESS_FRAMES), mix(31, 17, press * ANIM_ONE / PRESS_FRAMES), mix(31, 19, press * ANIM_ONE / PRESS_FRAMES)) : COLOR_TEXT_DIM;
            int nudge = press < PRESS_FRAMES ? (k ? 1 : -1) * (PRESS_FRAMES - press) / 4 : 0;
            drawArrow((k ? rightArrow : leftArrow) + nudge, y + ROW_H / 2, k ? 1 : -1, color);
        }
    }
}

static void drawGoButton(int alpha, int offset)
{
    int x1 = GO_X1, y1 = GO_Y1 + offset, x2 = GO_X2, y2 = GO_Y2 + offset;
    bool starting = lobby.goTime >= 0;

    if (polyAlpha(alpha, 16))
        NE_2DDrawQuadGradient(x1, y1, x2, y2, 50, RGB15(10, 23, 11), RGB15(10, 23, 11), RGB15(5, 15, 6),
                              RGB15(5, 15, 6));

    // A slow breathing highlight while it waits, a flash when pressed.
    int pulse = starting ? 0 : 3 + ((AnimSin((int32_t)(lobby.time * 364 & (ANIM_TURN - 1))) + ANIM_ONE) * 4 >> 13);
    if (starting)
        pulse = AnimClamp(18 - lobby.goTime, 0, 18);
    if (polyAlpha(pulse * alpha / 31, 17))
        quad2D(x1, y1, x2, y2, 45, RGB15(31, 31, 31));

    // Focus ring.
    if (lobby.focus == ROW_GO && polyAlpha(alpha, 0))
    {
        u32 c = COLOR_ACCENT;
        quad2D(x1 - 2, y1 - 2, x2 + 2, y1, 40, c);
        quad2D(x1 - 2, y2, x2 + 2, y2 + 2, 40, c);
        quad2D(x1 - 2, y1, x1, y2, 40, c);
        quad2D(x2, y1, x2 + 2, y2, 40, c);
    }

    textCentered(&lobby.big, (x1 + x2) / 2, (y1 + y2) / 2 - 13, 20, RGB15(30, 31, 30), alpha, starting ? "GO!" : "GO");
}

static void drawLobbyMenu()
{
    Init2DViewPixelExact();

    NE_2DDrawQuadGradient(0, 0, ScreenWidth, ScreenHeight, 100, RGB15(3, 4, 6), RGB15(3, 4, 6), RGB15(1, 1, 2),
                          RGB15(1, 1, 2));

    // Header: the tab name, with an accent line drawing itself under it.
    quad2D(0, 0, ScreenWidth, HEADER_H, 90, RGB15(5, 6, 8));
    quad2D(0, HEADER_H, ScreenWidth, HEADER_H + 1, 90, COLOR_PANEL_EDGE);
    int titleWidth = Font_TextWidth(&lobby.big, "PLAY");
    Font_DrawAlpha(&lobby.big, 10, 0, 20, COLOR_TEXT, 31, "PLAY");
    int line = mix(0, titleWidth, AnimEaseOut(progress((int)lobby.time - 4, 18)));
    if (line > 0)
        quad2D(10, HEADER_H - 3, 10 + line, HEADER_H, 80, COLOR_ACCENT);
    Font_DrawAlpha(&lobby.small, 10 + titleWidth + 10, 7, 20, COLOR_TEXT_DIM, 31, "OFFLINE MATCH");

    // Rows slide in from the right, one after another.
    int enter[SETTING_ROWS], rowAlpha[SETTING_ROWS];
    for (int row = 0; row < SETTING_ROWS; row++)
    {
        int32_t p = AnimEaseOut(progress((int)lobby.time - ROW_ENTER_DELAY - row * ROW_ENTER_STAGGER, ROW_ENTER_FRAMES));
        enter[row] = mix(72, 0, p);
        rowAlpha[row] = mix(0, 31, p);
    }

    // The focus highlight glides between rows.
    if (lobby.focus < SETTING_ROWS)
    {
        int fy = lobby.focusY >> 8;
        int offset = enter[lobby.focus];
        if (polyAlpha(rowAlpha[lobby.focus] / 5, 15))
            quad2D(ROW_X1 + offset, fy, ROW_X2 + offset, fy + ROW_H, 55, RGB15(24, 27, 31));
        if (polyAlpha(rowAlpha[lobby.focus], 0))
            quad2D(ROW_X1 + offset, fy, ROW_X1 + 3 + offset, fy + ROW_H, 54, COLOR_ACCENT);
    }

    for (int row = 0; row < SETTING_ROWS; row++)
        drawRow(row, enter[row], rowAlpha[row]);

    // GO comes up last.
    int32_t go = AnimEaseOut(progress((int)lobby.time - ROW_ENTER_DELAY - SETTING_ROWS * ROW_ENTER_STAGGER, ROW_ENTER_FRAMES));
    drawGoButton(mix(0, 31, go), mix(18, 0, go));

    // Controls, beside GO.
    static const char *const hints[3] = {"UP DOWN  choose", "LEFT RIGHT  change", "B  back"};
    for (int i = 0; i < 3; i++)
        Font_DrawAlpha(&lobby.small, 10, GO_Y1 + 1 + i * 13, 20, COLOR_TEXT_OFF, mix(0, 31, go), hints[i]);

    drawFade();

    // Whatever drawBottomScreenUI draws next was laid out for the engine's own view.
    NE_2DViewInit();
}
