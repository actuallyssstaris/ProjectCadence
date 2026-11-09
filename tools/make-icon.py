#!/usr/bin/env python3
# sce_sys/icon0.png, 512x512. Needs pillow.
import sys
from PIL import Image, ImageDraw, ImageFont

S = 512
out = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png"

# draw at 2x then downscale, poor man's AA
k = 2
img = Image.new("RGBA", (S * k, S * k), (34, 34, 42, 255))
d = ImageDraw.Draw(img)
c = S * k // 2
r = int(S * k * 0.40)
d.ellipse((c - r, c - r, c + r, c + r), fill=(255, 102, 170, 255))
w = int(S * k * 0.035)
r2 = r - int(S * k * 0.03)
d.ellipse((c - r2, c - r2, c + r2, c + r2), outline=(255, 255, 255, 255), width=w)

font = None
for p in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
    try:
        font = ImageFont.truetype(p, int(S * k * 0.22))
        break
    except OSError:
        pass
if font is None:
    font = ImageFont.load_default()

d.text((c, c), "osu!", font=font, fill=(255, 255, 255, 255), anchor="mm")
img.resize((S, S), Image.LANCZOS).save(out)
print("wrote", out)
