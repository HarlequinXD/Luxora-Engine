#!/usr/bin/env python3
"""Draws the Windows application icons for Luxora.

  resources/windows/luxora_editor.ico  - the Luxora editor (an ember flame on an obsidian tile)
  resources/windows/luxora_game.ico    - default icon for games (a game controller)
  editor/app_icon.h                    - the editor icon (title bar, Linux window icon) and the default game icon, as PNGs
  android/app/src/main/res/mipmap-*    - default Android launcher icon for games

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


def tile(size, top, bottom, radius=0.22, inset=0.03, gloss=34, gloss_rgb=(255, 255, 255)):
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
    ImageDraw.Draw(hl).rounded_rectangle([i, i, S - i - 1, int(S * 0.55)], int(S * radius), fill=gloss)
    hl = Image.composite(hl, Image.new("L", (S, S), 0), m)
    img = Image.alpha_composite(img, Image.merge("RGBA", tuple(Image.new("L", (S, S), c) for c in gloss_rgb) + (hl,)))
    return img, m


def flame_points(cx, base, h, w, lean=0.0, n=120):
    """A flame outline: round belly at the bottom, sides curling in to a leaning tip."""
    import math
    pts = []
    for k in range(n + 1):
        t = k / n  # 0 = tip, going clockwise down the right side, round the base, up the left
        a = t * 2 * math.pi
        # teardrop: x = sin(a) * width profile, y from tip (0) to base (1)
        y = (1 - math.cos(a)) / 2                      # 0 at tip, 1 at bottom, back to 0
        prof = math.sin(a) * (y ** 0.75) * (1.0 - 0.18 * math.sin(a * 3) * (1 - y))
        x = cx + prof * w + lean * (1 - y) ** 2 * w
        pts.append((x, base - h + y * h))
    return pts


def editor_icon(S):
    """Luxora: an ember flame on an obsidian tile."""
    img, mask = tile(S, (62, 18, 12, 255), (10, 6, 6, 255), gloss=16, gloss_rgb=(255, 140, 90))
    # heat glow behind the flame
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([S * 0.18, S * 0.30, S * 0.82, S * 0.98], fill=(255, 70, 20, 150))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.09))
    glow.putalpha(Image.composite(glow.getchannel("A"), Image.new("L", (S, S), 0), mask))
    img = Image.alpha_composite(img, glow)

    def layer(points, top, bottom):
        m = Image.new("L", (S, S), 0)
        ImageDraw.Draw(m).polygon(points, fill=255)
        col = gradient(S, top, bottom)
        out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        out.paste(col, (0, 0), m)
        return out

    cx, base = S * 0.5, S * 0.88
    outer = layer(flame_points(cx, base, S * 0.74, S * 0.29, lean=0.35), (255, 150, 40, 255), (200, 24, 12, 255))
    mid = layer(flame_points(cx + S * 0.01, base - S * 0.02, S * 0.50, S * 0.19, lean=-0.25), (255, 214, 90, 255), (255, 92, 24, 255))
    core = layer(flame_points(cx, base - S * 0.04, S * 0.30, S * 0.10, lean=0.2), (255, 252, 225, 255), (255, 190, 70, 255))
    for l in (outer, mid, core):
        img = Image.alpha_composite(img, l)
    # embers
    d = ImageDraw.Draw(img)
    for ex, ey, er in ((0.27, 0.30, 0.022), (0.74, 0.22, 0.018), (0.80, 0.40, 0.014)):
        d.ellipse([S * (ex - er), S * (ey - er), S * (ex + er), S * (ey + er)], fill=(255, 150, 60, 230))
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
    for name, fn, size in (("app_icon_64", editor_icon, 64), ("app_icon_32", editor_icon, 32), ("game_icon_64", game_icon, 64)):
        b = io.BytesIO()
        render(fn, size).save(b, "PNG", optimize=True)
        data = b.getvalue()
        out.append(f"static const unsigned char lx_{name}_png[{len(data)}] = {{")
        for i in range(0, len(data), 20):
            out.append("    " + ", ".join(str(x) for x in data[i:i + 20]) + ",")
        out.append("};")
    out.append("#endif")
    path = os.path.join(ROOT, "editor", "app_icon.h")
    open(path, "w").write("\n".join(out) + "\n")
    print("wrote", path)


def android_launcher_icons():
    """Default launcher icon for Android games (the editor replaces these with the game's own icon)."""
    for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(ROOT, "android", "app", "src", "main", "res", "mipmap-" + density)
        os.makedirs(d, exist_ok=True)
        render(game_icon, size).save(os.path.join(d, "ic_launcher.png"), optimize=True)
    print("wrote android launcher icons")


if __name__ == "__main__":
    save(editor_icon, "luxora_editor")
    save(game_icon, "luxora_game")
    embed_editor_icon()
    android_launcher_icons()
