#!/usr/bin/env python3
"""Draws the Windows application icons for Luxora.

  resources/windows/luxora_editor.ico  - the editor (a lit cube on a dark tile)
  resources/windows/luxora_game.ico    - default icon for games (a game controller)
  editor/app_icon.h                    - the editor icon as embedded PNGs (title bar, Linux window icon)

Each .ico holds 16..256 px images. Every size is rendered at 8x and
downsampled, so small sizes stay crisp.  Run:  python3 tools/make_icons.py
"""
import os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "windows")
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 8  # supersampling


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def gradient(size, top, bottom):
    g = Image.new("RGBA", (1, 256))
    for y in range(256):
        g.putpixel((0, y), lerp(top, bottom, y / 255.0))
    return g.resize((size, size), Image.BILINEAR)


def tile(size, top, bottom, radius=0.22, inset=0.03):
    """Rounded-square background with a vertical gradient and a soft top highlight."""
    S = size
    m = Image.new("L", (S, S), 0)
    i = int(S * inset)
    ImageDraw.Draw(m).rounded_rectangle([i, i, S - i - 1, S - i - 1], int(S * radius), fill=255)
    bg = gradient(S, top, bottom)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(bg, (0, 0), m)
    # glossy rim at the top
    hl = Image.new("L", (S, S), 0)
    ImageDraw.Draw(hl).rounded_rectangle([i, i, S - i - 1, int(S * 0.55)], int(S * radius), fill=34)
    hl = Image.composite(hl, Image.new("L", (S, S), 0), m)
    img = Image.alpha_composite(img, Image.merge("RGBA", (Image.new("L", (S, S), 255),) * 3 + (hl,)))
    return img, m


def editor_icon(S):
    img, mask = tile(S, (44, 50, 86, 255), (14, 15, 24, 255))
    d = ImageDraw.Draw(img)
    cx, cy, r = S * 0.5, S * 0.53, S * 0.29
    # isometric cube: top, left and right faces
    top = [(cx, cy - r), (cx + r * 0.866, cy - r * 0.5), (cx, cy), (cx - r * 0.866, cy - r * 0.5)]
    left = [(cx - r * 0.866, cy - r * 0.5), (cx, cy), (cx, cy + r), (cx - r * 0.866, cy + r * 0.5)]
    right = [(cx, cy), (cx + r * 0.866, cy - r * 0.5), (cx + r * 0.866, cy + r * 0.5), (cx, cy + r)]
    # warm light from the top-left: bright top, mid left, cool shadowed right
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([cx - r * 1.35, cy - r * 1.45, cx + r * 1.35, cy + r * 1.1], fill=(255, 190, 90, 90))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.07))
    glow.putalpha(Image.composite(glow.getchannel("A"), Image.new("L", (S, S), 0), mask))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    d.polygon(top, fill=(255, 214, 128, 255))
    d.polygon(left, fill=(236, 128, 64, 255))
    d.polygon(right, fill=(94, 92, 214, 255))
    # crisp edges
    w = max(1, int(S * 0.012))
    for poly in (top, left, right):
        d.line(poly + [poly[0]], fill=(255, 255, 255, 110), width=w, joint="curve")
    # photon spark top-right
    sx, sy, sr = S * 0.76, S * 0.24, S * 0.085
    d.polygon([(sx, sy - sr), (sx + sr * 0.22, sy - sr * 0.22), (sx + sr, sy), (sx + sr * 0.22, sy + sr * 0.22),
               (sx, sy + sr), (sx - sr * 0.22, sy + sr * 0.22), (sx - sr, sy), (sx - sr * 0.22, sy - sr * 0.22)],
              fill=(255, 246, 220, 255))
    return img


def game_icon(S):
    img, mask = tile(S, (40, 200, 160, 255), (18, 84, 150, 255))
    pad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(pad)
    white = (250, 252, 255, 255)
    # controller body: a wide rounded bar plus two grips
    bx0, by0, bx1, by1 = S * 0.14, S * 0.33, S * 0.86, S * 0.62
    d.rounded_rectangle([bx0, by0, bx1, by1], int(S * 0.13), fill=white)
    gr = S * 0.135
    for gx in (S * 0.27, S * 0.73):
        d.ellipse([gx - gr, S * 0.47 - gr, gx + gr, S * 0.72 - gr * 0.2], fill=white)
    # cut-outs: d-pad, four face buttons
    cut = (0, 0, 0, 0)
    dx, dy, a, b = S * 0.30, S * 0.48, S * 0.105, S * 0.034
    d.rectangle([dx - a, dy - b, dx + a, dy + b], fill=cut)
    d.rectangle([dx - b, dy - a, dx + b, dy + a], fill=cut)
    fx, fy, o, br = S * 0.70, S * 0.48, S * 0.068, S * 0.036
    for ox, oy in ((0, -o), (o, 0), (0, o), (-o, 0)):
        d.ellipse([fx + ox - br, fy + oy - br, fx + ox + br, fy + oy + br], fill=cut)
    # soft drop shadow under the controller
    sh = pad.getchannel("A").filter(ImageFilter.GaussianBlur(S * 0.025))
    shadow = Image.new("RGBA", (S, S), (0, 20, 40, 0))
    shadow.putalpha(sh.point(lambda v: v * 0.45))
    shadow = shadow.transform((S, S), Image.AFFINE, (1, 0, 0, 0, 1, -S * 0.025))
    img = Image.alpha_composite(img, shadow)
    return Image.alpha_composite(img, pad)


def render(fn, size):
    big = fn(size * SS)
    return big.resize((size, size), Image.LANCZOS)


def save(fn, name):
    os.makedirs(OUT, exist_ok=True)
    images = [render(fn, s) for s in SIZES]
    big = images[-1]
    big.save(os.path.join(OUT, name + ".ico"), format="ICO", sizes=[(s, s) for s in SIZES],
             append_images=images[:-1])
    big.save(os.path.join(OUT, name + ".png"))
    print("wrote", os.path.join(OUT, name + ".ico"))


def embed_editor_icon():
    """64 px for the title bar (crisp at 2x UI scale) and 32 px for small uses."""
    import io
    out = ["/* Generated by tools/make_icons.py - do not edit. The Luxora editor icon as PNG. */",
           "#ifndef LX_APP_ICON_H", "#define LX_APP_ICON_H", ""]
    for size in (64, 32):
        b = io.BytesIO()
        render(editor_icon, size).save(b, "PNG", optimize=True)
        data = b.getvalue()
        out.append(f"static const unsigned char lx_app_icon_{size}_png[{len(data)}] = {{")
        for i in range(0, len(data), 20):
            out.append("    " + ", ".join(str(x) for x in data[i:i + 20]) + ",")
        out.append("};")
    out.append("#endif")
    path = os.path.join(ROOT, "editor", "app_icon.h")
    open(path, "w").write("\n".join(out) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    save(editor_icon, "luxora_editor")
    save(game_icon, "luxora_game")
    embed_editor_icon()
