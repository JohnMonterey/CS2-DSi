"""TrueType -> Nintendo DS bitmap font, and a reference renderer for it.

The DS 3D engine cannot blend a texture per texel: a texel is either drawn or not, and
alpha belongs to the whole polygon. So anti-aliased text is stored as coverage *levels*.
Each glyph is rasterised once, unhinted, with 16x16 supersampling; its coverage is
rounded to LEVELS steps; and each non-zero step becomes its own 1-bit mask. The masks sit
in one 2bpp (GL_RGB4) atlas, stacked vertically in the same layout, and the game draws
them as one textured quad per glyph per level with that level's polygon alpha. Three
levels -- about 1/3, 2/3 and opaque -- come within a couple of percent of full grey-scale
anti-aliasing at this size, where 1-bit text visibly breaks the letterforms.

Usage:
    python3 tools/assets/dsfont.py build --ttf font.ttf --size 20 --out data/font.bin
    python3 tools/assets/dsfont.py render --font data/font.bin --text "Quit" --out quit.png

Needs numpy, Pillow and fontTools (pip install numpy pillow fonttools). The game build
does not: the generated .bin is committed.

Binary layout (little-endian), read by Counter-Strike-nds/source/graphics/font_core.c:

    0x00  char[4] "CSF1"
    0x04  u8  version (1)        u8  first_char       u8  glyph_count     u8  levels (3)
    0x08  u8  line_height        u8  ascent           u16 kern_count
    0x0C  u16 atlas_width        u16 layer_height     (atlas height = layer_height * levels)
    0x10  u16 glyph_table        u16 kern_table       (offsets from the start of the file)
    0x14  u32 atlas_offset       (4-byte aligned, so it can be copied to VRAM word-wise)
    glyph (10 bytes): u8 x, u8 y, u8 w, u8 h, s8 left, s8 top, u8 level_mask, u8 pad,
                      u16 advance (1/16 px)
          x, y locate the glyph in level 1's layer; level n is layer_height*(n-1) lower.
          left is from the pen position, top from the top of the line box.
    kern  (4 bytes):  u8 left_char, u8 right_char, s16 adjust (1/16 px), sorted by pair
    atlas: 2bpp texels, row-major, low bits first; index 0 transparent, 1 opaque.

Pen positions are kept in 1/16 px. A glyph's origin lands on (pen + 8) >> 4, and the
bitmap was rasterised with its origin at PHASE within that pixel, so the rounding costs
at most half a pixel and never blurs a stem.
"""

from __future__ import annotations

import argparse
import struct
import sys

import numpy as np

MAGIC = b"CSF1"
VERSION = 1
LEVELS = 3
SUPERSAMPLE = 16
PHASE = 0.5            # sub-pixel position of the glyph origin inside its pixel
PAD = 1                # empty texels between glyphs in the atlas
GLYPH_FORMAT = "<BBBBbbBBH"
KERN_FORMAT = "<BBh"
HEADER_FORMAT = "<4sBBBBBBHHHHHI"


# --------------------------------------------------------------------------------------
# Rasterising
# --------------------------------------------------------------------------------------


def _outline_pen(glyphset):
    from fontTools.pens.basePen import BasePen

    class FlattenPen(BasePen):
        """Collects contours as polylines; curves become short straight segments."""

        def __init__(self):
            super().__init__(glyphset)
            self.contours = []

        def _moveTo(self, p):
            self.contours.append([p])

        def _lineTo(self, p):
            self.contours[-1].append(p)

        def _qCurveToOne(self, p1, p2):
            p0 = self.contours[-1][-1]
            for t in np.linspace(0.0, 1.0, 9)[1:]:
                u = 1.0 - t
                self.contours[-1].append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                                          u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))

        def _curveToOne(self, p1, p2, p3):
            p0 = self.contours[-1][-1]
            for t in np.linspace(0.0, 1.0, 9)[1:]:
                u = 1.0 - t
                self.contours[-1].append(
                    (u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                     u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))

        def _closePath(self):
            pass

    return FlattenPen()


def coverage(contours, width, height):
    """Nonzero-winding fill of pixel-space contours (y down) -> coverage in [0, 1]."""
    ss = SUPERSAMPLE
    edges = []
    for contour in contours:
        for i in range(len(contour)):
            x0, y0 = contour[i]
            x1, y1 = contour[(i + 1) % len(contour)]
            if y0 != y1:
                edges.append((x0 * ss, y0 * ss, x1 * ss, y1 * ss))
    grid = np.zeros((height * ss, width * ss), np.uint8)
    if edges:
        e = np.array(edges, float)
        lo = np.minimum(e[:, 1], e[:, 3])
        hi = np.maximum(e[:, 1], e[:, 3])
        for row in range(height * ss):
            sy = row + 0.5
            m = (sy >= lo) & (sy < hi)
            if not m.any():
                continue
            x0, y0, x1, y1 = e[m, 0], e[m, 1], e[m, 2], e[m, 3]
            xs = x0 + (sy - y0) / (y1 - y0) * (x1 - x0)
            wind = np.where(y1 > y0, 1, -1)
            order = np.argsort(xs, kind="stable")
            xs, wind = xs[order], wind[order]
            total = 0
            for k in range(len(xs) - 1):
                total += wind[k]
                if total:
                    a = max(int(np.ceil(xs[k] - 0.5)), 0)
                    b = min(int(np.ceil(xs[k + 1] - 0.5)), width * ss)
                    if b > a:
                        grid[row, a:b] = 1
    return grid.reshape(height, ss, width, ss).mean(axis=(1, 3))


class TrueType:
    def __init__(self, path):
        from fontTools.ttLib import TTFont

        self.tt = TTFont(path)
        self.cmap = self.tt.getBestCmap()
        self.glyphset = self.tt.getGlyphSet()
        self.upm = self.tt["head"].unitsPerEm
        self.kern = {}
        if "kern" in self.tt:
            for table in self.tt["kern"].kernTables:
                self.kern.update(table.kernTable)

    def glyph_name(self, char):
        return self.cmap.get(ord(char))

    def advance(self, name):
        return self.tt["hmtx"][name][0]

    def contours(self, name):
        pen = _outline_pen(self.glyphset)
        self.glyphset[name].draw(pen)
        return pen.contours


# --------------------------------------------------------------------------------------
# Building
# --------------------------------------------------------------------------------------


def quantise(cov):
    return np.rint(cov * LEVELS).astype(np.uint8)


def build(ttf_path, size, chars, size_y=None):
    """Rasterise `chars` and pack them. Returns a dict describing the whole font."""
    font = TrueType(ttf_path)
    sx = size / font.upm
    sy = (size_y or size) / font.upm

    y_max = max(font.tt["head"].yMax, 1)
    y_min = min(font.tt["head"].yMin, 0)
    ascent = int(np.ceil(y_max * sy))
    line_height = ascent + int(np.ceil(-y_min * sy))

    space = font.glyph_name(" ")
    glyphs = []
    for char in chars:
        name = font.glyph_name(char)
        advance = font.advance(name if name else space) * sx
        record = {"char": char, "advance": int(round(advance * 16)), "levels": None,
                  "left": 0, "top": 0}
        if name:
            contours = font.contours(name)
            if contours:
                # Rasterise into a canvas generous enough for any glyph at this size, with
                # the origin at (margin + PHASE, ascent + margin), then crop to the ink.
                margin = 4
                w = int(np.ceil(font.advance(name) * sx + size)) + 2 * margin
                h = line_height + 2 * margin
                pixel = [[(margin + PHASE + x * sx, margin + ascent - y * sy) for x, y in c]
                         for c in contours]
                levels = quantise(coverage(pixel, w, h))
                ys, xs = np.nonzero(levels)
                if len(xs):
                    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
                    record["levels"] = levels[y0:y1, x0:x1]
                    record["left"] = int(x0 - margin)
                    record["top"] = int(y0 - margin)
        glyphs.append(record)

    atlas_width, layer_height = pack(glyphs)

    kerning = []
    codes = {c: i for i, c in enumerate(chars)}
    for (left, right), value in sorted(font.kern.items()):
        lc = next((c for c in chars if font.glyph_name(c) == left), None)
        rc = next((c for c in chars if font.glyph_name(c) == right), None)
        if lc is None or rc is None:
            continue
        adjust = int(round(value * sx * 16))
        if adjust:
            kerning.append((ord(lc), ord(rc), adjust))
    kerning.sort()

    return {"first": ord(chars[0]), "glyphs": glyphs, "ascent": ascent,
            "line_height": line_height, "atlas_width": atlas_width,
            "layer_height": layer_height, "kerning": kerning, "codes": codes}


def pack(glyphs):
    """Shelf-pack the glyph boxes. Picks the power-of-two width with the smallest area,
    preferring the wider atlas when the areas are within 10% of each other."""
    best = None
    for width in (256, 128, 64):
        order = sorted((g for g in glyphs if g["levels"] is not None),
                       key=lambda g: -g["levels"].shape[0])
        x = y = shelf = 0
        places = {}
        ok = True
        for g in order:
            h, w = g["levels"].shape
            if w + PAD > width:
                ok = False
                break
            if x + w + PAD > width:
                x, y, shelf = 0, y + shelf, 0
            places[id(g)] = (x, y)
            x += w + PAD
            shelf = max(shelf, h + PAD)
        height = y + shelf
        if ok and height <= 255 and (best is None or width * height < best[0] * 0.9):
            best = (width * height, width, height, places)
    if best is None:
        raise SystemExit("glyphs do not fit a 256-wide atlas layer; use a smaller size")
    _, width, height, places = best
    for g in glyphs:
        g["x"], g["y"] = places.get(id(g), (0, 0))
    return width, height


def serialise(font):
    glyphs = font["glyphs"]
    header_size = struct.calcsize(HEADER_FORMAT)
    glyph_table = header_size
    kern_table = glyph_table + len(glyphs) * struct.calcsize(GLYPH_FORMAT)
    atlas_offset = kern_table + len(font["kerning"]) * struct.calcsize(KERN_FORMAT)
    atlas_offset = (atlas_offset + 3) & ~3

    out = bytearray(struct.pack(
        HEADER_FORMAT, MAGIC, VERSION, font["first"], len(glyphs), LEVELS,
        font["line_height"], font["ascent"], len(font["kerning"]),
        font["atlas_width"], font["layer_height"], glyph_table, kern_table, atlas_offset))
    for g in glyphs:
        if g["levels"] is None:
            out += struct.pack(GLYPH_FORMAT, 0, 0, 0, 0, 0, 0, 0, 0, g["advance"])
            continue
        h, w = g["levels"].shape
        mask = 0
        for level in range(1, LEVELS + 1):
            if (g["levels"] == level).any():
                mask |= 1 << (level - 1)
        out += struct.pack(GLYPH_FORMAT, g["x"], g["y"], w, h, g["left"], g["top"], mask, 0,
                           g["advance"])
    for left, right, adjust in font["kerning"]:
        out += struct.pack(KERN_FORMAT, left, right, adjust)
    out += bytes(atlas_offset - len(out))

    width, layer = font["atlas_width"], font["layer_height"]
    texels = np.zeros((layer * LEVELS, width), np.uint8)
    for g in glyphs:
        if g["levels"] is None:
            continue
        h, w = g["levels"].shape
        for level in range(1, LEVELS + 1):
            top = g["y"] + layer * (level - 1)
            texels[top:top + h, g["x"]:g["x"] + w] = (g["levels"] == level)
    flat = texels.reshape(-1)
    packed = flat[0::4] | (flat[1::4] << 2) | (flat[2::4] << 4) | (flat[3::4] << 6)
    out += packed.astype(np.uint8).tobytes()
    out += bytes((-len(out)) % 4)
    return bytes(out)


# --------------------------------------------------------------------------------------
# Reading and reference rendering. Mirrors font_core.c; the host test holds them together.
# --------------------------------------------------------------------------------------


class BitmapFont:
    def __init__(self, data: bytes):
        (magic, version, self.first, count, self.levels, self.line_height, self.ascent,
         kern_count, self.atlas_width, self.layer_height, glyph_table, kern_table,
         atlas_offset) = struct.unpack_from(HEADER_FORMAT, data, 0)
        if magic != MAGIC or version != VERSION:
            raise ValueError("not a CSF1 font")
        size = struct.calcsize(GLYPH_FORMAT)
        self.glyphs = [struct.unpack_from(GLYPH_FORMAT, data, glyph_table + i * size)
                       for i in range(count)]
        ksize = struct.calcsize(KERN_FORMAT)
        self.kerning = {(l, r): a for l, r, a in
                        (struct.unpack_from(KERN_FORMAT, data, kern_table + i * ksize)
                         for i in range(kern_count))}
        height = self.layer_height * self.levels
        packed = np.frombuffer(data, np.uint8, (self.atlas_width * height + 3) // 4, atlas_offset)
        texels = np.stack([(packed >> s) & 3 for s in (0, 2, 4, 6)], axis=1).reshape(-1)
        self.atlas = texels[:self.atlas_width * height].reshape(height, self.atlas_width)

    def glyph(self, char):
        index = ord(char) - self.first
        if 0 <= index < len(self.glyphs):
            return self.glyphs[index]
        return None

    def placements(self, text, x, y):
        """(char, glyph, screen_x, screen_y) for every drawn glyph, as the DS lays them out."""
        pen = x * 16
        prev = None
        out = []
        for char in text:
            g = self.glyph(char)
            if g is None:
                prev = None
                continue
            if prev is not None:
                pen += self.kerning.get((ord(prev), ord(char)), 0)
            gx, gy, w, h, left, top, mask, _, advance = g
            if w and h:
                out.append((char, g, ((pen + 8) >> 4) + left, y + top))
            pen += advance
            prev = char
        return out

    def width(self, text):
        pen = 0
        prev = None
        for char in text:
            g = self.glyph(char)
            if g is None:
                prev = None
                continue
            if prev is not None:
                pen += self.kerning.get((ord(prev), ord(char)), 0)
            pen += g[8]
            prev = char
        return (pen + 8) >> 4

    def render_levels(self, text, x, y, width, height):
        """Screen-sized level map (0..levels) -- what the DS draws, before blending."""
        out = np.zeros((height, width), np.uint8)
        for level in range(self.levels, 0, -1):
            for _, g, sx, sy in self.placements(text, x, y):
                gx, gy, w, h = g[:4]
                if not g[6] & (1 << (level - 1)):
                    continue
                mask = self.atlas[gy + self.layer_height * (level - 1):
                                  gy + self.layer_height * (level - 1) + h, gx:gx + w]
                for yy in range(h):
                    for xx in range(w):
                        px, py = sx + xx, sy + yy
                        if mask[yy, xx] and 0 <= px < width and 0 <= py < height and not out[py, px]:
                            out[py, px] = level
        return out


# The DS blends a polygon with alpha a (0..31) as (src * (a + 1) + dst * (31 - a)) / 32.
# These are the level alphas the game uses; keep them in step with font.c.
LEVEL_ALPHA = {1: 10, 2: 20, 3: 31}


def blend(background_rgb, levels, color_rgb):
    """Composite a level map over an RGB float image the way the DS 3D engine blends."""
    out = background_rgb.astype(float).copy()
    color = np.array(color_rgb, float)
    for level, alpha in LEVEL_ALPHA.items():
        m = levels == level
        weight = 1.0 if alpha == 31 else (alpha + 1) / 32.0
        out[m] = color * weight + out[m] * (1 - weight)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="convert a TTF into a CSF1 bitmap font")
    b.add_argument("--ttf", required=True)
    b.add_argument("--size", type=float, required=True, help="pixels per em")
    b.add_argument("--size-y", type=float, default=None, help="vertical pixels per em")
    b.add_argument("--first", type=lambda s: int(s, 0), default=0x20)
    b.add_argument("--last", type=lambda s: int(s, 0), default=0x7E)
    b.add_argument("--out", required=True)
    r = sub.add_parser("render", help="preview text with a built font")
    r.add_argument("--font", required=True)
    r.add_argument("--text", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--scale", type=int, default=4)
    args = parser.parse_args(argv)

    if args.cmd == "build":
        chars = "".join(chr(c) for c in range(args.first, args.last + 1))
        font = build(args.ttf, args.size, chars, args.size_y)
        data = serialise(font)
        with open(args.out, "wb") as fh:
            fh.write(data)
        missing = [g["char"] for g in font["glyphs"] if g["levels"] is None and g["char"] != " "]
        vram = font["atlas_width"] * font["layer_height"] * LEVELS // 4
        print("%s: %d glyphs, atlas %dx%d (%d bytes of VRAM), %d kerning pairs, "
              "line %d px, ascent %d px, %d bytes"
              % (args.out, len(font["glyphs"]), font["atlas_width"],
                 font["layer_height"] * LEVELS, vram, len(font["kerning"]),
                 font["line_height"], font["ascent"], len(data)))
        if missing:
            print("  no outline in the TTF for: %s" % " ".join(repr(c) for c in missing))
        return 0

    from PIL import Image

    with open(args.font, "rb") as fh:
        font = BitmapFont(fh.read())
    width = font.width(args.text) + 8
    height = font.line_height + 8
    levels = font.render_levels(args.text, 4, 4, width, height)
    rgb = blend(np.zeros((height, width, 3)), levels, (0xED, 0xE9, 0xD6))
    img = Image.fromarray(rgb.clip(0, 255).astype(np.uint8))
    img.resize((width * args.scale, height * args.scale), Image.NEAREST).save(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
