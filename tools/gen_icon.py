#!/usr/bin/env python3
"""Build the Stalgia app icon from the approved iridescent-S artwork.

Crops uploads/stalgia-icon-v2.jpg to the rounded tile, drops the white
surround and drop shadow, and writes a multi-size ICO plus PNG previews.

    python3 tools/gen_icon.py
"""
from __future__ import annotations

import os
import struct
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC_CANDIDATES = (
    os.path.join(ROOT, "uploads", "stalgia-icon-v2.jpg"),
    os.path.join(ROOT, "resources", "stalgia-icon-v2.jpg"),
)
ICO_OUT = os.path.join(ROOT, "resources", "app.ico")
PNG_OUT = os.path.join(ROOT, "resources", "stalgia-256.png")
LOGO_OUT = os.path.join(ROOT, "resources", "stalgia-logo.png")
SHEET_OUT = os.path.join(ROOT, "resources", "stalgia-icon-sheet.png")

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SHEET_SIZES = (16, 24, 32, 48, 256)
# iOS-style continuous corner, expressed as a rounded-rect radius.
CORNER_FRAC = 0.223


def png_bytes(w: int, h: int, rgba: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + rgba[y * w * 4 : (y + 1) * w * 4] for y in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def ico_bmp32(w: int, rgba: bytes) -> bytes:
    header = struct.pack(
        "<IiiHHIIiiII",
        40,
        w,
        w * 2,
        1,
        32,
        0,
        w * w * 4,
        0,
        0,
        0,
        0,
    )
    xor = bytearray(w * w * 4)
    for y in range(w):
        src_y = w - 1 - y
        for x in range(w):
            i = (src_y * w + x) * 4
            o = (y * w + x) * 4
            r, g, b, a = rgba[i : i + 4]
            xor[o : o + 4] = bytes((b, g, r, a))
    row_and = ((w + 31) // 32) * 4
    mask = bytearray(row_and * w)
    for y in range(w):
        src_y = w - 1 - y
        for x in range(w):
            a = rgba[(src_y * w + x) * 4 + 3]
            if a < 16:
                mask[y * row_and + (x >> 3)] |= 0x80 >> (x & 7)
    return header + bytes(xor) + bytes(mask)


def write_ico(path: str, frames: list[tuple[int, bytes]]) -> None:
    count = len(frames)
    header = struct.pack("<HHH", 0, 1, count)
    offset = 6 + 16 * count
    entries = b""
    payloads = b""
    for s, payload in frames:
        entries += struct.pack(
            "<BBBBHHII",
            s if s < 256 else 0,
            s if s < 256 else 0,
            0,
            0,
            1,
            32,
            len(payload),
            offset,
        )
        offset += len(payload)
        payloads += payload
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + entries + payloads)


def luma_of(arr: np.ndarray) -> np.ndarray:
    r = arr[:, :, 0].astype(np.float32)
    g = arr[:, :, 1].astype(np.float32)
    b = arr[:, :, 2].astype(np.float32)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def chroma_of(arr: np.ndarray) -> np.ndarray:
    mx = arr.max(axis=2).astype(np.float32)
    mn = arr.min(axis=2).astype(np.float32)
    return mx - mn


def rounded_rect_alpha(size: int, radius: float) -> np.ndarray:
    """Antialiased coverage of a rounded square filling [0, size)."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    px = xx + 0.5
    py = yy + 0.5
    hx = size * 0.5
    qx = np.abs(px - hx) - (hx - radius)
    qy = np.abs(py - hx) - (hx - radius)
    ax = np.maximum(qx, 0.0)
    ay = np.maximum(qy, 0.0)
    sdf = np.hypot(ax, ay) + np.minimum(np.maximum(qx, qy), 0.0) - radius
    return np.clip(0.5 - sdf, 0.0, 1.0).astype(np.float32)


def find_tile_square(rgb: np.ndarray) -> tuple[int, int, int]:
    """Return (x, y, side) of the dark rounded tile, excluding white + shadow."""
    h, w = rgb.shape[:2]
    luma = luma_of(rgb)
    chroma = chroma_of(rgb)
    tile = (luma < 110.0) | (chroma > 22.0)

    def run_bounds(vec: np.ndarray, min_run: int) -> tuple[int, int]:
        idx = np.where(vec)[0]
        if idx.size < min_run:
            raise RuntimeError("could not find icon tile in source artwork")
        return int(idx[0]), int(idx[-1])

    # Use mid-band occupancy so sparkles don't shrink the box.
    row_occ = tile.sum(axis=1)
    col_occ = tile.sum(axis=0)
    y0, y1 = run_bounds(row_occ > w * 0.08, 40)
    x0, x1 = run_bounds(col_occ > h * 0.08, 40)

    # Shadow lives under the tile: trim the bottom until the left/right
    # edges are actually dark (the shadow is a light gray halo).
    for y in range(y1, y0, -1):
        left = luma[y, x0 : x0 + 8].mean()
        right = luma[y, x1 - 7 : x1 + 1].mean()
        if left < 140.0 and right < 140.0:
            y1 = y
            break

    for x in range(x0, x1):
        if luma[y0 + 8 : y1 - 8, x].mean() < 140.0:
            x0 = x
            break
    for x in range(x1, x0, -1):
        if luma[y0 + 8 : y1 - 8, x].mean() < 140.0:
            x1 = x
            break

    side = max(x1 - x0 + 1, y1 - y0 + 1)
    # Prefer the sharp top edge; the shadow is below.
    x = max(0, x0 - 1)
    y = max(0, y0 - 1)
    if x + side > w:
        x = w - side
    if y + side > h:
        y = h - side
    return x, y, side


def crop_tile(src: Image.Image) -> Image.Image:
    rgb = np.asarray(src.convert("RGB"))
    x, y, side = find_tile_square(rgb)
    crop = src.convert("RGB").crop((x, y, x + side, y + side))
    master = crop.resize((512, 512), Image.Resampling.LANCZOS)
    arr = np.asarray(master).astype(np.float32)
    alpha = rounded_rect_alpha(512, 512 * CORNER_FRAC)

    # Kill residual white/shadow that survived the crop (outside the tile
    # body): near-white, low-chroma pixels become fully transparent.
    luma = luma_of(arr)
    chroma = chroma_of(arr)
    leftover = (luma > 210.0) & (chroma < 18.0)
    alpha = np.where(leftover, 0.0, alpha)

    out = np.zeros((512, 512, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(arr, 0, 255).astype(np.uint8)
    out[:, :, 3] = np.clip(np.round(alpha * 255.0), 0, 255).astype(np.uint8)
    # Premultiply RGB by alpha so LANCZOS resizes don't fringe white.
    a = out[:, :, 3:4].astype(np.float32) / 255.0
    out[:, :, :3] = np.clip(np.round(out[:, :, :3].astype(np.float32) * a), 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def _s_mask(arr: np.ndarray) -> Image.Image:
    rgb = arr[:, :, :3].astype(np.float32)
    a = arr[:, :, 3].astype(np.float32) / 255.0
    luma = luma_of(rgb)
    chroma = chroma_of(rgb)
    bg = float(np.median(luma[a > 0.8]))
    m = ((chroma > 14.0) | (luma > bg + 22.0)) & (a > 0.45)
    return Image.fromarray((m * 255).astype(np.uint8), "L")


def _connected_keep(mask: Image.Image, min_area: int) -> Image.Image:
    """Drop isolated sparkles smaller than min_area (4-connected)."""
    a = np.array(mask)
    h, w = a.shape
    seen = np.zeros_like(a, dtype=bool)
    out = np.zeros_like(a)
    for y in range(h):
        for x in range(w):
            if a[y, x] < 80 or seen[y, x]:
                continue
            stack = [(y, x)]
            blob = []
            seen[y, x] = True
            while stack:
                cy, cx = stack.pop()
                blob.append((cy, cx))
                for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                    if 0 <= ny < h and 0 <= nx < w and not seen[ny, nx] and a[ny, nx] >= 80:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            if len(blob) >= min_area:
                for by, bx in blob:
                    out[by, bx] = 255
    return Image.fromarray(out, "L")


def tune_small(img: Image.Image, size: int) -> Image.Image:
    """Bolder S, fewer particles, higher contrast for tray/taskbar sizes."""
    work = img.resize((size * 4, size * 4), Image.Resampling.LANCZOS)
    arr = np.array(work)
    mask = _s_mask(arr)
    # Thicken the letter, then strip specks that aren't attached to it.
    grow = 5 if size <= 16 else 3 if size <= 24 else 3
    mask = mask.filter(ImageFilter.MaxFilter(grow))
    min_area = 18 if size <= 16 else 12 if size <= 24 else 8
    mask = _connected_keep(mask, min_area)
    # A light close so the stroke reads as one S, not glitter.
    mask = mask.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))
    mask_a = np.asarray(mask).astype(np.float32) / 255.0

    rgb = arr[:, :, :3].astype(np.float32)
    a = arr[:, :, 3].astype(np.float32)
    luma = luma_of(rgb)
    bg = float(np.median(luma[a > 200])) if np.any(a > 200) else 28.0

    # Darken the tile so the S pops.
    tile = np.clip((bg - 10.0) / max(bg, 1.0), 0.55, 0.92)
    rgb = rgb * (1.0 - 0.38 * (1.0 - mask_a)[..., None] * (a[:, :, None] / 255.0))
    rgb *= tile + (1.0 - tile) * mask_a[..., None]

    # Boost S contrast and saturation.
    mean = rgb.mean(axis=2, keepdims=True)
    rgb = mean + (rgb - mean) * (1.55 if size <= 20 else 1.40)
    rgb = bg * 0.35 + rgb * 1.25
    rgb = np.clip(rgb, 0, 255)

    # Composite thickened S over the darkened tile using original colours
    # lifted toward white on the stroke core.
    core = np.asarray(mask.filter(ImageFilter.MinFilter(3))).astype(np.float32) / 255.0
    lift = 0.22 if size <= 16 else 0.14
    rgb = rgb * (1.0 - core[..., None] * lift) + 255.0 * core[..., None] * lift

    out = arr.copy()
    out[:, :, :3] = np.clip(np.round(rgb), 0, 255).astype(np.uint8)
    # Keep original alpha (rounded-rect), but punch tiny leftover halos.
    down = Image.fromarray(out, "RGBA").resize((size, size), Image.Resampling.LANCZOS)
    return reapply_round_alpha(down, size)


def reapply_round_alpha(img: Image.Image, size: int) -> Image.Image:
    arr = np.array(img).astype(np.float32)
    cov = rounded_rect_alpha(size, size * CORNER_FRAC)
    # Un-premultiply if needed, then remultiply by clean coverage.
    a = np.clip(arr[:, :, 3] / 255.0, 0.0, 1.0)
    rgb = arr[:, :, :3]
    with np.errstate(divide="ignore", invalid="ignore"):
        straight = np.where(a[:, :, None] > 0.02, rgb / a[:, :, None], rgb)
    a2 = np.minimum(a, cov)
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, :3] = np.clip(np.round(straight * a2[:, :, None]), 0, 255).astype(np.uint8)
    out[:, :, 3] = np.clip(np.round(a2 * 255.0), 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def render_size(master: Image.Image, size: int) -> Image.Image:
    if size <= 32:
        return tune_small(master, size)
    img = master.resize((size, size), Image.Resampling.LANCZOS)
    if size <= 48:
        img = ImageEnhance.Contrast(img).enhance(1.08)
        img = ImageEnhance.Color(img).enhance(1.08)
    return reapply_round_alpha(img, size)


def as_straight(img: Image.Image) -> Image.Image:
    arr = np.array(img.convert("RGBA")).astype(np.float32)
    a = np.clip(arr[:, :, 3] / 255.0, 0.0, 1.0)
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(
            a[:, :, None] > 0.02,
            np.clip(arr[:, :, :3] / np.maximum(a[:, :, None], 1e-6), 0, 255),
            arr[:, :, :3],
        )
    out = np.zeros(arr.shape, dtype=np.uint8)
    out[:, :, :3] = np.clip(np.round(rgb), 0, 255).astype(np.uint8)
    out[:, :, 3] = arr[:, :, 3].astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def rgba_bytes(img: Image.Image) -> bytes:
    return as_straight(img).tobytes()


def contact_sheet(frames: dict[int, Image.Image]) -> Image.Image:
    pad = 28
    gap = 28
    label_h = 22
    cells = []
    for s in SHEET_SIZES:
        cells.append(256 if s == 256 else max(s, 64))
    row_w = sum(cells) + gap * (len(cells) - 1)
    row_h = 256 + label_h + 28
    width = pad * 2 + row_w
    height = pad * 2 + 2 * row_h + 48
    sheet = Image.new("RGBA", (width, height), (36, 36, 40, 255))
    draw = ImageDraw.Draw(sheet)

    def blit_row(y0: int, bg: tuple[int, int, int], title: str) -> None:
        draw.rounded_rectangle([12, y0 - 12, width - 12, y0 + row_h + 4], 18, fill=bg + (255,))
        title_c = (40, 40, 40) if bg[0] > 80 else (220, 220, 220)
        draw.text((pad, y0 - 4), title, fill=title_c)
        x = pad
        y_icon = y0 + 22
        for s, cw in zip(SHEET_SIZES, cells):
            icon = frames[s]
            if s == 256:
                cell = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
                cd = ImageDraw.Draw(cell)
                c1 = (210, 210, 210) if bg[0] > 80 else (40, 40, 40)
                c2 = (230, 230, 230) if bg[0] > 80 else (28, 28, 28)
                for yy in range(0, 256, 16):
                    for xx in range(0, 256, 16):
                        cd.rectangle(
                            [xx, yy, xx + 15, yy + 15],
                            fill=(c1 if (xx // 16 + yy // 16) % 2 == 0 else c2),
                        )
                sheet.paste(cell, (x, y_icon), cell)
            ox = x + (cw - s) // 2
            oy = y_icon + (256 - s) // 2
            sheet.paste(icon, (ox, oy), icon)
            label = f"{s}px"
            tw = draw.textlength(label)
            lc = (30, 30, 30) if bg[0] > 80 else (230, 230, 230)
            draw.text((x + (cw - tw) / 2, y_icon + 256 + 6), label, fill=lc)
            x += cw + gap

    blit_row(pad + 18, (244, 244, 246), "Light")
    blit_row(pad + 18 + row_h + 24, (18, 18, 22), "Dark")
    return sheet


def load_source() -> Image.Image:
    for p in SRC_CANDIDATES:
        if os.path.isfile(p):
            return Image.open(p)
    raise FileNotFoundError("stalgia-icon-v2.jpg not found in uploads/ or resources/")


def main() -> None:
    src = load_source()
    master = crop_tile(src)
    frames: dict[int, Image.Image] = {}
    ico_entries: list[tuple[int, bytes]] = []
    for s in SIZES:
        img = render_size(master, s)
        frames[s] = img
        raw = rgba_bytes(img)
        payload = png_bytes(s, s, raw) if s >= 96 else ico_bmp32(s, raw)
        ico_entries.append((s, payload))

    write_ico(ICO_OUT, ico_entries)
    as_straight(frames[256]).save(PNG_OUT, "PNG")
    as_straight(frames[128]).save(LOGO_OUT, "PNG")
    contact_sheet({s: as_straight(im) for s, im in frames.items()}).save(SHEET_OUT, "PNG")
    print(f"wrote {ICO_OUT} ({os.path.getsize(ICO_OUT)} bytes)")
    print(f"wrote {PNG_OUT} ({os.path.getsize(PNG_OUT)} bytes)")
    print(f"wrote {LOGO_OUT} ({os.path.getsize(LOGO_OUT)} bytes)")
    print(f"wrote {SHEET_OUT} ({os.path.getsize(SHEET_OUT)} bytes)")


if __name__ == "__main__":
    main()
