# PS4 app icon (sce_sys/icon0.png, 512x512 opaque) from the artwork in sce_sys/icon-source.jpg:
# scaled to the icon's width (a small margin for the tile's rounded corners) and centred on
# black, the artwork's own background.
#   make-icon.py [source] <out.png>   (needs Pillow)
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
source = sys.argv[1] if len(sys.argv) > 2 else os.path.join(HERE, "..", "sce_sys", "icon-source.jpg")
out = sys.argv[-1]
S, MARGIN = 512, 12
art = Image.open(source).convert("RGB")
w = S - 2 * MARGIN
h = round(art.height * w / art.width)
icon = Image.new("RGB", (S, S), (0, 0, 0))
icon.paste(art.resize((w, h), Image.LANCZOS), (MARGIN, (S - h) // 2))
icon.save(out)
print("wrote", out, icon.size)
