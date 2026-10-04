# PS4 app icon (sce_sys/icon0.png, 512x512 opaque): the silhouette of Dolphin's own logo
# (Data/dolphin-emu.svg) filled with a pink-to-blue gradient, on black.
import io, os, sys
import cairosvg
from PIL import Image, ImageChops, ImageDraw, ImageFilter

svg = os.path.expanduser("~/dolphinps4-build/src/dolphin/Data/dolphin-emu.svg")
out = sys.argv[1]
S = 512
LOGO = 400
logo = Image.open(io.BytesIO(cairosvg.svg2png(url=svg, output_width=LOGO, output_height=LOGO))).convert("RGBA")
mask = logo.split()[3]

# Diagonal gradient across the logo: pink (top left) -> lilac -> sky blue (bottom right).
stops = [(0.0, (255, 110, 205)), (0.45, (185, 140, 255)), (1.0, (80, 205, 255))]
def colour(t):
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0)
            return tuple(int(c0[i] + (c1[i] - c0[i]) * f) for i in range(3))
    return stops[-1][1]
grad = Image.new("RGB", (LOGO, LOGO))
px = grad.load()
for y in range(LOGO):
    for x in range(LOGO):
        px[x, y] = colour((x + y) / (2 * (LOGO - 1)))
fill = grad.convert("RGBA")
fill.putalpha(mask)

img = Image.new("RGBA", (S, S), (0, 0, 0, 255))
pos = ((S - LOGO) // 2, (S - LOGO) // 2)
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
glow.paste((150, 120, 255, 255), pos, mask.point(lambda a: a * 0.30))
img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(14)))
img.alpha_composite(fill, pos)
img.convert("RGB").save(out)
print("wrote", out, img.size)
