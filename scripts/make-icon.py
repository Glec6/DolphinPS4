# PS4 app icon (sce_sys/icon0.png, 512x512 opaque): the silhouette of Dolphin's own logo
# (Data/dolphin-emu.svg) with a cyan-to-pink gradient and a soft glow on black, "DOLPHIN
# EMULATOR" below it and a small gamepad + "PS4" tag.
#   make-icon.py <out.png>   (needs cairosvg and Pillow; fonts from xmb/fonts)
import io, os, sys
import cairosvg
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
svg = os.path.expanduser("~/dolphinps4-build/src/dolphin/Data/dolphin-emu.svg")
font_dir = os.path.join(HERE, "..", "xmb", "fonts")
out = sys.argv[1]
S = 512

# The dolphin: the logo's shape (head on the left), trimmed to its outline.
LOGO = 330
logo = Image.open(io.BytesIO(cairosvg.svg2png(url=svg, output_width=LOGO, output_height=LOGO))).convert("RGBA")
mask = logo.split()[3]
bbox = mask.getbbox()
mask = mask.crop(bbox)
w, h = mask.size

# Horizontal gradient: cyan head (left) -> lilac -> pink tail (right).
stops = [(0.0, (120, 232, 245)), (0.45, (205, 190, 245)), (1.0, (248, 182, 214))]
def colour(t):
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            f = (t - t0) / (t1 - t0)
            return tuple(int(c0[i] + (c1[i] - c0[i]) * f) for i in range(3))
    return stops[-1][1]
grad = Image.new("RGB", (w, h))
px = grad.load()
for x in range(w):
    c = colour(x / max(1, w - 1))
    for y in range(h):
        px[x, y] = c
fill = grad.convert("RGBA")
fill.putalpha(mask)

img = Image.new("RGBA", (S, S), (0, 0, 0, 255))
pos = ((S - w) // 2, 92)
# Glow: a blurred, brightened copy of the dolphin under it.
glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
glow.paste(fill, pos, fill)
glow = glow.filter(ImageFilter.GaussianBlur(16))
glow_alpha = glow.split()[3].point(lambda a: min(255, int(a * 0.9)))
glow.putalpha(glow_alpha)
img.alpha_composite(glow)
img.alpha_composite(fill, pos)

d = ImageDraw.Draw(img)
text_colour = (232, 232, 236, 255)
title_font = ImageFont.truetype(os.path.join(font_dir, "Nunito-Regular.ttf"), 38)
title = "DOLPHIN EMULATOR"
# Slight letter spacing.
spacing = 2
widths = [d.textlength(ch, font=title_font) for ch in title]
total = sum(widths) + spacing * (len(title) - 1)
x = (S - total) / 2
y = 346
for ch, cw in zip(title, widths):
    d.text((x, y), ch, font=title_font, fill=text_colour)
    x += cw + spacing

# Tag: a generic gamepad outline and "PS4", right-aligned under the title.
tag_font = ImageFont.truetype(os.path.join(font_dir, "Nunito-Regular.ttf"), 27)
tag = "PS4"
tw = d.textlength(tag, font=tag_font)
right = (S + total) / 2
ty = 398
tx = right - tw
d.text((tx, ty), tag, font=tag_font, fill=(190, 190, 196, 255))
# Gamepad: rounded body with two grips, a d-pad cross and two buttons.
gx1 = tx - 14
gx0 = gx1 - 46
gy0, gy1 = ty + 8, ty + 32
pad = (190, 190, 196, 255)
d.rounded_rectangle((gx0, gy0, gx1, gy1 - 6), radius=8, outline=pad, width=3)
d.ellipse((gx0 - 2, gy0 + 6, gx0 + 14, gy1 + 2), outline=pad, width=3)
d.ellipse((gx1 - 14, gy0 + 6, gx1 + 2, gy1 + 2), outline=pad, width=3)
cx, cy = gx0 + 13, gy0 + 9
d.line((cx - 5, cy, cx + 5, cy), fill=pad, width=3)
d.line((cx, cy - 5, cx, cy + 5), fill=pad, width=3)
d.ellipse((gx1 - 17, gy0 + 4, gx1 - 11, gy0 + 10), fill=pad)
d.ellipse((gx1 - 10, gy0 + 9, gx1 - 4, gy0 + 15), fill=pad)

img.convert("RGB").save(out)
print("wrote", out, img.size)
