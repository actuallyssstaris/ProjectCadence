#!/usr/bin/env python3
# ProjectCadence icon: a cyan crescent with a note riding it and a pink orbit
# around both. Writes sce_sys/icon0.png (512x512) and docs/logo.png. Needs pillow.
import os
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 512
K = 4  # drawn big, scaled down: cheap anti-aliasing
N = S * K

CYAN_LIGHT = (130, 232, 240)
CYAN_DARK = (24, 176, 204)
PINK = (255, 120, 160)


def gradient(size, top, bottom, angle_deg=0):
    """vertical gradient, then rotated, as an RGBA image"""
    g = Image.new("RGBA", (1, 256))
    for y in range(256):
        t = y / 255
        g.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    g = g.resize(size, Image.BILINEAR)
    return g.rotate(angle_deg, resample=Image.BICUBIC, expand=False) if angle_deg else g


def mask_ellipse(box):
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).ellipse(box, fill=255)
    return m


def fill(base, mask, colour_img):
    base.paste(colour_img, (0, 0), mask)


def ring_mask(cx, cy, rx, ry, width, angle):
    m = Image.new("L", (N, N), 0)
    ImageDraw.Draw(m).ellipse((cx - rx, cy - ry, cx + rx, cy + ry), outline=255, width=width)
    return m.rotate(angle, center=(cx, cy), resample=Image.BICUBIC)


def note_mask():
    """eighth note: round head bottom left, stem, a flag sweeping right"""
    m = Image.new("L", (N, N), 0)
    d = ImageDraw.Draw(m)
    hx, hy, hr = N * 0.43, N * 0.66, N * 0.095
    d.ellipse((hx - hr, hy - hr, hx + hr, hy + hr), fill=255)
    sx0, sx1 = hx + hr * 0.55, hx + hr * 0.98
    top = N * 0.24
    d.polygon([(sx0, hy - hr * 0.2), (sx0, top + N * 0.02), (sx1, top), (sx1, hy - hr * 0.3)], fill=255)

    # flag: a fat curve from the stem top out and down, its inner edge a smaller curve
    def bez(p0, p1, p2, p3, n=48):
        pts = []
        for i in range(n + 1):
            t = i / n
            u = 1 - t
            pts.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                        u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
        return pts

    outer = bez((sx1, top), (N * 0.66, N * 0.28), (N * 0.78, N * 0.36), (N * 0.72, N * 0.52))
    inner = bez((N * 0.72, N * 0.52), (N * 0.70, N * 0.43), (N * 0.62, N * 0.39), (sx1, N * 0.40))
    d.polygon(outer + inner, fill=255)
    return m


def shadow(img, mask, blur):
    sh = mask.filter(ImageFilter.GaussianBlur(N * blur)).point(lambda v: int(v * 0.35))
    img.paste((0, 0, 0, 255), (int(N * 0.01), int(N * 0.015)), sh)


def draw():
    img = Image.new("RGBA", (N, N))
    img.paste(gradient((N, N), (16, 20, 38), (30, 36, 66)), (0, 0))

    cx, cy = N * 0.5, N * 0.52
    orbit = ring_mask(cx, cy, N * 0.44, N * 0.15, int(N * 0.045), 18)
    pink = gradient((N, N), (255, 150, 185), PINK)

    # orbit behind everything first
    fill(img, orbit, pink)

    # crescent: a disc with a second, offset disc cut out of it
    r = N * 0.36
    disc = mask_ellipse((cx - r, cy - r, cx + r, cy + r))
    cut_r = r * 0.86
    ox, oy = cx + r * 0.30, cy - r * 0.16
    crescent = ImageChops.subtract(disc, mask_ellipse((ox - cut_r, oy - cut_r, ox + cut_r, oy + cut_r)))
    cyan = gradient((N, N), CYAN_LIGHT, CYAN_DARK, angle_deg=-25)
    shadow(img, crescent, 0.02)
    fill(img, crescent, cyan)

    # the near side of the orbit passes in front of the crescent's lower left...
    front = Image.new("L", (N, N), 0)
    ImageDraw.Draw(front).rectangle((0, cy, cx * 0.95, N), fill=255)
    fill(img, ImageChops.multiply(orbit, front), pink)

    # ...and behind the note, which sits on top of everything
    note = note_mask()
    shadow(img, note, 0.015)
    fill(img, note, cyan)

    return img.resize((S, S), Image.LANCZOS)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png"
    icon = draw()
    icon.convert("RGB").save(out)
    os.makedirs("docs", exist_ok=True)
    icon.save("docs/logo.png")
    print("wrote", out, "and docs/logo.png")
