#!/usr/bin/env python3
"""Рисует иконки веб-приложения и пишет lib/ui_icons.h (PNG как массивы байт)."""
import io, os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (180, 192, 512)

def draw(kind, size):
    s = size * 4
    img = Image.new("RGB", (s, s))
    top, bot = ((79, 140, 255), (40, 70, 200)) if kind == "scale" else ((52, 199, 150), (16, 120, 110))
    px = img.load()
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(s):
            px[x, y] = c
    d = ImageDraw.Draw(img, "RGBA")
    u = s / 100.0
    white = (255, 255, 255, 245)
    soft = (255, 255, 255, 90)
    if kind == "scale":
        d.rounded_rectangle([22*u, 30*u, 78*u, 74*u], radius=9*u, fill=white)      # платформа
        d.pieslice([34*u, 36*u, 66*u, 68*u], 180, 360, fill=soft)                   # шкала
        d.pieslice([34*u, 36*u, 66*u, 68*u], 180, 360, outline=(40, 70, 200, 255), width=int(2.2*u))
        d.line([50*u, 52*u, 58*u, 42*u], fill=(40, 70, 200, 255), width=int(2.6*u))  # стрелка
        d.ellipse([47.5*u, 49.5*u, 52.5*u, 54.5*u], fill=(40, 70, 200, 255))
        d.rounded_rectangle([30*u, 66*u, 70*u, 70*u], radius=2*u, fill=soft)
    else:
        d.rounded_rectangle([20*u, 26*u, 80*u, 64*u], radius=7*u, fill=white)        # экран
        d.rounded_rectangle([25*u, 31*u, 75*u, 59*u], radius=4*u, fill=(16, 120, 110, 255))
        for i, h in enumerate((10, 16, 7, 13)):                                       # столбики
            x = 31*u + i*11*u
            d.rounded_rectangle([x, (55 - h)*u, x + 7*u, 55*u], radius=1.5*u, fill=white)
        d.rounded_rectangle([42*u, 64*u, 58*u, 72*u], radius=2*u, fill=white)         # ножка
        d.rounded_rectangle([34*u, 72*u, 66*u, 76*u], radius=2*u, fill=white)
    img = img.resize((size, size), Image.LANCZOS)
    return img.quantize(colors=48, method=Image.MEDIANCUT, dither=Image.NONE)

def png(kind, size):
    b = io.BytesIO()
    draw(kind, size).save(b, "PNG", optimize=True)
    return b.getvalue()

def main():
    out = ["#pragma once", "// Сгенерировано ui/make_icons.py — не править вручную.", "#include <stddef.h>", "#include <stdint.h>", "", "namespace ui {", ""]
    for kind in ("scale", "screen"):
        for size in SIZES:
            data = png(kind, size)
            out.append(f"static const uint8_t ICON_{kind.upper()}_{size}[] = {{")
            for i in range(0, len(data), 24):
                out.append("  " + ",".join(f"0x{c:02x}" for c in data[i:i+24]) + ",")
            out.append("};")
            out.append(f"static const size_t ICON_{kind.upper()}_{size}_LEN = sizeof(ICON_{kind.upper()}_{size});")
            out.append("")
            print(kind, size, len(data), "bytes")
    out.append("}  // namespace ui")
    open(os.path.join(ROOT, "lib", "ui_icons.h"), "w").write("\n".join(out) + "\n")
    # превью для проверки глазами
    if os.environ.get("PREVIEW"):
        sheet = Image.new("RGB", (512*2+60, 512+40), "white")
        for i, k in enumerate(("scale", "screen")):
            sheet.paste(Image.open(io.BytesIO(png(k, 512))).convert("RGB"), (20 + i*(512+20), 20))
        sheet.save(os.environ["PREVIEW"])

main()
