#!/usr/bin/env python3
"""Contact sheet of tray/title icons at 100 / 125 / 150% DPI.

Fant-scales resources/stalgia-256.png to the SM_CXSMICON / SM_CXICON
sizes Windows reports at those DPIs, plus a 4x zoom of each so jagged
edges are obvious.
"""
from __future__ import annotations

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "resources", "stalgia-256.png")
OUT = os.path.join(ROOT, "resources", "stalgia-dpi-icons.png")

# GetSystemMetricsForDpi(SM_CXSMICON / SM_CXICON) at these DPI values.
SCALES = (
    ("100%", 96, 16, 32),
    ("125%", 120, 20, 40),
    ("150%", 144, 24, 48),
)


def fant(src: Image.Image, px: int) -> Image.Image:
    return src.convert("RGBA").resize((px, px), Image.Resampling.LANCZOS)


def zoom4(img: Image.Image) -> Image.Image:
    return img.resize((img.width * 4, img.height * 4), Image.Resampling.NEAREST)


def main() -> None:
    src = Image.open(SRC).convert("RGBA")
    pad = 28
    col_w = 220
    row_h = 280
    w = pad * 2 + col_w * len(SCALES)
    h = pad * 2 + 36 + row_h
    sheet = Image.new("RGBA", (w, h), (18, 18, 22, 255))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 16)
        small = ImageFont.truetype("DejaVuSans.ttf", 12)
    except OSError:
        font = ImageFont.load_default()
        small = font
    draw.text((pad, 12), "Stalgia icons at 100 / 125 / 150% DPI (LANCZOS from 256 px)", fill=(230, 230, 230), font=font)

    checker = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    cd = ImageDraw.Draw(checker)
    for yy in range(0, 64, 8):
        for xx in range(0, 64, 8):
            cd.rectangle([xx, yy, xx + 7, yy + 7], fill=(40, 40, 46) if (xx // 8 + yy // 8) % 2 == 0 else (28, 28, 32))

    for i, (label, dpi, sm, lg) in enumerate(SCALES):
        x0 = pad + i * col_w
        y0 = pad + 40
        draw.text((x0, y0), f"{label}  dpi {dpi}", fill=(180, 186, 196), font=small)
        small_icon = fant(src, sm)
        large_icon = fant(src, lg)
        z_small = zoom4(small_icon)
        z_large = zoom4(large_icon)
        # native
        sheet.paste(checker.crop((0, 0, sm + 8, sm + 8)), (x0, y0 + 24))
        sheet.paste(small_icon, (x0 + 4, y0 + 28), small_icon)
        sheet.paste(checker.crop((0, 0, lg + 8, lg + 8)), (x0 + 80, y0 + 24))
        sheet.paste(large_icon, (x0 + 84, y0 + 28), large_icon)
        draw.text((x0, y0 + 24 + lg + 12), f"tray {sm}px   title {lg}px", fill=(140, 146, 156), font=small)
        # 4x zoom
        zy = y0 + 24 + lg + 36
        sheet.paste(z_small, (x0, zy), z_small)
        sheet.paste(z_large, (x0 + 90, zy), z_large)
        draw.text((x0, zy + max(z_small.height, z_large.height) + 6), "4x nearest zoom", fill=(120, 126, 136), font=small)

    sheet.save(OUT, "PNG")
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
