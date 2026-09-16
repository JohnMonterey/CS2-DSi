// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Proportional bitmap fonts: file parsing and text layout.

#ifndef FONT_CORE_H_ /* Include guard */
#define FONT_CORE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * No Nitro Engine, no libnds: this compiles for the host test runner as well as the DS,
 * and holds every decision about where a glyph lands. font.c only turns placements into
 * quads.
 *
 * A font is a CSF1 file made by tools/assets/dsfont.py from a TrueType font. The DS 3D
 * engine cannot blend per texel, so anti-aliasing is stored as FONT_LEVELS coverage levels:
 * each level is a 1-bit mask in its own layer of one 2bpp atlas, all layers sharing one
 * layout, and each is drawn with its own polygon alpha. See dsfont.py for the file layout.
 *
 * Positions are kept in 1/16 px. A glyph's origin lands on the nearest whole pixel; the
 * converter rasterised each glyph for exactly that, so text stays crisp.
 */

#define FONT_LEVELS 3

typedef struct
{
    uint8_t x, y;      // position in level 1's atlas layer
    uint8_t w, h;      // 0x0 for glyphs with no ink (space)
    int8_t left;       // from the pen position
    int8_t top;        // from the top of the line box
    uint8_t levelMask; // bit n set: level n+1 has texels
    uint16_t advance;  // 1/16 px
} FontGlyph;

typedef struct
{
    uint8_t firstChar;
    uint8_t glyphCount;
    uint8_t lineHeight;
    uint8_t ascent;
    uint16_t kernCount;
    uint16_t atlasWidth;
    uint16_t layerHeight; // the atlas is layerHeight * FONT_LEVELS texels tall
    const uint8_t *glyphTable;
    const uint8_t *kernTable;
    const uint8_t *atlas; // 2bpp, row-major, low bits first; 4-byte aligned in the file
} FontData;

typedef struct
{
    FontGlyph glyph;
    int x, y; // top-left of the glyph's bitmap on screen
} FontPlacement;

typedef struct
{
    const FontData *font;
    const char *text;
    int pen; // 1/16 px
    int y;
    int previous; // previous drawn character, or -1
} FontCursor;

// Checks the header and that every table lies inside `size` bytes.
bool FontData_Parse(FontData *font, const uint8_t *data, uint32_t size);

// False for characters the font does not cover.
bool FontData_Glyph(const FontData *font, unsigned char c, FontGlyph *glyph);

// Pair adjustment in 1/16 px, 0 when the pair is not kerned.
int FontData_Kerning(const FontData *font, unsigned char left, unsigned char right);

// Advance width of a single line, in whole pixels.
int FontData_TextWidth(const FontData *font, const char *text);

// Lays out one line with its line box's top-left corner at (x, y). Characters the font
// does not cover are skipped. Glyphs with no ink advance the pen but are not returned.
void FontCursor_Begin(FontCursor *cursor, const FontData *font, const char *text, int x, int y);
bool FontCursor_Next(FontCursor *cursor, FontPlacement *placement);

#endif // FONT_CORE_H_
