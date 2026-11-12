#!/usr/bin/env python3
"""Bake Slow Myco luminance + region palette masks.

Luma comes from uploads/mushroom-turtle.jpg (internal cartoon shading):
plate seams, pupil, mouth and toe lines stay dim; rims, spots, eye glints
and plate centers stay bright. An unsharp + black-hat + Sobel pass
exaggerates that so 9px rain cells can resolve the plates and face.

The RGB palette keeps 1.6.1 region hues, then *shades* them with the
same luma (darker red at the cap edge, lighter green on the head/legs
and plate centers).

    python3 tools/gen_myco_mask.py
"""
from __future__ import annotations

import math
import os
import struct
import zlib
from collections import deque

from PIL import Image, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC_COLOR = os.path.join(ROOT, "uploads", "turtle-original.png")
SRC_COLOR_RES = os.path.join(ROOT, "resources", "turtle-original.png")
SRC_GRAY = os.path.join(ROOT, "uploads", "mushroom-turtle.jpg")
OUT_GRAY = os.path.join(ROOT, "resources", "mushroom-turtle.jpg")
OUT_GRAY_PNG = os.path.join(ROOT, "resources", "myco-mask.png")
OUT_PREV = os.path.join(ROOT, "resources", "myco-mask-preview.png")
CPP = os.path.join(ROOT, "src", "effects", "MycoMaskData.cpp")
HDR = os.path.join(ROOT, "src", "effects", "MycoMaskData.h")

BG, CAP, SPOT, STEM, SHELL, BODY, PUPIL = range(7)

# Region hue bases. Value is modulated by reference luma.
PAL_DARK = {
    CAP: (0x8A, 0x1A, 0x16),
    SPOT: (0xFF, 0xFF, 0xFF),
    STEM: (0xC8, 0xB4, 0x8C),
    SHELL: (0x28, 0x52, 0x22),
    BODY: (0x38, 0x72, 0x30),
    PUPIL: (0x1A, 0x2E, 0x16),
}
PAL_LIT = {
    CAP: (0xE2, 0x48, 0x3C),
    SPOT: (0xFF, 0xFF, 0xFF),
    STEM: (0xF8, 0xEE, 0xD4),
    SHELL: (0x62, 0xA8, 0x48),
    BODY: (0x7A, 0xC8, 0x58),
    PUPIL: (0x3A, 0x68, 0x32),
}

NAMES = {
    CAP: "cap",
    SPOT: "spot",
    STEM: "stem",
    SHELL: "shell",
    BODY: "body",
    PUPIL: "pupil",
}


def png_gray(w: int, h: int, gray: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + gray[y * w : (y + 1) * w] for y in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def png_rgb(w: int, h: int, rgb: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def rgb_to_hsv(r, g, b):
    r, g, b = r / 255.0, g / 255.0, b / 255.0
    mx, mn = max(r, g, b), min(r, g, b)
    d = mx - mn
    v = mx
    s = 0.0 if mx < 1e-6 else d / mx
    if d < 1e-6:
        h = 0.0
    elif mx == r:
        h = (60.0 * ((g - b) / d) + 360.0) % 360.0
    elif mx == g:
        h = 60.0 * ((b - r) / d) + 120.0
    else:
        h = 60.0 * ((r - g) / d) + 240.0
    return h, s, v


def classify(r, g, b) -> int:
    mx = max(r, g, b)
    if mx < 10:
        return BG
    h, s, v = rgb_to_hsv(r, g, b)
    if v < 0.16:
        return 0
    if s < 0.20 and v > 0.82:
        return SPOT
    if v > 0.62 and s < 0.50 and 18.0 <= h <= 58.0:
        return STEM
    if s > 0.32 and v > 0.22 and (h <= 20.0 or h >= 340.0):
        return CAP
    if 55.0 <= h <= 108.0 and v >= 0.46 and s > 0.28:
        return BODY
    if 80.0 <= h <= 165.0 and s > 0.20:
        return SHELL
    if v > 0.55 and s < 0.22:
        return SPOT
    return 0


def flood_bg(im: Image.Image, thresh: int = 10) -> Image.Image:
    w, h = im.size
    px = im.load()
    bg = Image.new("L", (w, h), 0)
    bp = bg.load()
    stack = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1), (w // 2, 0), (w // 2, h - 1)]
    seen = bytearray(w * h)
    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h:
            continue
        i = y * w + x
        if seen[i]:
            continue
        seen[i] = 1
        r, g, b = px[x, y][:3]
        if max(r, g, b) > thresh:
            continue
        bp[x, y] = 255
        stack.extend(((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)))
    return bg


def bbox_fg(bg: Image.Image):
    w, h = bg.size
    bp = bg.load()
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if bp[x, y] == 0:
                xs.append(x)
                ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)


def propagate(labels, inside, w, h):
    q = deque()
    for y in range(h):
        row = y * w
        for x in range(w):
            i = row + x
            if inside[i] and labels[i] != BG:
                q.append(i)
    nbs = (-1, 1, -w, w)
    while q:
        i = q.popleft()
        lab = labels[i]
        x = i % w
        for d in nbs:
            j = i + d
            if d in (-1, 1):
                nx = j % w
                if abs(nx - x) > 1:
                    continue
            if j < 0 or j >= w * h:
                continue
            if inside[j] and labels[j] == BG:
                labels[j] = lab
                q.append(j)


def sobel_mag(gray: Image.Image) -> Image.Image:
    w, h = gray.size
    p = gray.tobytes()
    out = bytearray(w * h)
    for y in range(1, h - 1):
        row = y * w
        for x in range(1, w - 1):
            i = row + x
            gx = (
                -p[i - w - 1]
                + p[i - w + 1]
                - 2 * p[i - 1]
                + 2 * p[i + 1]
                - p[i + w - 1]
                + p[i + w + 1]
            )
            gy = (
                -p[i - w - 1]
                - 2 * p[i - w]
                - p[i - w + 1]
                + p[i + w - 1]
                + 2 * p[i + w]
                + p[i + w + 1]
            )
            mag = int(math.sqrt(gx * gx + gy * gy) * 0.125 + 0.5)
            out[i] = 255 if mag > 255 else mag
    return Image.frombytes("L", (w, h), bytes(out))


def lerp_rgb(a, b, t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return (
        int(a[0] + (b[0] - a[0]) * t + 0.5),
        int(a[1] + (b[1] - a[1]) * t + 0.5),
        int(a[2] + (b[2] - a[2]) * t + 0.5),
    )


def write_cpp(w: int, h: int, luma: bytes, rgb: bytes) -> None:
    hdr = """#pragma once

// Generated luminance (R8) + region RGB palette for Slow Myco.
// 0 luma = keyed background. Non-zero = turtle/mushroom shading.
extern const int kMycoMaskW;
extern const int kMycoMaskH;
extern const int kMycoMaskBytes;
extern const unsigned char kMycoMask[];
extern const unsigned char kMycoRgb[];
"""
    os.makedirs(os.path.dirname(HDR), exist_ok=True)
    with open(HDR, "w", encoding="utf-8") as f:
        f.write(hdr)

    lines = [
        "// Auto-generated by tools/gen_myco_mask.py — do not edit by hand.",
        '#include "effects/MycoMaskData.h"',
        "",
        f"const int kMycoMaskW = {w};",
        f"const int kMycoMaskH = {h};",
        "const unsigned char kMycoMask[] = {",
    ]
    for i in range(0, len(luma), 24):
        chunk = luma[i : i + 24]
        lines.append("    " + ", ".join(str(b) for b in chunk) + ",")
    lines.append("};")
    lines.append(f"const int kMycoMaskBytes = {len(luma)};")
    lines.append("const unsigned char kMycoRgb[] = {")
    for i in range(0, len(rgb), 24):
        chunk = rgb[i : i + 24]
        lines.append("    " + ", ".join(str(b) for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    with open(CPP, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def segment_color(crop_c: Image.Image, crop_bg: Image.Image):
    cw, ch = crop_c.size
    cp = crop_c.load()
    bp = crop_bg.load()
    n = cw * ch
    labels = bytearray(n)
    inside = bytearray(n)
    counts = {k: 0 for k in NAMES}
    for y in range(ch):
        for x in range(cw):
            i = y * cw + x
            if bp[x, y]:
                continue
            inside[i] = 1
            lab = classify(*cp[x, y][:3])
            labels[i] = lab
            if lab in counts:
                counts[lab] += 1

    for y in range(ch):
        for x in range(cw):
            i = y * cw + x
            if not inside[i] or labels[i] != BG:
                continue
            r, g, b = cp[x, y]
            _h, _s, v = rgb_to_hsv(r, g, b)
            if v > 0.22:
                continue
            near_spot = False
            for dy in range(-3, 4):
                for dx in range(-3, 4):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < cw and 0 <= ny < ch and labels[ny * cw + nx] == SPOT:
                        near_spot = True
            if near_spot:
                labels[i] = PUPIL
                counts[PUPIL] += 1

    propagate(labels, inside, cw, ch)
    leftover = 0
    for i in range(n):
        if inside[i] and labels[i] == BG:
            labels[i] = BODY
            leftover += 1
            counts[BODY] += 1
    return labels, inside, counts, leftover


def build_luma(gray: Image.Image, inside: bytearray, labels: bytearray) -> Image.Image:
    """Edge-enhanced luma: bright rims/highlights, dim interior linework."""
    w, h = gray.size
    g = gray.convert("L")
    gp = g.load()

    # Unsharp / local-contrast (CLAHE-style punch without flattening).
    blur = g.filter(ImageFilter.GaussianBlur(radius=1.8))
    bp = blur.load()
    sharp = Image.new("L", (w, h), 0)
    sp = sharp.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not inside[i]:
                continue
            v = gp[x, y]
            m = bp[x, y]
            s = int(m + 2.2 * (v - m) + 0.5)
            sp[x, y] = 0 if s < 0 else 255 if s > 255 else s

    # Black-hat = dark thin lines (plate seams, mouth, toes, pupil ring).
    closed = sharp.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(5))
    cp = closed.load()
    thin = Image.new("L", (w, h), 0)
    tp = thin.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not inside[i]:
                continue
            d = cp[x, y] - sp[x, y]
            if d > 10:
                tp[x, y] = min(255, d * 3)

    # Keep only thin structures: opening removes large dark blobs (underside).
    opened = thin.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))
    op = opened.load()
    for y in range(h):
        for x in range(w):
            if op[x, y] > 40:
                tp[x, y] = 0  # large dark region, not a seam

    # Thicken shell seams to ~one glyph cell; keep body strokes thinner.
    thin = thin.filter(ImageFilter.MaxFilter(3))
    tp = thin.load()

    sob = sobel_mag(sharp)
    sobp = sob.load()

    # Outer rim (silhouette) — brightest band.
    rim = Image.new("L", (w, h), 0)
    rp = rim.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not inside[i]:
                continue
            border = False
            if x == 0 or not inside[i - 1]:
                border = True
            if x + 1 >= w or not inside[i + 1]:
                border = True
            if y == 0 or not inside[i - w]:
                border = True
            if y + 1 >= h or not inside[i + w]:
                border = True
            if border:
                rp[x, y] = 255
    rim = rim.filter(ImageFilter.MaxFilter(3))
    rp = rim.load()

    out = Image.new("L", (w, h), 0)
    lp = out.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not inside[i]:
                continue
            v = sp[x, y] / 255.0
            # Stretch cartoon values so plate fill (~0.43) lifts and
            # near-seam leftovers drop.
            if v < 0.08:
                mapped = 0.0
            elif v < 0.42:
                mapped = 0.22 + (v / 0.42) * 0.38  # dark plates / underside
            elif v < 0.72:
                mapped = 0.62 + (v - 0.42) / 0.30 * 0.26  # body / plate
            else:
                mapped = 0.88 + (v - 0.72) / 0.28 * 0.12  # spots / stem
            # Sobel boost on bright edges only (not on dark seams).
            edge = sobp[x, y] / 255.0
            if tp[x, y] < 40:
                mapped = min(1.0, mapped + 0.45 * edge)
            # Interior linework. Shell seams become empty gaps so hex
            # plates read as separate tiles; body/face strokes only dim.
            seam = tp[x, y] / 255.0
            lab = labels[i]
            if lab == PUPIL:
                mapped = 0.12
            elif lab == SHELL and seam > 0.10:
                mapped = 0.0
            elif seam > 0.18:
                mapped = min(mapped, 0.32)
            if rp[x, y] and lab != PUPIL:
                mapped = max(mapped, 0.94)
            # Eye glints / white spots stay max.
            if gp[x, y] >= 240:
                mapped = 1.0
            lum = int(mapped * 255 + 0.5)
            if lum < 8:
                lum = 0
            lp[x, y] = lum

    # Widen shell-plate gaps so they occupy a rain cell.
    holes = Image.new("L", (w, h), 0)
    hp = holes.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if labels[i] == SHELL and lp[x, y] < 8 and inside[i]:
                hp[x, y] = 255
    holes = holes.filter(ImageFilter.MaxFilter(5))
    hp = holes.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if hp[x, y] and labels[i] == SHELL and not rp[x, y]:
                lp[x, y] = 0

    # Eye: solid dim pupil, bright sclera/glints so the face reads in rain.
    glint = Image.new("L", (w, h), 0)
    gp2 = glint.load()
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if labels[i] == PUPIL:
                lp[x, y] = 28
            elif labels[i] != BG and gp[x, y] >= 220:
                # white of the eye / cap spots already handled; mark glints
                # near a pupil so they occupy a glyph.
                near_p = False
                for dy in range(-12, 13):
                    yy = y + dy
                    if yy < 0 or yy >= h:
                        continue
                    for dx in range(-12, 13):
                        xx = x + dx
                        if 0 <= xx < w and labels[yy * w + xx] == PUPIL:
                            near_p = True
                            break
                    if near_p:
                        break
                if near_p:
                    gp2[x, y] = 255
    glint = glint.filter(ImageFilter.MaxFilter(3))
    gp2 = glint.load()
    for y in range(h):
        for x in range(w):
            if gp2[x, y]:
                lp[x, y] = 255
    return out


def main() -> int:
    color_path = SRC_COLOR if os.path.isfile(SRC_COLOR) else SRC_COLOR_RES
    if not os.path.isfile(color_path):
        raise SystemExit("missing turtle-original.png")
    if not os.path.isfile(SRC_GRAY):
        raise SystemExit("missing uploads/mushroom-turtle.jpg")

    color = Image.open(color_path).convert("RGB")
    gray_src = Image.open(SRC_GRAY).convert("L")
    bg = flood_bg(color)
    x0, y0, x1, y1 = bbox_fg(bg)
    pad = int(round(0.04 * max(x1 - x0 + 1, y1 - y0 + 1)))
    x0, y0 = max(0, x0 - pad), max(0, y0 - pad)
    x1, y1 = min(color.size[0] - 1, x1 + pad), min(color.size[1] - 1, y1 + pad)
    crop_c = color.crop((x0, y0, x1 + 1, y1 + 1))
    crop_bg = bg.crop((x0, y0, x1 + 1, y1 + 1))

    ow, oh = gray_src.size
    crop_c = crop_c.resize((ow, oh), Image.Resampling.LANCZOS)
    crop_bg = crop_bg.resize((ow, oh), Image.Resampling.NEAREST)
    gray = gray_src.resize((ow, oh), Image.Resampling.LANCZOS)

    # Re-key background from the gray (cleaner silhouette than the color AA).
    gp0 = gray.load()
    bp = crop_bg.load()
    for y in range(oh):
        for x in range(ow):
            if gp0[x, y] < 8:
                bp[x, y] = 255

    labels, inside, counts, leftover = segment_color(crop_c, crop_bg)
    luma_img = build_luma(gray, inside, labels)
    lp = luma_img.load()
    gp = gray.load()

    rgb_img = Image.new("RGB", (ow, oh), (0, 0, 0))
    rp = rgb_img.load()
    for y in range(oh):
        for x in range(ow):
            i = y * ow + x
            if not inside[i] or lp[x, y] < 8:
                continue
            lab = labels[i]
            # Shade with the *reference* luma, not the enhanced mask, so
            # plate centers go light-green and seams/underside go dark.
            t = gp[x, y] / 255.0
            t = t ** 0.85
            if lab == SPOT or gp[x, y] >= 240:
                rp[x, y] = PAL_LIT[SPOT]
            else:
                rp[x, y] = lerp_rgb(PAL_DARK.get(lab, PAL_DARK[BODY]), PAL_LIT.get(lab, PAL_LIT[BODY]), t)

    rgb_img = rgb_img.filter(ImageFilter.GaussianBlur(radius=0.8))
    rp = rgb_img.load()
    for y in range(oh):
        for x in range(ow):
            if lp[x, y] < 8:
                rp[x, y] = (0, 0, 0)

    os.makedirs(os.path.join(ROOT, "resources"), exist_ok=True)
    luma_bytes = luma_img.tobytes()
    rgb_bytes = rgb_img.tobytes()
    luma_img.save(OUT_GRAY, quality=95)
    with open(OUT_GRAY_PNG, "wb") as f:
        f.write(png_gray(ow, oh, luma_bytes))
    with open(OUT_PREV, "wb") as f:
        f.write(png_rgb(ow, oh, rgb_bytes))

    write_cpp(ow, oh, luma_bytes, rgb_bytes)
    nz = sum(1 for b in luma_bytes if b > 0)
    print(f"gray {gray_src.size} color-crop -> {ow}x{oh} inside={nz}/{ow * oh} leftover={leftover}")
    for k, name in NAMES.items():
        print(f"  {name:6s} {counts[k]:6d}")
    print(f"wrote {OUT_GRAY}")
    print(f"wrote {OUT_GRAY_PNG}")
    print(f"wrote {OUT_PREV}")
    print(f"wrote {CPP} ({os.path.getsize(CPP)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
