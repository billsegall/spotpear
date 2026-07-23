#!/usr/bin/env python3
"""Convert an image to the raw RGB565 blob the app embeds.

    ./tools/png_to_rgb565.py photo.jpg main/image.rgb565

Output is 240*240*2 = 115200 bytes, no header. Pixels are packed RGB565
**high byte first**: the GC9A01 clocks the most significant byte out first, so
writing native little-endian uint16 would swap the bytes and scramble colour.
Anything else that generates image.rgb565 must use the same convention --
see to_rgb565_be(), which make_test_pattern.py shares.

The panel is round, so the image is scaled to cover 240x240 and centre-cropped;
the corners fall outside the visible circle anyway.
"""

import sys

from PIL import Image

WIDTH = 240
HEIGHT = 240


def to_rgb565_be(img):
    """Pack a 24-bit RGB PIL image into big-endian RGB565 bytes."""
    img = img.convert("RGB")
    out = bytearray()
    for r, g, b in img.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += bytes((value >> 8, value & 0xFF))  # high byte first
    return bytes(out)


def cover_crop(img, width=WIDTH, height=HEIGHT):
    """Scale to cover the target box, then centre-crop to it."""
    scale = max(width / img.width, height / img.height)
    resized = img.resize(
        (max(width, round(img.width * scale)), max(height, round(img.height * scale))),
        Image.LANCZOS,
    )
    left = (resized.width - width) // 2
    top = (resized.height - height) // 2
    return resized.crop((left, top, left + width, top + height))


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    src, dst = argv[1], argv[2]
    img = cover_crop(Image.open(src))
    data = to_rgb565_be(img)

    expected = WIDTH * HEIGHT * 2
    assert len(data) == expected, f"packed {len(data)} bytes, expected {expected}"

    with open(dst, "wb") as fh:
        fh.write(data)
    print(f"{src} -> {dst}: {len(data)} bytes ({WIDTH}x{HEIGHT} RGB565, big-endian)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
