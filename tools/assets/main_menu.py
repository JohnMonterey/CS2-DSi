"""Build the main menu's two screen textures from the art in Counter-Strike-nds/assets/main_menu.

Everything on the main menu except the button labels is static, so each screen is flattened
into one 256x192 paletted texture: the wallpaper, the vignette, the logo, the button strips
and the footer. The labels are drawn at runtime with the bitmap font (see dsfont.py).

Texture VRAM in Dual 3D mode is banks A and B -- 256 KB for the whole game -- and about
128 KB of it is free at the main menu, so the screens are 8bpp (49 KB each), not 16bpp
(98 KB each). The palette is fitted in RGB555, the only precision the DS has, and the image
is error-diffused against it there, so the 5-bit rounding is dithered along with the
palette error instead of banding the vignette.

Usage:
    python3 tools/assets/main_menu.py                 # write the textures into data/
    python3 tools/assets/main_menu.py --preview out/  # also write the predicted DS frame

Writes Counter-Strike-nds/data/menu_top.bin and menu_bottom.bin, each 256 little-endian
RGB555 palette entries followed by 256x192 palette indices (49,664 bytes).

The layout below must agree with MAIN_MENU_* in Counter-Strike-nds/source/graphics/ui.c.
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dsfont  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
GAME = os.path.join(ROOT, "Counter-Strike-nds")
ART = os.path.join(GAME, "assets", "main_menu")
DATA = os.path.join(GAME, "data")

W, H = 256, 192
LOGO_POS = (12, 12)                     # top screen
BUTTON_Y = [33, 66, 99, 132]            # bottom screen, strip tops; strips are 26 px tall
LABELS = ["Singleplayer", "Multiplayer", "Settings", "Quit"]
LABEL_X = 8                             # pen start; the label's line box top is its strip top
FOOTER_LEFT_POS = (9, 174)
FOOTER_RIGHT_POS = (206, 174)
TEXT_COLOR = (0xED, 0xE9, 0xD6)         # as designed; the DS shows it as RGB15(29, 28, 26)
TEXT_COLOR_DS = tuple(round(round(c * 31 / 255) * 255 / 31) for c in TEXT_COLOR)


def rgba(name):
    return np.asarray(Image.open(os.path.join(ART, name)).convert("RGBA")).astype(float)


def over(dst, src, x, y):
    h, w = src.shape[:2]
    a = src[..., 3:4] / 255.0
    dst[y:y + h, x:x + w] = src[..., :3] * a + dst[y:y + h, x:x + w] * (1 - a)


def button_strip():
    """The supplied button with its baked-in label removed.

    The strip is black with a horizontal alpha ramp, so each column's alpha is taken from
    that column's label-free (pure black) pixels.
    """
    button = rgba("button.png")
    black = button[..., :3].sum(axis=-1) < 8
    alpha = np.where(black, button[..., 3], np.nan)
    ramp = np.nanmean(alpha, axis=0)
    strip = np.zeros_like(button)
    strip[..., 3] = ramp[None, :]
    return strip


def compose():
    wallpaper = rgba("background.png")[..., :3]
    vignette = rgba("vignette.png")

    top = wallpaper[:H].copy()
    over(top, vignette, 0, 0)
    over(top, rgba("logo.png"), *LOGO_POS)

    bottom = wallpaper[H:2 * H].copy()
    over(bottom, vignette, 0, 0)
    strip = button_strip()
    for y in BUTTON_Y:
        over(bottom, strip, 0, y)
    over(bottom, rgba("footer_unofficial_demake.png"), *FOOTER_LEFT_POS)
    over(bottom, rgba("footer_version.png"), *FOOTER_RIGHT_POS)
    return top, bottom


def fit_palette(img5, colors=256, iterations=12):
    """k-means in 5-bit RGB, seeded from Pillow's median cut. Returns integer RGB555 triples.

    Centres are snapped to RGB555 every round. Dark images make many of them snap onto the
    same colour, so the freed slots go to the most common exact colours still missing.
    """
    pixels = img5.reshape(-1, 3)
    exact, counts = np.unique(np.clip(np.rint(pixels), 0, 31).astype(int), axis=0,
                              return_counts=True)
    if len(exact) <= colors:
        return exact
    by_frequency = exact[np.argsort(-counts, kind="stable")]

    seed_img = Image.fromarray((img5 * 255 / 31).round().astype(np.uint8))
    centres = np.array(seed_img.quantize(colors, method=Image.Quantize.MEDIANCUT)
                       .getpalette()[:colors * 3], float).reshape(-1, 3) * 31 / 255
    palette = None
    for _ in range(iterations):
        labels = nearest(pixels, centres)
        for k in range(len(centres)):
            members = pixels[labels == k]
            if len(members):
                centres[k] = members.mean(axis=0)
        palette = np.unique(np.clip(np.rint(centres), 0, 31).astype(int), axis=0)
        if len(palette) < colors:
            present = {tuple(c) for c in palette}
            extra = [c for c in by_frequency if tuple(c) not in present][:colors - len(palette)]
            palette = np.concatenate([palette, np.array(extra, int).reshape(-1, 3)])
        centres = palette.astype(float)
    return palette


def nearest(pixels, palette):
    out = np.empty(len(pixels), int)
    for start in range(0, len(pixels), 8192):
        chunk = pixels[start:start + 8192]
        d = ((chunk[:, None, :] - palette[None, :, :]) ** 2).sum(axis=-1)
        out[start:start + 8192] = d.argmin(axis=1)
    return out


def dither(img5, palette):
    """Serpentine Floyd-Steinberg against an exact RGB555 palette."""
    work = img5.copy()
    pal = palette.astype(float)
    out = np.zeros((H, W), np.uint8)
    for y in range(H):
        xs = range(W) if y % 2 == 0 else range(W - 1, -1, -1)
        step = 1 if y % 2 == 0 else -1
        for x in xs:
            value = np.clip(work[y, x], 0, 31)
            k = int(((pal - value) ** 2).sum(axis=1).argmin())
            out[y, x] = k
            err = value - pal[k]
            if 0 <= x + step < W:
                work[y, x + step] += err * 7 / 16
            if y + 1 < H:
                if 0 <= x - step < W:
                    work[y + 1, x - step] += err * 3 / 16
                work[y + 1, x] += err * 5 / 16
                if 0 <= x + step < W:
                    work[y + 1, x + step] += err * 1 / 16
    return out


def to_texture(img8):
    img5 = img8 * 31.0 / 255.0
    palette = fit_palette(img5)
    indices = dither(img5, palette)
    full = np.zeros((256, 3), int)
    full[:len(palette)] = palette
    rgb555 = full[:, 0] | (full[:, 1] << 5) | (full[:, 2] << 10)
    blob = rgb555.astype("<u2").tobytes() + indices.tobytes()
    shown = full[indices] * 255.0 / 31.0          # what the DS puts on screen, in 8-bit
    return blob, shown, len(palette)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build the main menu screen textures")
    parser.add_argument("--preview", help="directory for predicted-frame PNGs")
    parser.add_argument("--font", default=os.path.join(DATA, "font_cs20.bin"))
    args = parser.parse_args(argv)

    top, bottom = compose()
    screens = {}
    for name, img in (("top", top), ("bottom", bottom)):
        blob, shown, used = to_texture(img)
        path = os.path.join(DATA, "menu_%s.bin" % name)
        with open(path, "wb") as fh:
            fh.write(blob)
        err = np.abs(shown - img).mean()
        print("%s: %d colours, mean error %.2f/255 against the 8-bit composite"
              % (os.path.relpath(path, ROOT), used, err))
        screens[name] = shown

    if args.preview:
        os.makedirs(args.preview, exist_ok=True)
        with open(args.font, "rb") as fh:
            font = dsfont.BitmapFont(fh.read())
        frame_bottom = screens["bottom"]
        for label, y in zip(LABELS, BUTTON_Y):
            levels = font.render_levels(label, LABEL_X, y, W, H)
            frame_bottom = dsfont.blend(frame_bottom, levels, TEXT_COLOR_DS)
        frame = np.concatenate([screens["top"], frame_bottom]).clip(0, 255)
        Image.fromarray(frame.round().astype(np.uint8)).save(
            os.path.join(args.preview, "main_menu_predicted.png"))

        golden = np.asarray(Image.open(os.path.join(ART, "golden.png")).convert("RGB")).astype(float)
        diff = np.abs(frame - golden).mean(axis=-1)
        print("predicted DS frame vs golden: mean %.2f/255, top %.2f, bottom %.2f, "
              "%.2f%% of pixels off by more than 24"
              % (diff.mean(), diff[:H].mean(), diff[H:].mean(), (diff > 24).mean() * 100))
        Image.fromarray((np.clip(diff * 4, 0, 255)).astype(np.uint8)).save(
            os.path.join(args.preview, "main_menu_diff_x4.png"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
