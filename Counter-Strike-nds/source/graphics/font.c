// SPDX-License-Identifier: MIT
//
// Counter Strike Nintendo DS Multiplayer Edition (CS:DS)
//
// Anti-aliased proportional text, drawn with the 3D engine. See font.h.

#include "font.h"

// Polygon alpha per coverage level. The DS blends as (src * (a + 1) + dst * (31 - a)) / 32,
// so these are about 1/3, 2/3 and opaque. tools/assets/dsfont.py predicts frames with the
// same values; change both together.
static const u8 levelAlpha[FONT_LEVELS] = {10, 20, 31};

// A translucent pixel is not drawn over one left by a translucent polygon with the same
// ID. The levels of one glyph never share a pixel, but neighbouring glyphs can overlap
// where kerning pulls them together, so each level gets its own ID.
static const u8 levelPolygonId[FONT_LEVELS] = {61, 62, 63};

bool Font_Load(Font *font, const uint8_t *data, uint32_t size)
{
    font->material = NULL;
    font->palette = NULL;
    if (!FontData_Parse(&font->data, data, size))
        return false;

    font->material = NE_MaterialCreate();
    font->palette = NE_PaletteCreate();
    if (font->material == NULL || font->palette == NULL)
    {
        Font_Unload(font);
        return false;
    }

    // Index 0 is transparent; 1 is white, so the vertex colour decides the text colour.
    static u16 colors[4] = {0, RGB15(31, 31, 31), 0, 0};
    if (!NE_MaterialTexLoad(font->material, GL_RGB4, font->data.atlasWidth,
                            font->data.layerHeight * FONT_LEVELS, GL_TEXTURE_COLOR0_TRANSPARENT,
                            (void *)font->data.atlas))
    {
        // A failed load can leave the material naming slot 0, which belongs to some other
        // texture; deleting it as it stands would free that one.
        font->material->texindex = NE_NO_TEXTURE;
        Font_Unload(font);
        return false;
    }
    if (!NE_PaletteLoad(font->palette, colors, 4, GL_RGB4))
    {
        font->palette->index = NE_NO_PALETTE;
        Font_Unload(font);
        return false;
    }
    NE_MaterialTexSetPal(font->material, font->palette);
    return true;
}

void Font_Unload(Font *font)
{
    if (font->material != NULL)
        NE_MaterialDelete(font->material);
    if (font->palette != NULL)
        NE_PaletteDelete(font->palette);
    font->material = NULL;
    font->palette = NULL;
}

bool Font_IsLoaded(const Font *font)
{
    return font->material != NULL;
}

void Font_Draw(const Font *font, int x, int y, int z, u32 color, const char *text)
{
    if (!Font_IsLoaded(font))
        return;

    for (int level = 0; level < FONT_LEVELS; level++)
    {
        // The polygon format is latched when a batch begins.
        NE_PolyFormat(levelAlpha[level], levelPolygonId[level], 0, NE_CULL_NONE, NE_MODULATION);
        NE_MaterialUse(font->material);
        GFX_COLOR = color;
        GFX_BEGIN = GL_QUADS;

        int layer = font->data.layerHeight * level;
        FontCursor cursor;
        FontPlacement p;
        FontCursor_Begin(&cursor, &font->data, text, x, y);
        while (FontCursor_Next(&cursor, &p))
        {
            if (!(p.glyph.levelMask & (1 << level)))
                continue;

            int u1 = p.glyph.x, u2 = p.glyph.x + p.glyph.w;
            int v1 = p.glyph.y + layer, v2 = v1 + p.glyph.h;
            int x1 = p.x, x2 = p.x + p.glyph.w;
            int y1 = p.y, y2 = p.y + p.glyph.h;

            GFX_TEX_COORD = TEXTURE_PACK(inttot16(u1), inttot16(v1));
            GFX_VERTEX16 = (y1 << 16) | (x1 & 0xFFFF);
            GFX_VERTEX16 = z;

            GFX_TEX_COORD = TEXTURE_PACK(inttot16(u1), inttot16(v2));
            GFX_VERTEX_XY = (y2 << 16) | (x1 & 0xFFFF);

            GFX_TEX_COORD = TEXTURE_PACK(inttot16(u2), inttot16(v2));
            GFX_VERTEX_XY = (y2 << 16) | (x2 & 0xFFFF);

            GFX_TEX_COORD = TEXTURE_PACK(inttot16(u2), inttot16(v1));
            GFX_VERTEX_XY = (y1 << 16) | (x2 & 0xFFFF);
        }
    }

    // Back to what NE_2DViewInit sets, so whatever is drawn next is opaque again.
    NE_PolyFormat(31, 0, 0, NE_CULL_NONE, 0);
}
