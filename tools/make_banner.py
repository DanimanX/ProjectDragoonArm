#!/usr/bin/env python3
"""make_banner.py - build assets/projectdragoonarm-banner.{png,jpg}.

The README hero image, generated in the same visual language as ProjectDural's
banner (1376x768: dark technical backdrop, chrome title with tracking, glow and
hard outline, letterspaced cyan detail lines, key art blended in on the right,
hardware spec strip along the bottom).

Key art: assets/source/ref-01.jpg — the Panzer Dragoon Saga promotional
illustration. Panzer Dragoon Saga and its artwork are Sega's; it appears here
only to identify the game this port runs. No game data is in this repository.

Rebuild after a copy change:

    python3 tools/make_banner.py
    python3 tools/make_banner.py --check          # layout metrics only
    python3 tools/make_banner.py --title "PROJECT DRAGOON ARM"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "source" / "ref-01.jpg"
OUT_PNG = ROOT / "assets" / "projectdragoonarm-banner.png"
OUT_JPG = ROOT / "assets" / "projectdragoonarm-banner.jpg"

W, H = 1376, 768
SS = 2  # supersample factor

FONT_DIR = Path("/usr/share/fonts/TTF")
FONT_TITLE = FONT_DIR / "FiraSansCondensed-Heavy.ttf"
FONT_SUB = FONT_DIR / "FiraSansCondensed-Bold.ttf"
FONT_TAG = FONT_DIR / "FiraSans-Medium.ttf"
FONT_SPEC = FONT_DIR / "FiraSans-SemiBold.ttf"

INK_TOP = (4, 6, 12)
INK_BOTTOM = (9, 16, 30)
CYAN = (86, 224, 255)

# Chrome ramp for the title: white highlight, cool mid, dark band, second
# reflective band, deep base. The dark band near 0.55 is what reads as chrome.
CHROME_STOPS = [
    (0.00, (255, 255, 255)),
    (0.09, (255, 255, 255)),
    (0.24, (206, 244, 255)),
    (0.36, (124, 208, 232)),
    (0.48, (66, 156, 190)),
    (0.55, (38, 104, 134)),
    (0.68, (96, 182, 208)),
    (0.80, (40, 104, 134)),
    (0.92, (14, 38, 56)),
    (1.00, (8, 20, 32)),
]


def vgradient(size, stops):
    """Vertical gradient from [(pos, rgb), ...], pos in 0..1."""
    w, h = size
    strip = Image.new("RGB", (1, h))
    px = strip.load()
    for y in range(h):
        t = y / max(1, h - 1)
        lo, hi = stops[0], stops[-1]
        for i in range(len(stops) - 1):
            if stops[i][0] <= t <= stops[i + 1][0]:
                lo, hi = stops[i], stops[i + 1]
                break
        span = max(1e-6, hi[0] - lo[0])
        f = (t - lo[0]) / span
        px[0, y] = tuple(int(lo[1][c] + (hi[1][c] - lo[1][c]) * f) for c in range(3))
    return strip.resize((w, h), Image.BILINEAR)


def radial_glow(size, center, rx, ry, color, strength):
    w, h = size
    sw, sh = max(2, w // 8), max(2, h // 8)
    img = Image.new("RGB", (sw, sh), (0, 0, 0))
    px = img.load()
    cx, cy = center[0] * sw / w, center[1] * sh / h
    sx, sy = max(1.0, rx * sw / w), max(1.0, ry * sh / h)
    for y in range(sh):
        for x in range(sw):
            d = (((x - cx) / sx) ** 2 + ((y - cy) / sy) ** 2) ** 0.5
            if d >= 1.0:
                continue
            f = (1.0 - d) ** 2.2 * strength
            px[x, y] = tuple(min(255, int(color[c] * f)) for c in range(3))
    return img.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(8 * SS))


def draw_tracked(draw, xy, text, font, fill, tracking=0.0, stroke_width=0,
                 stroke_fill=None):
    """Draw text with letter tracking; returns total advance width."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill, stroke_width=stroke_width,
                  stroke_fill=stroke_fill)
        x += draw.textlength(ch, font=font) + tracking
    return x - xy[0] - (tracking if text else 0)


def tracked_width(draw, text, font, tracking):
    if not text:
        return 0.0
    return sum(draw.textlength(c, font=font) for c in text) + tracking * (len(text) - 1)


def text_canvas(size, text, font, fill, xy, tracking=0.0, stroke_width=0, stroke_fill=None):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    draw_tracked(ImageDraw.Draw(layer), xy, text, font, fill, tracking,
                 stroke_width, stroke_fill)
    return layer


def feather_edges(mask, left_px=0, right_px=0, top_px=0, bottom_px=0, gamma=1.3):
    """Fade only the outer N pixels of each edge (not the whole axis)."""
    w, h = mask.size
    grad = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(grad)
    if left_px:
        for i in range(min(left_px, w)):
            d.line([(i, 0), (i, h)], fill=int(255 * (i / left_px) ** gamma))
    if right_px:
        for i in range(min(right_px, w)):
            d.line([(w - 1 - i, 0), (w - 1 - i, h)], fill=int(255 * (i / right_px) ** gamma))
    if top_px:
        for i in range(min(top_px, h)):
            d.line([(0, i), (w, i)], fill=int(255 * (i / top_px) ** gamma))
    if bottom_px:
        for i in range(min(bottom_px, h)):
            d.line([(0, h - 1 - i), (w, h - 1 - i)], fill=int(255 * (i / bottom_px) ** gamma))
    return ImageChops.multiply(mask, grad)


def chrome_title(canvas, text, font_path, size, xy, tracking_ratio=0.030):
    """Hard outline, outer glow, banded specular gradient, top rim light."""
    size_px = canvas.size
    font = ImageFont.truetype(str(font_path), size)
    probe = ImageDraw.Draw(Image.new("RGBA", (8, 8)))
    tracking = size * tracking_ratio

    total_w = tracked_width(probe, text, font, tracking)
    ref = probe.textbbox((0, 0), "H", font=font)
    cap_top, cap_bottom = ref[1], ref[3]

    bx, by = int(xy[0]), int(xy[1] - cap_top)
    bw = int(total_w) + 4
    bh = int(cap_bottom - cap_top) + 4

    mask = Image.new("L", size_px, 0)
    draw_tracked(ImageDraw.Draw(mask), (bx, by), text, font, 255, tracking)

    stroke_w = max(2, int(size * 0.055))
    outline = Image.new("RGBA", size_px, (0, 0, 0, 0))
    draw_tracked(ImageDraw.Draw(outline), (bx, by), text, font, (0, 0, 0, 0),
                 tracking, stroke_w, (2, 7, 14, 245))

    g_in = Image.new("RGBA", size_px, (0, 0, 0, 0))
    g_in.putalpha(mask.filter(ImageFilter.GaussianBlur(size * 0.075))
                  .point(lambda v: int(v * 0.80)))
    g_in = Image.composite(Image.new("RGBA", size_px, CYAN + (0,)),
                           Image.new("RGBA", size_px, (0, 0, 0, 0)),
                           g_in.getchannel("A"))
    g_out = Image.new("RGBA", size_px, (0, 0, 0, 0))
    g_out.putalpha(mask.filter(ImageFilter.GaussianBlur(size * 0.24))
                   .point(lambda v: int(v * 0.40)))
    g_out = Image.composite(Image.new("RGBA", size_px, (40, 150, 210, 0)),
                            Image.new("RGBA", size_px, (0, 0, 0, 0)),
                            g_out.getchannel("A"))

    # specular gradient squeezed exactly onto the cap height
    pad = max(2, int(size * 0.05))
    strip = vgradient((1, bh + pad * 2), CHROME_STOPS)
    band_img = strip.resize((bw, bh + pad * 2), Image.BICUBIC).convert("RGBA")
    region = mask.crop((bx, by - pad, bx + bw, by + bh + pad))
    fill = Image.new("RGBA", size_px, (0, 0, 0, 0))
    fill.paste(band_img, (bx, by - pad), region)
    fill.putalpha(mask)

    # inner top rim light
    rim = ImageChops.subtract(mask.filter(ImageFilter.MaxFilter(3)),
                              mask.filter(ImageFilter.MinFilter(3)))
    rim_top = ImageChops.subtract(rim, ImageChops.offset(rim, 0, 2))
    rim_layer = Image.new("RGBA", size_px, (0, 0, 0, 0))
    rim_layer.paste((225, 250, 255, 135), (0, 0), rim_top.point(lambda v: int(v * 0.55)))

    out = canvas
    for layer in (g_out, g_in, outline, fill, rim_layer):
        out = Image.alpha_composite(out, layer)
    return out, total_w


def spec_strip(canvas, left, y, entries, width):
    """Row of 'LABEL / value' hardware specs separated by thin rules."""
    d = ImageDraw.Draw(canvas)
    lab = ImageFont.truetype(str(FONT_SPEC), int(13 * SS))
    val = ImageFont.truetype(str(FONT_SPEC), int(18 * SS))
    col_w = width / len(entries)
    for i, (label, value) in enumerate(entries):
        x = left + col_w * i
        if i:
            d.line([(int(x - col_w * 0.13), int(y)), (int(x - col_w * 0.13), int(y + 56 * SS))],
                   fill=(70, 120, 150, 115), width=1)
        d.text((x, y), label, font=lab, fill=(122, 160, 184, 255))
        d.text((x, y + 20 * SS), value, font=val, fill=(198, 228, 242, 255))
    return canvas


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", default="PROJECT DRAGOON ARM")
    ap.add_argument("--sub", default="PANZER DRAGOON SAGA")
    ap.add_argument("--tag", default="ARM64  ·  ANBERNIC RG34XX  ·  ALLWINNER H700  ·  MALI-G31 MP2")
    ap.add_argument("--kicker", default="SEGA SATURN  ·  1998  ·  MK-81307")
    ap.add_argument("--desc", default="SATURN ENGINE RECOMPILED FOR AARCH64  ·  VDP1 / VDP2 → OPENGL ES  ·  NO EMULATION",
                    help="dim descriptor line under the hardware tag")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    if not SRC.exists():
        print(f"error: missing source art {SRC}", file=sys.stderr)
        return 1

    cw, ch = W * SS, H * SS
    base = vgradient((cw, ch), [(0.00, INK_TOP), (0.44, (8, 14, 26)), (1.00, INK_BOTTOM)]).convert("RGB")
    base = ImageChops.screen(base, radial_glow((cw, ch), (cw * 0.76, ch * 0.44),
                                               cw * 0.40, ch * 0.52, (24, 88, 124), 0.90))
    base = ImageChops.screen(base, radial_glow((cw, ch), (cw * 0.08, ch * 0.10),
                                               cw * 0.30, ch * 0.30, (30, 40, 84), 0.50))

    # faint technical grid, fading downward
    grid = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grid)
    step = 34 * SS
    for x in range(0, cw, step):
        gd.line([(x, 0), (x, ch)], fill=(96, 190, 232, 9))
    for y in range(0, ch, step):
        gd.line([(0, y), (cw, y)], fill=(96, 190, 232, 9))
    fade = vgradient((cw, ch), [(0.0, (0, 0, 0)), (0.5, (255, 255, 255)), (1.0, (255, 255, 255))]).convert("L")
    grid.putalpha(ImageChops.multiply(grid.getchannel("A"), fade))
    base = Image.alpha_composite(base.convert("RGBA"), grid).convert("RGB")

    # faint orbital arcs, echoing the swirl in the sibling ports' banners
    rings = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rings)
    cxr, cyr = int(cw * 0.70), int(ch * 0.47)
    for i, (rx, ry, a) in enumerate([(cw * 0.33, ch * 0.44, 26), (cw * 0.40, ch * 0.54, 18),
                                     (cw * 0.47, ch * 0.63, 11)]):
        rd.ellipse([cxr - rx, cyr - ry, cxr + rx, cyr + ry],
                   outline=(120, 225, 255, a), width=max(1, 2 * SS))
    rings = rings.filter(ImageFilter.GaussianBlur(1.4 * SS))
    base = Image.alpha_composite(base.convert("RGBA"), rings).convert("RGB")

    # --- key art: bleeds past top/bottom, feathered into the panel --------
    art_src = Image.open(SRC).convert("RGB")
    target_h = int(ch * 1.12)
    art = art_src.resize((int(art_src.width * target_h / art_src.height), target_h), Image.LANCZOS)
    graded = ImageChops.multiply(art, Image.new("RGB", art.size, (168, 200, 245)))
    art = Image.blend(art, graded, 0.42)
    art = ImageEnhance.Color(art).enhance(1.12)
    art = ImageEnhance.Contrast(art).enhance(1.05)

    amask = feather_edges(
        Image.new("L", art.size, 255),
        left_px=int(art.width * 0.42), right_px=int(art.width * 0.04),
        gamma=1.35)
    amask = amask.filter(ImageFilter.GaussianBlur(6 * SS))
    art.putalpha(amask)

    ax = cw - art.width + int(art.width * 0.06)
    ay = -(art.height - ch) // 2
    layer = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    layer.paste(art, (ax, ay), art)
    # feather top/bottom in *canvas* space: the art bleeds past both edges, so
    # an art-space fade of a few percent would still land at ~85% alpha on
    # screen and leave a visible cut line across the top of the banner.
    canvas_feather = vgradient((cw, ch), [
        (0.00, (0, 0, 0)), (0.13, (255, 255, 255)),
        (0.86, (255, 255, 255)), (1.00, (0, 0, 0)),
    ]).convert("L")
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), canvas_feather))
    base = Image.alpha_composite(base.convert("RGBA"), layer).convert("RGB")

    # cool wash over the art's left shoulder so the seam disappears
    wash = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    wd = ImageDraw.Draw(wash)
    span = int(cw * 0.22)
    for i in range(span):
        a = int(150 * (1 - i / span) ** 1.7)
        wd.line([(ax + i, 0), (ax + i, ch)], fill=(6, 12, 22, a))
    base = Image.alpha_composite(base.convert("RGBA"), wash).convert("RGB")

    # --- typography -------------------------------------------------------
    canvas = base.convert("RGBA")
    left = int(cw * 0.050)
    probe = ImageDraw.Draw(Image.new("RGBA", (8, 8)))

    target_w = cw * 0.565
    size = 60 * SS
    while size < 420 * SS:
        f = ImageFont.truetype(str(FONT_TITLE), size)
        if tracked_width(probe, args.title, f, size * 0.030) >= target_w:
            break
        size += 2 * SS
    size -= 2 * SS

    kf = ImageFont.truetype(str(FONT_TAG), int(15 * SS))
    canvas = Image.alpha_composite(canvas, text_canvas(
        (cw, ch), args.kicker, kf, (148, 176, 196, 255), (left, int(ch * 0.150)),
        tracking=3.2 * SS))

    canvas, tw = chrome_title(canvas, args.title, FONT_TITLE, size,
                              (left, int(ch * 0.190)), tracking_ratio=0.030)

    rule_y = int(ch * 0.318)
    d = ImageDraw.Draw(canvas)
    d.rectangle([left, rule_y, left + int(tw * 0.26), rule_y + max(2, 3 * SS)],
                fill=CYAN + (215,))

    sf = ImageFont.truetype(str(FONT_SUB), int(42 * SS))
    sub_layer = text_canvas((cw, ch), args.sub, sf, (150, 238, 255, 255),
                            (left, int(ch * 0.336)), tracking=6.5 * SS)
    sub_glow = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    sub_glow.putalpha(sub_layer.getchannel("A").filter(ImageFilter.GaussianBlur(10 * SS))
                      .point(lambda v: int(v * 0.55)))
    sub_glow = Image.composite(Image.new("RGBA", (cw, ch), CYAN + (0,)),
                               Image.new("RGBA", (cw, ch), (0, 0, 0, 0)),
                               sub_glow.getchannel("A"))
    canvas = Image.alpha_composite(Image.alpha_composite(canvas, sub_glow), sub_layer)

    tf = ImageFont.truetype(str(FONT_TAG), int(19 * SS))
    canvas = Image.alpha_composite(canvas, text_canvas(
        (cw, ch), args.tag, tf, (170, 192, 208, 255), (left, int(ch * 0.398)),
        tracking=2.2 * SS))

    df = ImageFont.truetype(str(FONT_TAG), int(15 * SS))
    canvas = Image.alpha_composite(canvas, text_canvas(
        (cw, ch), args.desc, df, (108, 140, 162, 255), (left, int(ch * 0.452)),
        tracking=1.4 * SS))

    canvas = spec_strip(canvas, left, int(ch * 0.735), [
        ("CPU · 4 CORE", "Cortex-A53 1.5 GHz"),
        ("GPU · OPENGL ES 3.2", "Mali-G31 MP2"),
        ("DISPLAY · 3:2", "720 × 480 IPS"),
        ("MEMORY", "1 GB LPDDR4"),
    ], tw * 0.98)

    # --- finish -----------------------------------------------------------
    out = canvas.convert("RGB")
    vig = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(vig).ellipse([-cw * 0.22, -ch * 0.34, cw * 1.22, ch * 1.34], fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(64 * SS))
    out = Image.composite(out, Image.new("RGB", (cw, ch), (0, 0, 0)), vig)
    out = out.resize((W, H), Image.LANCZOS)
    out = ImageEnhance.Sharpness(out).enhance(1.10)

    print(f"title {size // SS}px, width {int(tw) // SS}px of {W}")
    if args.check:
        return 0

    OUT_PNG.parent.mkdir(parents=True, exist_ok=True)
    out.save(OUT_PNG, "PNG", optimize=True)
    out.save(OUT_JPG, "JPEG", quality=95, subsampling=0, optimize=True)
    for path in (OUT_PNG, OUT_JPG):
        try:
            shown = path.relative_to(ROOT)
        except ValueError:
            shown = path
        print(f"wrote {shown} ({path.stat().st_size / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
