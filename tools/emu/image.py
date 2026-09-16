"""Minimal BMP reading and image diffing, stdlib only.

The harness deliberately has no Pillow/numpy dependency: agents run it on whatever
Python happens to be on the machine, and a missing wheel is a failure mode that looks
exactly like a real test failure. `sips` (built into macOS) normalises captures to a
fixed-size BMP and this module compares them.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


@dataclass
class Image:
    width: int
    height: int
    pixels: bytearray  # RGB, row-major, top-down, 3 bytes per pixel

    def pixel(self, x: int, y: int):
        i = (y * self.width + x) * 3
        return self.pixels[i], self.pixels[i + 1], self.pixels[i + 2]


def read_bmp(path: str) -> Image:
    with open(path, "rb") as fh:
        data = fh.read()
    return parse_bmp(data)


def parse_bmp(data: bytes) -> Image:
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP file")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    header_size = struct.unpack_from("<I", data, 14)[0]
    if header_size < 40:
        raise ValueError("unsupported BMP header size %d" % header_size)
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    if bpp not in (24, 32):
        raise ValueError("unsupported BMP depth %d" % bpp)
    if compression not in (0, 3):
        raise ValueError("unsupported BMP compression %d" % compression)

    bottom_up = height > 0
    height = abs(height)
    stride = ((width * bpp + 31) // 32) * 4
    need = pixel_offset + stride * height
    if len(data) < need:
        raise ValueError("BMP truncated: need %d bytes, have %d" % (need, len(data)))

    step = bpp // 8
    out = bytearray(width * height * 3)
    for row in range(height):
        src_row = (height - 1 - row) if bottom_up else row
        base = pixel_offset + src_row * stride
        dst = row * width * 3
        for col in range(width):
            s = base + col * step
            # BMP stores BGR(A)
            out[dst] = data[s + 2]
            out[dst + 1] = data[s + 1]
            out[dst + 2] = data[s]
            dst += 3
    return Image(width, height, out)


def write_bmp(img: Image, path: str) -> None:
    stride = ((img.width * 24 + 31) // 32) * 4
    pad = stride - img.width * 3
    body = bytearray()
    for row in range(img.height - 1, -1, -1):  # bottom-up
        base = row * img.width * 3
        for col in range(img.width):
            i = base + col * 3
            body += bytes((img.pixels[i + 2], img.pixels[i + 1], img.pixels[i]))
        body += b"\x00" * pad
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(body), 0, 0, 14 + 40)
    dib = struct.pack(
        "<IiiHHIIiiII", 40, img.width, img.height, 1, 24, 0, len(body), 2835, 2835, 0, 0
    )
    with open(path, "wb") as fh:
        fh.write(header)
        fh.write(dib)
        fh.write(body)


@dataclass
class DiffResult:
    mean_abs: float          # mean absolute channel difference, 0-255
    changed_fraction: float  # fraction of pixels differing by more than `threshold`
    width: int
    height: int

    def passed(self, max_mean: float, max_changed: float) -> bool:
        return self.mean_abs <= max_mean and self.changed_fraction <= max_changed


def diff(a: Image, b: Image, threshold: int = 12, diff_path: str = None) -> DiffResult:
    """Compare two images of identical dimensions.

    `threshold` is the per-channel tolerance below which a pixel counts as unchanged;
    it absorbs the dithering and scaling noise you get from resampling a captured
    window, which is why the comparison is tolerant rather than exact.
    """
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(
            "size mismatch: %dx%d vs %dx%d" % (a.width, a.height, b.width, b.height)
        )
    total = a.width * a.height
    acc = 0
    changed = 0
    marked = bytearray(b.pixels) if diff_path else None
    for i in range(0, total * 3, 3):
        dr = abs(a.pixels[i] - b.pixels[i])
        dg = abs(a.pixels[i + 1] - b.pixels[i + 1])
        db = abs(a.pixels[i + 2] - b.pixels[i + 2])
        acc += dr + dg + db
        if max(dr, dg, db) > threshold:
            changed += 1
            if marked is not None:
                marked[i] = 255
                marked[i + 1] = 0
                marked[i + 2] = 255
    if marked is not None:
        write_bmp(Image(a.width, a.height, marked), diff_path)
    return DiffResult(acc / (total * 3.0), changed / float(total), a.width, a.height)
