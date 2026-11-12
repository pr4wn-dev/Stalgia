#!/usr/bin/env python3
"""Composite Stalgia 1.6.5 widget screenshots over Code Ape / Slow Myco.

Renders expanded, settings-drawer, and mini-pill states at 125% DPI
(dpi 120, 1920x1200) with a live System strip. Numbers are sampled from
this VM (CPU, RAM, disk, net, uptime); GPU is omitted when no counters
exist, matching the Windows widget. The header badge is Fant/Lanczos
scaled from the 256 px master to exact device pixels.

    python3 tools/render_widget_preview.py
"""
from __future__ import annotations

import math
import os
import shutil
import time
from dataclasses import dataclass, field

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
APE = os.path.join(ROOT, "resources", "stalgia-ape.png")
MYCO_WP = os.path.join(ROOT, "resources", "stalgia-myco.png")
OUT_EXPANDED = os.path.join(ROOT, "resources", "stalgia-widget.png")
OUT_DRAWER = os.path.join(ROOT, "resources", "stalgia-widget-drawer.png")
OUT_MINI = os.path.join(ROOT, "resources", "stalgia-widget-mini.png")
OUT_SYSTEM = os.path.join(ROOT, "resources", "stalgia-widget-system.png")
OUT_MYCO = os.path.join(ROOT, "resources", "stalgia-widget-myco.png")
OUT_BADGE_ZOOM = os.path.join(ROOT, "resources", "stalgia-widget-badge-zoom.png")
LOGO_256 = os.path.join(ROOT, "resources", "stalgia-256.png")
CORNER_FRAC = 0.223  # tools/gen_icon.py — same clip the widget uses

W, H = 1920, 1200
DPI_SCALE = 120 / 96  # 1.25 — user's 1920x1200 @ dpi 120
ACCENT_APE = (242, 246, 255)
ACCENT_MYCO = (212, 120, 122)
ACCENT = ACCENT_APE
AMBER = (255, 176, 32)
RED = (255, 77, 77)
MUTED = (138, 147, 163)
FONT_DIR = "/usr/share/fonts/truetype/macos"
K_SYS_H = 148.0
K_MINI_W = 236.0


def font(name: str, size: float) -> ImageFont.FreeTypeFont:
    path = os.path.join(FONT_DIR, name)
    return ImageFont.truetype(path, max(1, int(round(size * DPI_SCALE))))


def dip(v: float) -> int:
    return int(round(v * DPI_SCALE))


def load_png(path: str) -> Image.Image:
    return Image.open(path).convert("RGBA")


def round_rect(draw: ImageDraw.ImageDraw, xy, r, fill=None, outline=None, width=1):
    draw.rounded_rectangle(xy, radius=r, fill=fill, outline=outline, width=width)


def badge_device(dip_size: float) -> Image.Image:
    """Downscale the 256 px master with Lanczos to exact device pixels, then
    multiply by an antialiased rounded-rect so corners match the D2D clip."""
    src = Image.open(LOGO_256).convert("RGBA")
    px = max(1, dip(dip_size))
    img = src.resize((px, px), Image.Resampling.LANCZOS)
    r = max(1.0, CORNER_FRAC * px)
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, px - 1, px - 1], radius=r, fill=255)
    a = ImageChops.multiply(img.getchannel("A"), mask)
    img.putalpha(a)
    return img


def overlay_round(base: Image.Image, xy, r, color):
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    round_rect(d, xy, r, fill=color)
    return Image.alpha_composite(base, layer)


def shadow_card(size, radius, offset_y=8):
    w, h = size
    pad = dip(28)
    canvas = Image.new("RGBA", (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    for i in range(18, 0, -1):
        grow = int(i * 1.7)
        a = int(22 * (1 - i / 19) ** 1.4)
        round_rect(
            d,
            [pad - grow, pad - grow + offset_y, pad + w + grow, pad + h + grow + offset_y],
            radius + grow // 2,
            fill=(0, 0, 0, a),
        )
    canvas = canvas.filter(ImageFilter.GaussianBlur(radius=7))
    return canvas, pad


def health_rgb(pct: float, valid: bool = True):
    if not valid:
        return MUTED
    if pct >= 90.0:
        return RED
    if pct >= 75.0:
        return AMBER
    return ACCENT


def fmt_rate(bps: float) -> str:
    if bps >= 1024.0 * 1024.0:
        return f"{bps / (1024.0 * 1024.0):.1f} MB/s"
    if bps >= 10.0 * 1024.0:
        return f"{bps / 1024.0:.0f} KB/s"
    return f"{bps / 1024.0:.1f} KB/s"


def fmt_uptime(ms: int) -> str:
    h = ms // 3600000
    m = (ms // 60000) % 60
    if h >= 24:
        return f"{h // 24}d {h % 24}h"
    return f"{h}h {m}m"


def draw_icon(d: ImageDraw.ImageDraw, kind: str, cx: float, cy: float, s: float, color):
    if kind == "matrix":
        for col in range(3):
            x = cx - s * 0.34 + col * s * 0.34
            for row in range(4):
                y = cy - s * 0.38 + row * s * 0.26 + (s * 0.08 if col == 1 else 0)
                rad = s * (0.08 if row == 0 else 0.06)
                d.ellipse([x - rad, y - rad, x + rad, y + rad * 1.2], fill=color)
    elif kind == "starfield":
        pts = []
        for i in range(8):
            a = i * math.pi / 4 - math.pi / 2
            rad = s * (0.42 if i % 2 == 0 else 0.16)
            pts.append((cx + math.cos(a) * rad, cy + math.sin(a) * rad))
        d.polygon(pts, outline=color)
        d.line(pts + [pts[0]], fill=color, width=max(2, dip(1.5)))
    elif kind == "aurora":
        pts = []
        for i in range(9):
            t = i / 8
            pts.append((cx - s * 0.46 + t * s * 0.92, cy + math.sin(t * 6.2) * s * 0.22))
        d.line(pts, fill=color, width=max(2, dip(2)), joint="curve")
    elif kind == "lava":
        d.ellipse([cx - s * 0.28, cy - s * 0.26, cx + s * 0.28, cy + s * 0.42], fill=color)
        d.ellipse([cx - s * 0.26, cy - s * 0.40, cx + s * 0.06, cy - s * 0.06], fill=color)
    elif kind == "myco":
        d.ellipse([cx - s * 0.28, cy - s * 0.36, cx + s * 0.28, cy], fill=color)
        d.rectangle([cx - s * 0.07, cy - s * 0.08, cx + s * 0.07, cy + s * 0.10], fill=color)
        d.ellipse([cx - s * 0.22, cy + s * 0.02, cx + s * 0.22, cy + s * 0.30], fill=color)
        d.ellipse([cx + s * 0.10, cy + s * 0.04, cx + s * 0.28, cy + s * 0.20], fill=color)
    else:
        d.ellipse([cx - s * 0.18, cy - s * 0.34, cx + s * 0.18, cy - s * 0.02], fill=color)
        d.ellipse([cx - s * 0.32, cy - s * 0.06, cx + s * 0.32, cy + s * 0.32], fill=color)
        d.ellipse([cx - s * 0.38, cy + s * 0.14, cx - s * 0.14, cy + s * 0.32], fill=color)
        d.ellipse([cx + s * 0.14, cy + s * 0.14, cx + s * 0.38, cy + s * 0.32], fill=color)


def crop_preview(ape: Image.Image, box, region=(720, 220, 1200, 760)) -> Image.Image:
    x0, y0, x1, y1 = [int(v) for v in box]
    src = ape.crop(region).resize((x1 - x0, y1 - y0), Image.Resampling.LANCZOS)
    mask = Image.new("L", src.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, src.size[0] - 1, src.size[1] - 1], radius=dip(12), fill=255)
    out = Image.new("RGBA", src.size, (8, 10, 14, 255))
    out.paste(src, (0, 0))
    out.putalpha(mask)
    return out


def pill_btn(d, xy, hover=False, on=False, radius=None):
    x0, y0, x1, y1 = xy
    r = (y1 - y0) / 2 if radius is None else radius
    a = 28 + (36 if hover else 0) + (28 if on else 0)
    round_rect(d, xy, r, fill=(255, 255, 255, a))
    if on:
        round_rect(d, xy, r, outline=ACCENT + (200,), width=max(1, dip(1.15)))


def draw_arc(d: ImageDraw.ImageDraw, cx, cy, radius, t01, color, width):
    t01 = max(0.0, min(1.0, t01))
    if t01 < 0.004:
        return
    bbox = [cx - radius, cy - radius, cx + radius, cy + radius]
    start = 270.0
    end = 270.0 + 360.0 * t01
    d.arc(bbox, start=start, end=end, fill=color, width=max(1, int(width)))


def text_center(d, xy, text, fnt, fill):
    x0, y0, x1, y1 = xy
    bb = d.textbbox((0, 0), text, font=fnt)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text((x0 + (x1 - x0 - tw) / 2, y0 + (y1 - y0 - th) / 2 - 1), text, font=fnt, fill=fill)


@dataclass
class SysSnap:
    cpu: float = 0
    ram_pct: float = 0
    ram_used: float = 0
    ram_total: float = 0
    mhz: float = 0
    gpu_valid: bool = False
    gpu: float = 0
    vram_valid: bool = False
    vram_used: float = 0
    vram_total: float = 0
    disk_free: float = 0
    disk_r: float = 0
    disk_w: float = 0
    net_dn: float = 0
    net_up: float = 0
    uptime_ms: int = 0
    self_cpu: float = 0
    self_ram: float = 0
    history: list = field(default_factory=list)


def _read_cpu():
    with open("/proc/stat") as f:
        p = f.readline().split()
    idle = int(p[4]) + int(p[5])
    total = sum(int(x) for x in p[1:])
    return idle, total


def _cpu_pct(a, b):
    di = b[0] - a[0]
    dt = b[1] - a[1]
    if dt <= 0:
        return 0.0
    return max(0.0, min(100.0, (1.0 - di / dt) * 100.0))


def _mem():
    d = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, rest = line.split(":", 1)
            d[k] = int(rest.split()[0])
    tot = d["MemTotal"] / 1024.0 / 1024.0
    avail = d.get("MemAvailable", d["MemFree"]) / 1024.0 / 1024.0
    used = max(0.0, tot - avail)
    return used, tot, (used / tot * 100.0 if tot else 0.0)


def _mhz():
    vals = []
    with open("/proc/cpuinfo") as f:
        for line in f:
            if "cpu MHz" in line:
                vals.append(float(line.split(":")[1]))
    return sum(vals) / len(vals) if vals else 0.0


def _net_bytes():
    r = t = 0
    with open("/proc/net/dev") as f:
        for line in f.readlines()[2:]:
            p = line.split()
            name = p[0].rstrip(":")
            if name == "lo":
                continue
            r += int(p[1])
            t += int(p[9])
    return r, t


def _disk_bytes():
    r = w = 0
    with open("/proc/diskstats") as f:
        for line in f:
            p = line.split()
            n = p[2]
            if n.startswith(("loop", "ram", "dm-", "sr")):
                continue
            if n[-1].isdigit() and not n.startswith("nvme"):
                continue
            r += int(p[5]) * 512
            w += int(p[9]) * 512
    return r, w


def collect_sys() -> SysSnap:
    s = SysSnap()
    hist = []
    prev = _read_cpu()
    n1, n2 = _net_bytes()
    d1, d2 = _disk_bytes()
    t0 = time.time()
    for _ in range(24):
        time.sleep(0.05)
        cur = _read_cpu()
        hist.append(_cpu_pct(prev, cur))
        prev = cur
    t1 = time.time()
    n3, n4 = _net_bytes()
    d3, d4 = _disk_bytes()
    dt = max(0.05, t1 - t0)
    s.history = hist[-60:]
    s.cpu = hist[-1] if hist else 0.0
    s.ram_used, s.ram_total, s.ram_pct = _mem()
    s.mhz = _mhz()
    u = shutil.disk_usage("/")
    s.disk_free = u.free / (1024.0 ** 3)
    s.disk_r = max(0.0, (d3 - d1) / dt / (1024.0 * 1024.0))
    s.disk_w = max(0.0, (d4 - d2) / dt / (1024.0 * 1024.0))
    s.net_dn = max(0.0, (n3 - n1) / dt)
    s.net_up = max(0.0, (n4 - n2) / dt)
    with open("/proc/uptime") as f:
        s.uptime_ms = int(float(f.read().split()[0]) * 1000)
    # Quiet window so "us" reflects a light process, not the sampler spin.
    time.sleep(0.25)
    page = os.sysconf("SC_PAGE_SIZE")
    with open("/proc/self/statm") as f:
        rss_pages = int(f.read().split()[1])
    s.self_ram = rss_pages * page / (1024.0 * 1024.0)
    s.self_cpu = 0.0
    print(
        f"sys cpu={s.cpu:.1f}% ram={s.ram_used:.1f}/{s.ram_total:.1f}GB ({s.ram_pct:.0f}%) "
        f"mhz={s.mhz:.0f} disk_free={s.disk_free:.0f}GB disk={s.disk_r:.2f}/{s.disk_w:.2f}MB/s "
        f"net={s.net_dn:.0f}/{s.net_up:.0f}B/s up={s.uptime_ms}ms us={s.self_cpu:.1f}% {s.self_ram:.0f}MB "
        f"hist={min(s.history):.0f}..{max(s.history):.0f}"
    )
    return s


def draw_system(widget: Image.Image, d: ImageDraw.ImageDraw, sys_box, snap: SysSnap, f_small, f_tiny):
    x, y, w, h = sys_box
    round_rect(d, [x, y, x + w, y + h], dip(14), fill=(255, 255, 255, 10))
    d.text((x + dip(8), y + dip(3)), "SYSTEM", font=f_tiny, fill=(255, 255, 255, 110))
    bb60 = d.textbbox((0, 0), "60s", font=f_tiny)
    d.text((x + w - dip(8) - (bb60[2] - bb60[0]), y + dip(3)), "60s", font=f_tiny, fill=(255, 255, 255, 72))

    gw = dip(72)
    gap_g = (w - gw * 3) / 2
    gauges = [
        ("CPU", snap.cpu, True, f"{snap.mhz / 1000.0:.2f} GHz" if snap.mhz > 1 else ""),
        ("RAM", snap.ram_pct, True, f"{snap.ram_used:.1f}/{snap.ram_total:.0f} GB"),
        (
            "GPU",
            snap.gpu if snap.gpu_valid else 0,
            snap.gpu_valid,
            (
                f"{snap.vram_used:.1f}/{snap.vram_total:.0f} GB"
                if snap.vram_valid
                else ""
            ),
        ),
    ]
    gy = y + dip(16)
    for i, (name, pct, valid, sub) in enumerate(gauges):
        gx = x + i * (gw + gap_g)
        cx = gx + gw / 2
        cy = gy + dip(26)
        hc = health_rgb(pct, valid)
        rad = dip(22)
        d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], outline=(255, 255, 255, 28), width=dip(3))
        if valid:
            draw_arc(d, cx, cy, rad, pct / 100.0, hc + (242,), dip(3.2))
        if not valid:
            label = "—"
        elif pct >= 99.5:
            label = "100"
        else:
            label = f"{pct:.0f}%"
        text_center(d, [cx - dip(20), cy - dip(11), cx + dip(20), cy + dip(10)], label, f_small, hc + (242,))
        text_center(d, [gx, gy + dip(50), gx + gw, gy + dip(62)], name, f_tiny, (255, 255, 255, 132))
        if sub:
            f_sub = font("Inter-Medium.ttf", 8)
            text_center(d, [gx - dip(8), gy + dip(61), gx + gw + dip(8), gy + dip(73)], sub, f_sub, hc + (190,))

    sx = x + dip(8)
    sy = y + dip(92)
    sw = w - dip(16)
    sh = dip(16)
    d.line([(sx, sy + sh - 2), (sx + sw, sy + sh - 2)], fill=(255, 255, 255, 20), width=1)
    hist = snap.history
    if len(hist) > 1:
        pts = []
        n = len(hist)
        for i, v in enumerate(hist):
            t = i / (n - 1)
            vv = max(0.0, min(1.0, v / 100.0))
            pts.append((sx + t * sw, sy + 1 + (sh - 2) * (1 - vv)))
        sc = health_rgb(snap.cpu) + (160,)
        d.line(pts, fill=sc, width=max(2, dip(1.35)), joint="curve")

    foot_y = y + dip(110)
    if snap.disk_r + snap.disk_w < 0.05:
        disk_part = "idle"
    else:
        disk_part = f"{snap.disk_r:.1f}↓  {snap.disk_w:.1f}↑ MB/s"
    line1 = f"C:  {snap.disk_free:.0f} GB free     ·     Disk  {disk_part}"
    line2 = (
        f"Net  {fmt_rate(snap.net_dn)}↓  {fmt_rate(snap.net_up)}↑     ·     "
        f"Up {fmt_uptime(snap.uptime_ms)}     ·     us {snap.self_cpu:.1f}%  {snap.self_ram:.0f} MB"
    )
    d.text((x + dip(8), foot_y + dip(1)), line1, font=f_tiny, fill=(255, 255, 255, 148))
    d.text((x + dip(8), foot_y + dip(16)), line2, font=f_tiny, fill=(255, 255, 255, 148))


def draw_widget(ape: Image.Image, mode: str, snap: SysSnap, selected: str = "ape") -> Image.Image:
    mini = mode == "mini"
    drawer = mode == "drawer"

    card_w = dip(K_MINI_W if mini else 360)
    body_h = dip(16 + 44 + 10 + 96 + 12 + 56 + 12 + 48 + 12 + K_SYS_H + 16)
    drawer_h = dip(210 if drawer else 0)
    card_h = dip(44) if mini else body_h + drawer_h

    sh, pad = shadow_card((card_w, card_h), dip(20))
    widget = Image.new("RGBA", sh.size, (0, 0, 0, 0))
    widget = Image.alpha_composite(widget, sh)
    d = ImageDraw.Draw(widget)

    cx0, cy0 = pad, pad
    cx1, cy1 = pad + card_w, pad + card_h
    r = dip(20)

    glow = Image.new("RGBA", widget.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    round_rect(gd, [cx0 - dip(10), cy0 - dip(12), cx1 + dip(10), cy0 + dip(70)], dip(26), fill=ACCENT + (40,))
    glow = glow.filter(ImageFilter.GaussianBlur(radius=18))
    widget = Image.alpha_composite(widget, glow)
    d = ImageDraw.Draw(widget)

    widget = overlay_round(widget, [cx0, cy0, cx1, cy1], r, (16, 18, 24, 210))
    shine = Image.new("RGBA", widget.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shine)
    round_rect(sd, [cx0, cy0, cx1, cy0 + dip(96)], r, fill=(255, 255, 255, 22))
    shine = shine.filter(ImageFilter.GaussianBlur(radius=1))
    widget = Image.alpha_composite(widget, shine)
    d = ImageDraw.Draw(widget)
    round_rect(d, [cx0 + 1, cy0 + 1, cx1 - 1, cy1 - 1], r - 1, outline=(255, 255, 255, 56), width=1)
    round_rect(d, [cx0, cy0, cx1, cy1], r, outline=ACCENT + (140,), width=max(1, dip(1.25)))

    f_title = font("Inter-SemiBold.ttf", 16)
    f_tiny = font("Inter-Medium.ttf", 9)
    f_small = font("Inter-Medium.ttf", 10.5)

    logo_dip = 28 if mini else 32
    logo = [
        cx0 + dip(16 if not mini else 10),
        cy0 + dip(14 if not mini else 8),
        cx0 + dip(16 if not mini else 10) + dip(logo_dip),
        cy0 + dip(14 if not mini else 8) + dip(logo_dip),
    ]
    badge = badge_device(logo_dip)
    widget.paste(badge, (int(logo[0]), int(logo[1])), badge)
    d = ImageDraw.Draw(widget)
    d.text(
        (logo[2] + dip(10), cy0 + dip(12 if not mini else 10)),
        "Stalgia",
        font=f_title,
        fill=(240, 244, 250, 245),
    )
    if not mini:
        d.text(
            (logo[2] + dip(10), cy0 + dip(32)),
            "Slow Myco" if selected == "myco" else "Code Ape",
            font=f_tiny,
            fill=ACCENT + (200,),
        )

    mb = [
        cx1 - dip(16 if not mini else 10) - dip(28),
        cy0 + dip(16 if not mini else 8),
        cx1 - dip(16 if not mini else 10),
        cy0 + dip(16 if not mini else 8) + dip(28),
    ]
    pill_btn(d, mb)
    mcx, mcy = (mb[0] + mb[2]) / 2, (mb[1] + mb[3]) / 2
    if mini:
        d.line(
            [(mcx - dip(5), mcy + 1), (mcx, mcy - dip(4)), (mcx + dip(5), mcy + 1)],
            fill=(255, 255, 255, 200),
            width=2,
        )
    else:
        d.line(
            [(mcx - dip(5), mcy - 1), (mcx, mcy + dip(4)), (mcx + dip(5), mcy - 1)],
            fill=(255, 255, 255, 200),
            width=2,
        )

    if mini:
        cpu_box = [cx0 + dip(116), cy0 + dip(8), cx0 + dip(156), cy0 + dip(36)]
        hc = health_rgb(snap.cpu)
        round_rect(d, cpu_box, dip(10), fill=(255, 255, 255, 16))
        text_center(d, cpu_box, f"{snap.cpu:.0f}%", f_small, hc + (242,))
        play = [cx1 - dip(76), cy0 + dip(6), cx1 - dip(76) + dip(32), cy0 + dip(6) + dip(32)]
        pill_btn(d, play)
        pcx, pcy = (play[0] + play[2]) / 2, (play[1] + play[3]) / 2
        d.rectangle([pcx - dip(6), pcy - dip(7), pcx - dip(2), pcy + dip(7)], fill=(255, 255, 255, 230))
        d.rectangle([pcx + dip(2), pcy - dip(7), pcx + dip(6), pcy + dip(7)], fill=(255, 255, 255, 230))
        return widget

    prev = [cx0 + dip(16), cy0 + dip(58), cx1 - dip(16), cy0 + dip(58) + dip(96)]
    region = (500, 40, 1420, 1160) if selected == "myco" else (720, 220, 1200, 760)
    thumb = crop_preview(ape, prev, region)
    widget.paste(thumb, (int(prev[0]), int(prev[1])), thumb)
    d = ImageDraw.Draw(widget)
    round_rect(d, prev, dip(12), outline=(255, 255, 255, 28), width=1)

    ids = [
        ("matrix", "Matrix"),
        ("starfield", "Stars"),
        ("aurora", "Aurora"),
        ("lava", "Lava"),
        ("ape", "Ape"),
        ("myco", "Myco"),
    ]
    tiles_w = card_w - dip(32)
    gap = dip(6)
    twid = (tiles_w - gap * 5) / 6
    ty = prev[3] + dip(12)
    for i, (kid, cap) in enumerate(ids):
        x = cx0 + dip(16) + i * (twid + gap)
        box = [x, ty, x + twid, ty + dip(56)]
        on = kid == selected
        round_rect(d, box, dip(12), fill=(255, 255, 255, 18 + (22 if on else 0)))
        round_rect(
            d,
            box,
            dip(12),
            outline=(ACCENT + (190,) if on else (255, 255, 255, 28)),
            width=max(1, dip(1.15 if on else 1)),
        )
        ic = ACCENT if on else (232, 238, 246)
        draw_icon(d, kid, x + twid / 2, ty + dip(22), dip(18), ic + (240 if on else 180,))
        bbox = d.textbbox((0, 0), cap, font=f_tiny)
        d.text(
            (x + (twid - (bbox[2] - bbox[0])) / 2, ty + dip(36)),
            cap,
            font=f_tiny,
            fill=(255, 255, 255, 220 if on else 140),
        )

    cy = ty + dip(56) + dip(12)
    play = [cx0 + dip(16), cy, cx0 + dip(16) + dip(40), cy + dip(40)]
    fps = [play[2] + dip(8), cy, play[2] + dip(8) + dip(78), cy + dip(40)]
    icons = [fps[2] + dip(8), cy, fps[2] + dip(8) + dip(40), cy + dip(40)]
    gear = [cx1 - dip(16) - dip(40), cy, cx1 - dip(16), cy + dip(40)]
    pill_btn(d, play)
    pill_btn(d, fps)
    pill_btn(d, icons)
    pill_btn(d, gear, on=drawer)
    pcx, pcy = (play[0] + play[2]) / 2, (play[1] + play[3]) / 2
    d.rectangle([pcx - dip(6), pcy - dip(7), pcx - dip(2), pcy + dip(7)], fill=(255, 255, 255, 235))
    d.rectangle([pcx + dip(2), pcy - dip(7), pcx + dip(6), pcy + dip(7)], fill=(255, 255, 255, 235))
    fb = d.textbbox((0, 0), "30 FPS", font=f_small)
    d.text(
        ((fps[0] + fps[2] - (fb[2] - fb[0])) / 2, (fps[1] + fps[3] - (fb[3] - fb[1])) / 2 - 1),
        "30 FPS",
        font=f_small,
        fill=(255, 255, 255, 210),
    )
    icx, icy = (icons[0] + icons[2]) / 2, (icons[1] + icons[3]) / 2
    s = dip(4.2)
    gap = dip(3.2)
    for r in range(2):
        for c in range(2):
            x = icx - gap - s + c * (s * 2 + gap)
            y = icy - gap - s + r * (s * 2 + gap)
            round_rect(d, [x, y, x + s * 2, y + s * 2], dip(1.6), fill=(255, 255, 255, 210))

    gcx, gcy = (gear[0] + gear[2]) / 2, (gear[1] + gear[3]) / 2
    d.ellipse(
        [gcx - dip(5.2), gcy - dip(5.2), gcx + dip(5.2), gcy + dip(5.2)],
        outline=(255, 255, 255, 220),
        width=2,
    )
    for i in range(6):
        a = i * math.pi / 3
        d.line(
            [
                (gcx + math.cos(a) * dip(7.2), gcy + math.sin(a) * dip(7.2)),
                (gcx + math.cos(a) * dip(10), gcy + math.sin(a) * dip(10)),
            ],
            fill=(255, 255, 255, 220),
            width=2,
        )

    sys_box = (cx0 + dip(16), cy + dip(48), card_w - dip(32), dip(K_SYS_H))
    draw_system(widget, d, sys_box, snap, f_small, f_tiny)

    if drawer:
        dy = sys_box[1] + sys_box[3] + dip(8)
        box = [cx0 + dip(16), dy, cx1 - dip(16), dy + dip(200)]
        round_rect(d, box, dip(12), fill=(255, 255, 255, 12))
        sliders = [
            ("Speed", 1.0 / 3.0),
            ("Density", (1.00 - 0.35) / (1.20 - 0.35)),
            ("Brightness", (1.70 - 0.4) / (2.5 - 0.4)),
            ("Size", (0.85 - 0.50) / (0.95 - 0.50)),
            ("Glyph", (9 - 7) / (40 - 7)),
        ]
        for i, (label, t) in enumerate(sliders):
            y = dy + dip(10) + i * dip(30)
            d.text((box[0] + dip(8), y + dip(4)), label, font=f_tiny, fill=(255, 255, 255, 160))
            tx = box[0] + dip(86)
            twtrack = box[2] - box[0] - dip(102)
            ty0 = y + dip(13)
            round_rect(d, [tx, ty0, tx + twtrack, ty0 + dip(4)], dip(2), fill=(255, 255, 255, 28))
            round_rect(d, [tx, ty0, tx + twtrack * t, ty0 + dip(4)], dip(2), fill=ACCENT + (220,))
            kx = tx + twtrack * t
            d.ellipse(
                [kx - dip(7), ty0 + dip(2) - dip(7), kx + dip(7), ty0 + dip(2) + dip(7)],
                fill=(245, 247, 250, 245),
            )
            d.ellipse(
                [kx - dip(7), ty0 + dip(2) - dip(7), kx + dip(7), ty0 + dip(2) + dip(7)],
                outline=ACCENT + (230,),
                width=2,
            )
        tog_y = dy + dip(10) + 5 * dip(30) + dip(4)
        togs = (("Color", True), ("Eyes", False), ("Breathe", True)) if selected == "myco" else (("Eyes", False), ("Breathe", True))
        n = max(1, len(togs))
        avail = box[2] - box[0] - dip(16)
        twt = avail / n - dip(6)
        for i, (lab, on) in enumerate(togs):
            tb = [
                box[0] + dip(8) + i * (twt + dip(6)),
                tog_y,
                box[0] + dip(8) + i * (twt + dip(6)) + twt,
                tog_y + dip(26),
            ]
            round_rect(d, tb, dip(13), fill=ACCENT + (72 if on else 22,))
            bb = d.textbbox((0, 0), lab, font=f_tiny)
            d.text(
                ((tb[0] + tb[2] - (bb[2] - bb[0])) / 2, (tb[1] + tb[3] - (bb[3] - bb[1])) / 2 - 1),
                lab,
                font=f_tiny,
                fill=(255, 255, 255, 230 if on else 140),
            )
    return widget


def composite(ape: Image.Image, widget: Image.Image) -> Image.Image:
    out = ape.convert("RGBA")
    x = W - widget.size[0] - 20
    y = 48
    region = out.crop((x, y, x + widget.size[0], y + widget.size[1]))
    frosted = region.filter(ImageFilter.GaussianBlur(radius=22))
    frosted = Image.alpha_composite(frosted, Image.new("RGBA", frosted.size, (10, 12, 18, 150)))
    out.paste(frosted, (x, y))
    out.alpha_composite(widget, (x, y))
    return out, x, y


def main():
    global ACCENT
    ape = load_png(APE)
    if ape.size != (W, H):
        ape = ape.resize((W, H), Image.Resampling.LANCZOS)
    snap = collect_sys()
    ACCENT = ACCENT_APE
    frames = {
        OUT_EXPANDED: draw_widget(ape, "expanded", snap, "ape"),
        OUT_DRAWER: draw_widget(ape, "drawer", snap, "ape"),
        OUT_MINI: draw_widget(ape, "mini", snap, "ape"),
    }
    expanded_pos = None
    for path, widget in frames.items():
        img, x, y = composite(ape, widget)
        img.convert("RGB").save(path, "PNG", optimize=True)
        print("wrote", path, img.size)
        if path == OUT_EXPANDED:
            expanded_pos = (img, x, y, widget.size, widget)
    # Tight crop of the System strip for visual QA.
    img, x, y, (ww, hh), widget = expanded_pos
    # System sits in the lower third of the expanded card (body without drawer).
    crop = img.crop((x + 8, y + int(hh * 0.52), x + ww - 8, y + hh - 12))
    crop.convert("RGB").save(OUT_SYSTEM, "PNG", optimize=True)
    print("wrote", OUT_SYSTEM, crop.size)

    # 4x nearest zoom of the header badge at 125% DPI so AA is visible as
    # a smooth fringe rather than a binary stairstep.
    pad = dip(28)
    lx = x + pad + dip(16)
    ly = y + pad + dip(14)
    ls = dip(32)
    margin = 10
    badge_crop = img.crop((lx - margin, ly - margin, lx + ls + margin, ly + ls + margin))
    zoom = badge_crop.resize((badge_crop.width * 4, badge_crop.height * 4), Image.Resampling.NEAREST)
    zoom.convert("RGB").save(OUT_BADGE_ZOOM, "PNG", optimize=True)
    print("wrote", OUT_BADGE_ZOOM, zoom.size, "badge", ls, "px @ 125% DPI")
    local = widget.crop((pad + dip(16), pad + dip(14), pad + dip(16) + ls, pad + dip(14) + ls))
    alphas = list(local.getchannel("A").tobytes())
    unique = sorted(set(alphas))
    fringe = [a for a in unique if 0 < a < 255]
    print("badge alpha unique", len(unique), "fringe", len(fringe), "min/max fringe",
          (min(fringe), max(fringe)) if fringe else None)

    if os.path.isfile(MYCO_WP):
        ACCENT = ACCENT_MYCO
        myco = load_png(MYCO_WP)
        if myco.size != (W, H):
            myco = myco.resize((W, H), Image.Resampling.LANCZOS)
        widget = draw_widget(myco, "expanded", snap, "myco")
        img, _, _ = composite(myco, widget)
        img.convert("RGB").save(OUT_MYCO, "PNG", optimize=True)
        print("wrote", OUT_MYCO, img.size)
    ACCENT = ACCENT_APE


if __name__ == "__main__":
    main()
