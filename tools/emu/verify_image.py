"""Cross-check tools/emu/image.py against Pillow-produced BMPs.

Run in the cloud container only; it exists to prove the stdlib BMP reader agrees with
a real decoder before the harness ships to a machine where nothing can be tested.
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PIL import Image as PILImage

from emu import image as imglib


def make_reference(w, h, seed=0):
    px = []
    for y in range(h):
        for x in range(w):
            px.append((((x * 7 + seed) % 256), ((y * 5 + seed) % 256), ((x + y) % 256)))
    im = PILImage.new("RGB", (w, h))
    im.putdata(px)
    return im


def to_bottom_up_32(im):
    """Hand-build a 32bpp BI_BITFIELDS bottom-up BMP: sips can emit this shape."""
    w, h = im.size
    px = im.load()
    stride = w * 4
    body = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b = px[x, y]
            body += bytes((b, g, r, 255))
    offset = 14 + 108
    header = struct.pack("<2sIHHI", b"BM", offset + len(body), 0, 0, offset)
    dib = bytearray(struct.pack(
        "<IiiHHIIiiII", 108, w, h, 1, 32, 3, len(body), 2835, 2835, 0, 0))
    dib += struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    dib += b"\x00" * (108 - len(dib))
    return bytes(header) + bytes(dib) + bytes(body)


def to_top_down_24(im):
    w, h = im.size
    px = im.load()
    stride = ((w * 24 + 31) // 32) * 4
    pad = stride - w * 3
    body = bytearray()
    for y in range(h):  # top-down
        for x in range(w):
            r, g, b = px[x, y]
            body += bytes((b, g, r))
        body += b"\x00" * pad
    offset = 14 + 40
    header = struct.pack("<2sIHHI", b"BM", offset + len(body), 0, 0, offset)
    dib = struct.pack(
        "<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    return bytes(header) + dib + bytes(body)


def check(name, ok):
    print(("PASS  " if ok else "FAIL  ") + name)
    return ok


def main():
    w, h = 256, 384
    ref = make_reference(w, h)
    expected = list(ref.getdata())
    all_ok = True

    tmp = tempfile.mkdtemp()

    # 1. Pillow-written BMP (24bpp bottom-up, the canonical shape)
    p = os.path.join(tmp, "pillow.bmp")
    ref.save(p)
    got = imglib.read_bmp(p)
    ok = got.width == w and got.height == h and [got.pixel(x, y) for y in range(h) for x in range(w)] == expected
    all_ok &= check("24bpp bottom-up written by Pillow", ok)

    # 2. 32bpp BI_BITFIELDS bottom-up
    p = os.path.join(tmp, "b32.bmp")
    open(p, "wb").write(to_bottom_up_32(ref))
    got = imglib.read_bmp(p)
    ok = [got.pixel(x, y) for y in range(h) for x in range(w)] == expected
    all_ok &= check("32bpp BI_BITFIELDS bottom-up", ok)
    # and Pillow agrees it is a valid file
    all_ok &= check("32bpp file decodes under Pillow too",
                    PILImage.open(p).convert("RGB").size == (w, h))

    # 3. 24bpp top-down (negative height)
    p = os.path.join(tmp, "td24.bmp")
    open(p, "wb").write(to_top_down_24(ref))
    got = imglib.read_bmp(p)
    ok = [got.pixel(x, y) for y in range(h) for x in range(w)] == expected
    all_ok &= check("24bpp top-down (negative height)", ok)

    # 4. round-trip through our own writer
    p = os.path.join(tmp, "rt.bmp")
    imglib.write_bmp(got, p)
    rt = imglib.read_bmp(p)
    all_ok &= check("write_bmp round-trips", rt.pixels == got.pixels)
    all_ok &= check("write_bmp output readable by Pillow",
                    list(PILImage.open(p).convert("RGB").getdata()) == expected)

    # 5. diff: identical images
    a = imglib.read_bmp(os.path.join(tmp, "pillow.bmp"))
    b = imglib.read_bmp(os.path.join(tmp, "pillow.bmp"))
    r = imglib.diff(a, b)
    all_ok &= check("identical images -> zero diff",
                    r.mean_abs == 0.0 and r.changed_fraction == 0.0)

    # 6. diff: one known changed block
    shifted = bytearray(b.pixels)
    for y in range(10):
        for x in range(10):
            i = (y * w + x) * 3
            shifted[i] = (shifted[i] + 200) % 256
    r = imglib.diff(a, imglib.Image(w, h, shifted),
                    diff_path=os.path.join(tmp, "d.bmp"))
    expect_fraction = 100.0 / (w * h)
    all_ok &= check("100 changed pixels detected (%.6f vs %.6f)"
                    % (r.changed_fraction, expect_fraction),
                    abs(r.changed_fraction - expect_fraction) < 1e-9)
    all_ok &= check("diff image written and readable",
                    imglib.read_bmp(os.path.join(tmp, "d.bmp")).width == w)

    # 7. diff: sub-threshold noise is tolerated
    noisy = bytearray(b.pixels)
    for i in range(0, len(noisy), 3):
        noisy[i] = min(255, noisy[i] + 5)
    r = imglib.diff(a, imglib.Image(w, h, noisy), threshold=12)
    all_ok &= check("small noise below threshold -> 0 changed pixels",
                    r.changed_fraction == 0.0 and r.mean_abs > 0)

    # 8. size mismatch is an error, not a silent pass
    try:
        imglib.diff(a, imglib.Image(2, 2, bytearray(12)))
        all_ok &= check("size mismatch raises", False)
    except ValueError:
        all_ok &= check("size mismatch raises", True)

    print("\n" + ("ALL CHECKS PASSED" if all_ok else "SOME CHECKS FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
