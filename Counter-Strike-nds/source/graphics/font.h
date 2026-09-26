// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Anti-aliased proportional text, drawn with the 3D engine.

#ifndef FONT_H_ /* Include guard */
#define FONT_H_

#include <NEMain.h>
#include "font_core.h"

typedef struct
{
    FontData data;
    NE_Material *material;
    NE_Palette *palette;
} Font;

// Parses a CSF1 font embedded in the binary and uploads its atlas (a few KB of texture
// VRAM). Returns false, holding nothing, if the data is bad or VRAM is full.
bool Font_Load(Font *font, const uint8_t *data, uint32_t size);
void Font_Unload(Font *font);
bool Font_IsLoaded(const Font *font);

// Draws one line with its line box's top-left corner at (x, y). Call inside a 2D view
// (NE_2DViewInit); z is the usual 2D priority, lower in front. Every glyph costs at most
// one quad per coverage level, and a line costs FONT_LEVELS polygon batches in all.
// Leaves the polygon format at the 2D default.
void Font_Draw(const Font *font, int x, int y, int z, u32 color, const char *text);

// Font_Draw at a fraction of full opacity: alpha 0 (draws nothing) to 31 (Font_Draw).
void Font_DrawAlpha(const Font *font, int x, int y, int z, u32 color, int alpha, const char *text);

// The same on the other set of polygon IDs. The DS does not draw a translucent pixel over
// one from a polygon with the same ID, so text fading across other fading text (a value
// sliding out as its successor slides in) must use the other set or lose strokes.
void Font_DrawAlphaAlternate(const Font *font, int x, int y, int z, u32 color, int alpha, const char *text);

// Width of one line in whole pixels, 0 if the font is not loaded.
int Font_TextWidth(const Font *font, const char *text);

#endif // FONT_H_
