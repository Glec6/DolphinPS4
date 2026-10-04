# XMB start-up animation images (xmb/intro-dolphin.png, xmb/intro-title.png) from the app's
# artwork (sce_sys/icon-source.jpg, drawn on black): the dolphin and the title are cut apart at
# the empty band between them, and black becomes transparent (alpha = brightest channel, colour
# un-premultiplied) so the glow blends over anything.
#   make-intro.py [source]   (needs Pillow)
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
source = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "sce_sys", "icon-source.jpg")
out_dir = os.path.join(HERE, "..", "xmb")
art = Image.open(source).convert("RGB")
W, H = art.size
px = art.load()


def row_level(y):
    return max(max(px[x, y]) for x in range(0, W, 2))


# The empty band: the longest run of dark rows in the middle of the picture.
dark = [row_level(y) < 18 for y in range(H)]
best, run_start, split = 0, None, H // 2
for y in range(H // 4, H * 3 // 4 + 1):
    if y < H and dark[y]:
        run_start = y if run_start is None else run_start
    else:
        if run_start is not None and y - run_start > best:
            best, split = y - run_start, (run_start + y) // 2
        run_start = None


def cut(top, bottom, name):
    part = art.crop((0, top, W, bottom))
    rgba = Image.new("RGBA", part.size)
    src, dst = part.load(), rgba.load()
    for y in range(part.height):
        for x in range(part.width):
            r, g, b = src[x, y]
            a = max(r, g, b)
            if a < 6:
                dst[x, y] = (0, 0, 0, 0)
            else:
                dst[x, y] = (min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a), a)
    box = rgba.getbbox()
    rgba = rgba.crop((max(0, box[0] - 8), max(0, box[1] - 8), min(rgba.width, box[2] + 8),
                      min(rgba.height, box[3] + 8)))
    path = os.path.join(out_dir, name)
    rgba.save(path)
    print("wrote", path, rgba.size)


cut(0, split, "intro-dolphin.png")
cut(split, H, "intro-title.png")
