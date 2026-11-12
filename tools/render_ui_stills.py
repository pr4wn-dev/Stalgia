#!/usr/bin/env python3
"""CPU stills of the 1.10.6 spacing pass: widget drawer, Settings, Gallery.

Layout numbers match src/host/UiMetrics.h. Wallpapers are flat color fields —
this script does not redraw the sky.
"""
from __future__ import annotations

import os
from PIL import Image, ImageDraw, ImageFont

OUT = "docs"
FONT = "/usr/share/fonts/truetype/macos/Inter-Regular.ttf"
FONT_MED = "/usr/share/fonts/truetype/macos/Inter-Medium.ttf"
FONT_SEMI = "/usr/share/fonts/truetype/macos/Inter-SemiBold.ttf"

PAD, GAP, SECTION, HEADER_H, HEADER_GAP = 16, 8, 16, 20, 8
ROW_H, ROW_GAP, CHIP_H, CHIP_GAP, CHIP_PAD = 32, 8, 28, 8, 12
BTN_H, GUTTER, THUMB_W, BOTTOM = 32, 24, 6, 16
VALUE_MIN, TRACK_H, MIN_TRACK, MIN_LABEL = 56, 6, 64, 72


def font(path: str, px: float) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(path, max(8, int(round(px))))


def text_width(label: str, px: float) -> float:
    w = 0.0
    for c in label:
        em = 0.56
        if c == " ":
            em = 0.33
        elif c in "iIlj.,'!|":
            em = 0.28
        elif c in "mwMW%@":
            em = 0.92
        elif c.isupper() or c == "×":
            em = 0.70
        elif c.isdigit():
            em = 0.58
        w += em * px
    return w


def chip_w(label: str, px: float, max_w: float) -> float:
    return min(max(44.0, max_w), max(44.0, text_width(label, px) + CHIP_PAD * 2))


def columns(content_w: float, label_need: float, value_need: float):
    value = max(VALUE_MIN, value_need + 8)
    label = max(MIN_LABEL, label_need + 4)
    track = content_w - label - value - GAP * 2
    if track < MIN_TRACK:
        label = max(MIN_LABEL, label - (MIN_TRACK - track))
        track = content_w - label - value - GAP * 2
    return label, max(24.0, track), value


def rr(d: ImageDraw.ImageDraw, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def vtext(d, box, text, fnt, fill, align="left"):
    x, y, w, h = box
    bbox = d.textbbox((0, 0), text, font=fnt)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    ty = y + (h - th) / 2 - bbox[1]
    if align == "right":
        tx = x + w - tw - 2
    elif align == "center":
        tx = x + (w - tw) / 2
    else:
        tx = x
    d.text((tx, ty), text, font=fnt, fill=fill)


MYCO = [
    ("Motion", [("slider", "Speed", 0.55, "1.00×"), ("slider", "Reveal", 0.42, "0.85")]),
    ("Look", [
        ("slider", "Density", 0.62, "1.00"),
        ("slider", "Detail", 0.55, "55%"),
        ("slider", "Breathe", 0.50, "1.00"),
        ("slider", "Brightness", 0.62, "1.70"),
        ("slider", "Glyph", 0.18, "7 px"),
        ("slider", "Size", 0.48, "0.72"),
        ("slider", "Position", 0.50, "0.50"),
        ("toggle", "Color", False),
        ("toggle", "Eyes", True),
        ("toggle", "Breathe", True),
        ("swatch", "Code color", (212, 120, 122)),
        ("slider", "Brightness", 0.70, "1.10"),
        ("slider", "Saturation", 0.55, "1.00"),
        ("slider", "Vignette", 0.30, "0.35"),
    ]),
    ("Advanced", [
        ("slider", "FPS cap", 0.66, "30"),
        ("toggle", "Time of day", True),
        ("toggle", "Mouse parallax", False),
    ]),
]


def flow_drawer(panel_w: float, font_px: float):
    content_w = max(80.0, panel_w - PAD - GUTTER)
    labels = [n for _, rows in MYCO for k, n, *_ in rows if k == "slider"]
    values = [row[-1] for _, rows in MYCO for row in rows if row[0] == "slider"]
    label_w, track_w, value_w = columns(content_w, max(text_width(s, font_px) for s in labels), max(text_width(s, font_px) for s in values))
    items = []
    y = PAD
    items.append(("button", PAD, y, content_w, BTN_H, "All settings"))
    y += BTN_H + GAP
    x = 0.0
    cy = y
    for name in ["Classic", "Mono", "Slow bloom", "Night", "Reset"]:
        w = chip_w(name, font_px, content_w)
        lead = GAP if name == "Reset" and x > 0 else 0
        if x > 0 and x + lead + w > content_w + 0.5:
            x = 0
            cy += CHIP_H + CHIP_GAP
            lead = 0
        x += lead
        items.append(("chip", PAD + x, cy, w, CHIP_H, name))
        x += w + CHIP_GAP
    y = cy + CHIP_H
    for title, rows in MYCO:
        y += SECTION
        items.append(("head", PAD, y, content_w, HEADER_H, title))
        y += HEADER_H + HEADER_GAP
        first = True
        for row in rows:
            if not first:
                y += ROW_GAP
            first = False
            kind = row[0]
            items.append(("row", PAD, y, content_w, ROW_H, row))
            if kind == "slider":
                items.append(("label", PAD, y, label_w, ROW_H, row[1]))
                items.append(("track", PAD + label_w + GAP, y + (ROW_H - TRACK_H) / 2, track_w, TRACK_H, row[2]))
                items.append(("value", PAD + content_w - value_w, y, value_w, ROW_H, row[3]))
            y += ROW_H
    y += BOTTOM
    return items, y, content_w, label_w, track_w, value_w


def assert_clean(items, content_w, content_h, what: str):
    rects = []
    for kind, x, y, w, h, *_ in items:
        if kind in ("row",):
            continue
        if x < PAD - 0.5 or x + w > PAD + content_w + 0.5 or y < PAD - 0.5 or y + h > content_h - BOTTOM + 0.5:
            raise SystemExit(f"{what} {kind} outside content {x,y,w,h}")
        if x + w > PAD + content_w + 0.5:
            raise SystemExit(f"{what} {kind} hits gutter")
        rects.append((x, y, w, h, kind))
    for i, a in enumerate(rects):
        for b in rects[i + 1 :]:
            if a[0] < b[0] + b[2] - 0.5 and b[0] < a[0] + a[2] - 0.5 and a[1] < b[1] + b[3] - 0.5 and b[1] < a[1] + a[3] - 0.5:
                raise SystemExit(f"{what} overlap {a[4]} and {b[4]}")


def paint_widget(scale: float, scroll: float = 0.0) -> Image.Image:
    panel_w = 360 - PAD * 2
    items, content_h, content_w, *_ = flow_drawer(panel_w, 12)
    assert_clean(items, content_w, content_h, "drawer")
    chrome = 168
    drawer_h = min(content_h, 292)
    card_w = 360
    card_h = chrome + drawer_h + 12
    margin = 24
    img = Image.new("RGB", (int((card_w + margin * 2) * scale), int((card_h + margin * 2) * scale)), (14, 16, 20))
    d = ImageDraw.Draw(img)
    ox, oy = margin * scale, margin * scale

    def S(v):
        return v * scale

    def box(x, y, w, h):
        return [ox + S(x), oy + S(y), ox + S(x + w), oy + S(y + h)]

    accent = (212, 120, 122)
    rr(d, box(0, 0, card_w, card_h), S(20), fill=(28, 31, 38))
    f_title = font(FONT_SEMI, 16 * scale)
    f_row = font(FONT, 12 * scale)
    rr(d, box(PAD, 14, card_w - PAD * 2, 36), S(10), fill=(46, 50, 58))
    d.text((ox + S(PAD + 8), oy + S(22)), "Stalgia", font=f_title, fill=(236, 238, 242))
    rr(d, box(PAD, 58, card_w - PAD * 2, 92), S(12), fill=(48, 32, 36))
    vtext(d, (ox + S(PAD), oy + S(58), S(card_w - PAD * 2), S(92)), "Slow Myco", font(FONT_MED, 14 * scale), (235, 220, 220), "center")
    drawer_y = chrome
    scroll = max(0.0, min(scroll, content_h - drawer_h))
    rr(d, box(PAD, drawer_y, panel_w, drawer_h), S(12), fill=(36, 39, 48))
    for kind, x, y, w, h, payload in items:
        vy = y - scroll
        if vy + h <= 0 or vy >= drawer_h:
            continue
        ay = drawer_y + vy
        # x is panel-local and already includes the inner pad. The panel starts at card PAD.
        ax = PAD + x
        b = box(ax, ay, w, min(h, drawer_h - vy))
        if kind == "button":
            rr(d, b, S(8), fill=(120, 72, 78))
            vtext(d, (b[0], b[1], b[2] - b[0], b[3] - b[1]), payload, f_row, (255, 255, 255), "center")
        elif kind == "chip":
            reset = payload == "Reset"
            rr(d, b, S(8), fill=(54, 58, 66) if reset else (120, 72, 78), outline=(186, 190, 198) if reset else None, width=max(1, int(round(scale))))
            vtext(d, (b[0], b[1], b[2] - b[0], b[3] - b[1]), payload, f_row, (214, 218, 224) if reset else (255, 255, 255), "center")
        elif kind == "head":
            vtext(d, (b[0], b[1], b[2] - b[0], b[3] - b[1]), payload, f_row, accent)
        elif kind == "label":
            vtext(d, (b[0], b[1], b[2] - b[0], b[3] - b[1]), payload, f_row, (206, 210, 216))
        elif kind == "value":
            vtext(d, (b[0], b[1], b[2] - b[0], b[3] - b[1]), payload, f_row, (230, 232, 236), "right")
        elif kind == "track":
            t = payload
            rr(d, b, S(2), fill=(68, 72, 80))
            rr(d, [b[0], b[1], b[0] + (b[2] - b[0]) * t, b[3]], S(2), fill=accent)
            cx = b[0] + (b[2] - b[0]) * t
            cy = (b[1] + b[3]) / 2
            r = S(6)
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(236, 238, 242))
        elif kind == "row" and payload[0] == "toggle":
            on = payload[2]
            rr(d, b, S(8), fill=(120, 72, 78) if on else (50, 54, 62))
            vtext(d, (b[0] + S(GAP), b[1], b[2] - b[0] - S(GAP * 2), b[3] - b[1]), payload[1], f_row, (255, 255, 255) if on else (176, 180, 188))
        elif kind == "row" and payload[0] == "swatch":
            vtext(d, (b[0], b[1], b[2] - b[0] - S(40), b[3] - b[1]), payload[1], f_row, (220, 224, 228))
            rr(d, box(ax + w - 28, ay + (h - 20) / 2, 28, 20), S(4), fill=payload[2])
    thumb_x = PAD + panel_w - GUTTER + (GUTTER - THUMB_W) / 2
    travel = drawer_h - GAP * 2
    thumb_h = max(22, travel * (drawer_h / content_h))
    span = max(1.0, content_h - drawer_h)
    thumb_y = drawer_y + GAP + (travel - thumb_h) * (scroll / span)
    rr(d, box(thumb_x, thumb_y, THUMB_W, thumb_h), S(3), fill=(196, 200, 208))
    # Cover anything a partial row painted past the drawer, then restore the card's bottom edge.
    d.rectangle([ox, oy + S(drawer_y + drawer_h), ox + S(card_w), oy + S(card_h)], fill=(28, 31, 38))
    return img.crop((int(ox - S(8)), int(oy - S(8)), int(ox + S(card_w + 8)), int(oy + S(card_h + 8))))


def paint_settings() -> Image.Image:
    W, H = 520, 1180
    img = Image.new("RGB", (W, H), (244, 244, 246))
    d = ImageDraw.Draw(img)
    f = font(FONT, 13)
    fm = font(FONT_MED, 13)
    title = font(FONT_SEMI, 15)
    pad, gap = PAD, GAP
    inner = W - pad * 2
    y = pad
    d.text((pad, y), "Stalgia", font=title, fill=(24, 24, 28))
    vtext(d, (pad + inner - 220, y, 220, 24), "Running", f, (90, 96, 104), "right")
    y += 24 + SECTION
    d.text((pad, y), "Wallpaper", font=fm, fill=(40, 44, 52))
    y += HEADER_H + gap
    rr(d, [pad, y, pad + inner, y + 96], 4, fill=(255, 255, 255), outline=(180, 184, 190))
    for i, name in enumerate(["Matrix (Classic)", "Code Ape", "Slow Myco", "Stars", "Aurora", "Lava"]):
        d.text((pad + 8, y + 6 + i * 15), name, font=font(FONT, 12), fill=(20, 20, 24) if i == 2 else (70, 74, 82))
    y += 96 + gap
    bw = (inner - gap * 2) // 3
    for i, label in enumerate(["Show wallpaper", "Hide wallpaper", "Pause"]):
        x = pad + i * (bw + gap)
        w = inner - 2 * (bw + gap) if i == 2 else bw
        rr(d, [x, y, x + w, y + BTN_H], 6, fill=(255, 255, 255), outline=(170, 174, 180))
        vtext(d, (x, y, w, BTN_H), label, f, (30, 32, 36), "center")
    y += BTN_H + SECTION
    gw = 156
    d.text((pad, y + 6), "Slow Myco settings", font=fm, fill=(32, 36, 42))
    rr(d, [pad + inner - gw, y, pad + inner, y + BTN_H], 6, fill=(36, 40, 48))
    vtext(d, (pad + inner - gw, y, gw, BTN_H), "All settings", f, (255, 255, 255), "center")
    y += BTN_H + gap
    rows = [
        ("slider", "Speed", 0.45, "1.00×"),
        ("slider", "Rain density", 0.6, "1.00"),
        ("slider", "Glyph size", 0.4, "18"),
        ("slider", "Brightness", 0.7, "1.70"),
        ("slider", "Size", 0.5, "0.72"),
        ("slider", "Position", 0.5, "0.50"),
        ("check", "Color", False),
        ("check", "Eye glow", True),
        ("check", "Breathing", True),
    ]
    label_need = max(text_width(r[1], 13) for r in rows if r[0] == "slider")
    value_need = max(text_width(r[3], 13) for r in rows if r[0] == "slider")
    label_w, track_w, value_w = columns(inner, label_need, value_need)
    def slider(name, t, val):
        nonlocal y
        vtext(d, (pad, y, label_w, ROW_H), name, f, (32, 36, 42))
        tx = pad + label_w + gap
        ty = y + (ROW_H - 4) / 2
        rr(d, [tx, ty, tx + track_w, ty + 4], 2, fill=(210, 214, 220))
        rr(d, [tx, ty, tx + track_w * t, ty + 4], 2, fill=(70, 130, 180))
        cx = tx + track_w * t
        d.ellipse([cx - 7, ty - 5, cx + 7, ty + 9], fill=(255, 255, 255), outline=(90, 140, 180))
        vtext(d, (pad + inner - value_w, y, value_w, ROW_H), val, f, (40, 44, 50), "right")
        y += ROW_H + gap
    def check(name, on):
        nonlocal y
        rr(d, [pad, y + 4, pad + 16, y + 20], 3, fill=(70, 130, 180) if on else (255, 255, 255), outline=(120, 126, 134))
        if on:
            d.line([(pad + 3, y + 12), (pad + 7, y + 16), (pad + 13, y + 7)], fill=(255, 255, 255), width=2)
        vtext(d, (pad + 24, y, inner - 24, 24), name, f, (32, 36, 42))
        y += 24 + gap
    for row in rows:
        if row[0] == "slider":
            slider(*row[1:])
        else:
            check(row[1], row[2])
    y += SECTION - gap
    d.text((pad, y), "Playback", font=fm, fill=(32, 36, 42))
    y += HEADER_H + gap
    slider("FPS", 0.5, "30")
    for name, on in [("Pause on fullscreen", True), ("Pause when the desktop is covered", True), ("Pause on battery saver", False), ("Start with Windows", False), ("Hide desktop icons", False)]:
        check(name, on)
    y += SECTION - gap
    half = (inner - gap) // 2
    for i, label in enumerate(["Open config file", "Exit"]):
        x = pad + i * (half + gap)
        w = inner - half - gap if i else half
        rr(d, [x, y, x + w, y + BTN_H], 6, fill=(255, 255, 255), outline=(170, 174, 180))
        vtext(d, (x, y, w, BTN_H), label, f, (30, 32, 36), "center")
    y += BTN_H + gap
    note = "Closing this window leaves the wallpaper running in the tray."
    d.text((pad, y), note, font=f, fill=(80, 84, 92))
    d.text((pad, y + 18), "Hide wallpaper tears the surface down so Show Desktop behaves normally.", font=f, fill=(80, 84, 92))
    y += 52 + pad
    # scrollbar gutter is the non-client bar; draw it outside the client inset
    d.rectangle([W - 1, 0, W, H], fill=(210, 212, 216))
    return img.crop((0, 0, W, min(H, int(y + 8))))


def paint_gallery() -> Image.Image:
    W, H = 1100, 720
    img = Image.new("RGB", (W, H), (18, 20, 26))
    d = ImageDraw.Draw(img)
    rail, side = 176, 320
    d.rectangle([0, 0, rail, H], fill=(23, 25, 32))
    d.rectangle([W - side, 0, W, H], fill=(26, 28, 36))
    f = font(FONT, 13)
    ft = font(FONT, 12)
    title = font(FONT_SEMI, 18)
    d.text((PAD, 16), "Wallpapers", font=title, fill=(236, 238, 242))
    for i, name in enumerate(["Favorites", "Matrix Rain", "Space", "Retro"]):
        y = 56 + i * 44
        on = i == 1
        rr(d, [PAD, y, rail - PAD, y + 36], 8, fill=(40, 108, 72) if on else (42, 46, 56))
        vtext(d, (PAD + 10, y, rail - PAD * 2 - 10, 36), name, f, (245, 246, 248) if on else (200, 204, 210))
    grid_x, grid_w = rail, W - rail - side
    content_w = grid_w - PAD - GUTTER
    cols = max(1, int((content_w + 12) // (200 + 12)))
    card_w = (content_w - 12 * (cols - 1)) / cols
    names = [("matrix", "Matrix (Classic)"), ("ape", "Code Ape"), ("myco", "Slow Myco")]
    for i, (id_, name) in enumerate(names):
        c, r = i % cols, i // cols
        x = grid_x + PAD + c * (card_w + 12)
        y = PAD + r * (150 + 12)
        rr(d, [x, y, x + card_w, y + 138], 12, fill=(32, 34, 42), outline=(90, 220, 140) if id_ == "myco" else (58, 62, 72))
        rr(d, [x + 12, y + 12, x + card_w - 12, y + 78], 8, fill=(20, 22, 28))
        d.text((x + 12, y + 88), name, font=f, fill=(240, 242, 246))
    sx = W - side + PAD
    sw = side - PAD - GUTTER
    y = PAD
    d.text((sx, y), "Slow Myco", font=title, fill=(240, 242, 246))
    y += 28 + GAP
    d.text((sx, y), "Rain that reveals a mushroom turtle.", font=f, fill=(180, 186, 196))
    y += 20 + GAP
    x = 0.0
    cy = y
    for name in ["Classic", "Mono", "Slow bloom", "Night", "Reset to default"]:
        w = chip_w(name, 12, sw)
        lead = GAP if name.startswith("Reset") and x > 0 else 0
        if x > 0 and x + lead + w > sw + 0.5:
            x = 0
            cy += CHIP_H + CHIP_GAP
            lead = 0
        x += lead
        reset = name.startswith("Reset")
        rr(d, [sx + x, cy, sx + x + w, cy + CHIP_H], 8, fill=(48, 52, 60) if reset else (40, 108, 72), outline=(190, 196, 204) if reset else None)
        vtext(d, (sx + x, cy, w, CHIP_H), name, ft, (230, 232, 236) if reset else (255, 255, 255), "center")
        x += w + CHIP_GAP
    y = cy + CHIP_H + SECTION
    rows = [("Speed", 0.5, "1.00×"), ("Reveal", 0.4, "0.85"), ("Density", 0.6, "1.00"), ("Detail", 0.55, "55%"), ("Brightness", 0.7, "1.70")]
    label_w, track_w, value_w = columns(sw, max(text_width(n, 12) for n, *_ in rows), max(text_width(v, 12) for *_, v in rows))
    d.text((sx, y), "Motion", font=ft, fill=(140, 220, 170))
    y += HEADER_H + HEADER_GAP
    for i, (name, t, val) in enumerate(rows):
        if i == 2:
            y += SECTION
            d.text((sx, y), "Look", font=ft, fill=(140, 220, 170))
            y += HEADER_H + HEADER_GAP
        vtext(d, (sx, y, label_w, ROW_H), name, ft, (220, 224, 230))
        tx = sx + label_w + GAP
        ty = y + (ROW_H - TRACK_H) / 2
        rr(d, [tx, ty, tx + track_w, ty + TRACK_H], 3, fill=(58, 62, 72))
        rr(d, [tx, ty, tx + track_w * t, ty + TRACK_H], 3, fill=(90, 210, 140))
        cx = tx + track_w * t
        cy2 = ty + TRACK_H / 2
        d.ellipse([cx - 6, cy2 - 6, cx + 6, cy2 + 6], fill=(235, 245, 238))
        vtext(d, (sx + sw - value_w, y, value_w, ROW_H), val, ft, (200, 206, 214), "right")
        y += ROW_H + (0 if i == len(rows) - 1 else ROW_GAP)
    thumb_x = W - GUTTER + (GUTTER - THUMB_W) / 2
    rr(d, [thumb_x, GAP, thumb_x + THUMB_W, GAP + 120], 3, fill=(255, 255, 255, 70))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    widget = paint_widget(1.25)
    widget.save(os.path.join(OUT, "stalgia-1.10.6-widget-drawer.png"))
    paint_widget(1.25, scroll=10_000).save(os.path.join(OUT, "stalgia-1.10.6-drawer-bottom.png"))
    # 100 / 125 / 150 drawer crops side by side
    crops = []
    for scale in (1.0, 1.25, 1.5):
        im = paint_widget(scale)
        # keep the lower two thirds where the drawer lives
        crops.append(im)
    h = max(c.height for c in crops)
    sheet = Image.new("RGB", (sum(c.width for c in crops) + 16 * 2, h), (12, 14, 18))
    x = 0
    for c in crops:
        sheet.paste(c.convert("RGB"), (x, 0))
        x += c.width + 16
    sheet.save(os.path.join(OUT, "stalgia-1.10.6-drawer-dpi.png"))
    paint_settings().save(os.path.join(OUT, "stalgia-1.10.6-settings.png"))
    paint_gallery().save(os.path.join(OUT, "stalgia-1.10.6-gallery.png"))
    print("stills written")


if __name__ == "__main__":
    main()
