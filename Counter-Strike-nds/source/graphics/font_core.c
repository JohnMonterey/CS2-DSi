// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Proportional bitmap fonts: file parsing and text layout. See font_core.h.

#include "font_core.h"

#include <string.h>

#define FONT_HEADER_SIZE 24
#define FONT_GLYPH_SIZE 10
#define FONT_KERN_SIZE 4
#define FONT_VERSION 1

static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool FontData_Parse(FontData *font, const uint8_t *data, uint32_t size)
{
    memset(font, 0, sizeof(*font));
    if (size < FONT_HEADER_SIZE || memcmp(data, "CSF1", 4) != 0 || data[4] != FONT_VERSION ||
        data[7] != FONT_LEVELS)
        return false;

    font->firstChar = data[5];
    font->glyphCount = data[6];
    font->lineHeight = data[8];
    font->ascent = data[9];
    font->kernCount = read16(data + 10);
    font->atlasWidth = read16(data + 12);
    font->layerHeight = read16(data + 14);

    uint32_t glyphTable = read16(data + 16);
    uint32_t kernTable = read16(data + 18);
    uint32_t atlas = read32(data + 20);
    uint32_t atlasBytes = ((uint32_t)font->atlasWidth * font->layerHeight * FONT_LEVELS + 3) / 4;

    if (glyphTable + (uint32_t)font->glyphCount * FONT_GLYPH_SIZE > size ||
        kernTable + (uint32_t)font->kernCount * FONT_KERN_SIZE > size ||
        atlas > size || atlasBytes > size - atlas || (atlas & 3) != 0)
        return false;

    font->glyphTable = data + glyphTable;
    font->kernTable = data + kernTable;
    font->atlas = data + atlas;
    return true;
}

bool FontData_Glyph(const FontData *font, unsigned char c, FontGlyph *glyph)
{
    if (c < font->firstChar || c - font->firstChar >= font->glyphCount)
        return false;

    const uint8_t *p = font->glyphTable + (c - font->firstChar) * FONT_GLYPH_SIZE;
    glyph->x = p[0];
    glyph->y = p[1];
    glyph->w = p[2];
    glyph->h = p[3];
    glyph->left = (int8_t)p[4];
    glyph->top = (int8_t)p[5];
    glyph->levelMask = p[6];
    glyph->advance = read16(p + 8);
    return true;
}

int FontData_Kerning(const FontData *font, unsigned char left, unsigned char right)
{
    // The table is sorted by (left, right): binary search it.
    unsigned key = (left << 8) | right;
    int lo = 0, hi = font->kernCount - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const uint8_t *p = font->kernTable + mid * FONT_KERN_SIZE;
        unsigned pair = (p[0] << 8) | p[1];
        if (pair == key)
            return (int16_t)read16(p + 2);
        if (pair < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

int FontData_TextWidth(const FontData *font, const char *text)
{
    int pen = 0;
    int previous = -1;
    for (; *text; text++)
    {
        unsigned char c = (unsigned char)*text;
        FontGlyph glyph;
        if (!FontData_Glyph(font, c, &glyph))
        {
            previous = -1;
            continue;
        }
        if (previous >= 0)
            pen += FontData_Kerning(font, (unsigned char)previous, c);
        pen += glyph.advance;
        previous = c;
    }
    return (pen + 8) >> 4;
}

void FontCursor_Begin(FontCursor *cursor, const FontData *font, const char *text, int x, int y)
{
    cursor->font = font;
    cursor->text = text;
    cursor->pen = x * 16;
    cursor->y = y;
    cursor->previous = -1;
}

bool FontCursor_Next(FontCursor *cursor, FontPlacement *placement)
{
    while (*cursor->text)
    {
        unsigned char c = (unsigned char)*cursor->text++;
        FontGlyph glyph;
        if (!FontData_Glyph(cursor->font, c, &glyph))
        {
            cursor->previous = -1;
            continue;
        }
        if (cursor->previous >= 0)
            cursor->pen += FontData_Kerning(cursor->font, (unsigned char)cursor->previous, c);

        int origin = (cursor->pen + 8) >> 4;
        cursor->pen += glyph.advance;
        cursor->previous = c;

        if (glyph.w == 0 || glyph.h == 0)
            continue;

        placement->glyph = glyph;
        placement->x = origin + glyph.left;
        placement->y = cursor->y + glyph.top;
        return true;
    }
    return false;
}
