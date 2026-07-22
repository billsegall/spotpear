#!/usr/bin/env python3
"""Generate the diagnostic placeholder image.

    ./tools/make_test_pattern.py main/image.rgb565

Nobody driving this app over serial can see the panel, so the default image is
built to be diagnosed at a glance rather than to look nice:

  * three horizontal bands -- pure RED (top), GREEN (middle), BLUE (bottom)
  * a WHITE square, off-centre and high-left, breaking every symmetry

Read the panel against that:

  bands are blue/green/red top-to-bottom  -> image is flipped vertically,
                                             toggle LCD_MIRROR_Y
  white square appears on the right       -> flipped horizontally,
                                             toggle LCD_MIRROR_X
  top band blue and bottom band red       -> R and B swapped, switch
                                             LCD_COLOR_ELEMENT_ORDER to RGB
  colours are muddy noise, not 3 bands    -> byte order wrong, check
                                             to_rgb565_be()
  bands present but colours look negative -> toggle LCD_INVERT_COLOR

Packing goes through the same to_rgb565_be() the real converter uses, so the
two can never disagree about byte order.
"""

import sys

from PIL import Image, ImageDraw

from png_to_rgb565 import HEIGHT, WIDTH, to_rgb565_be

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)

# Kept well inside the visible circle (centre 120,120 radius 120): the far
# corners of the square sit ~100px from centre.
MARK_BOX = (40, 20, 80, 60)


def build():
    img = Image.new("RGB", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(img)
    third = HEIGHT // 3
    draw.rectangle((0, 0, WIDTH, third), fill=RED)
    draw.rectangle((0, third, WIDTH, 2 * third), fill=GREEN)
    draw.rectangle((0, 2 * third, WIDTH, HEIGHT), fill=BLUE)
    draw.rectangle(MARK_BOX, fill=WHITE)
    return img


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    data = to_rgb565_be(build())
    expected = WIDTH * HEIGHT * 2
    assert len(data) == expected, f"packed {len(data)} bytes, expected {expected}"

    with open(argv[1], "wb") as fh:
        fh.write(data)
    print(f"wrote {argv[1]}: {len(data)} bytes (diagnostic pattern)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
