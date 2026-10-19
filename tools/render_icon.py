#!/usr/bin/env python3
"""Renders resources/midiworkbench.png and .ico from resources/midiworkbench.svg.

The icon source uses only <rect> elements (with rx corner radius and fill), so this script
rasterizes those rectangles directly with 4x4 supersampling and writes a PNG plus an ICO that
embeds the PNG (supported by Windows Vista and later). Standard library only.

Usage: python tools/render_icon.py
"""
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "resources" / "midiworkbench.svg"
SIZE = 256
SAMPLES = 4


def parse_rects(text):
    rects = []
    group_fill = None
    for token in re.finditer(r"<g[^>]*fill=\"(#[0-9a-fA-F]{3,6})\"[^>]*>|</g>|<rect([^>]*)/>", text):
        if token.group(0).startswith("<g"):
            group_fill = token.group(1)
        elif token.group(0) == "</g>":
            group_fill = None
        else:
            attributes = dict(re.findall(r'(\w+)="([^"]*)"', token.group(2)))
            fill = attributes.get("fill", group_fill or "#000000")
            rects.append((float(attributes["x"]), float(attributes["y"]), float(attributes["width"]),
                          float(attributes["height"]), float(attributes.get("rx", 0)), fill))
    return rects


def color(hex_text):
    hex_text = hex_text.lstrip("#")
    if len(hex_text) == 3:
        hex_text = "".join(c * 2 for c in hex_text)
    return tuple(int(hex_text[i:i + 2], 16) for i in (0, 2, 4))


def inside(px, py, rect):
    x, y, width, height, radius, _ = rect
    if px < x or py < y or px > x + width or py > y + height:
        return False
    cx = min(max(px, x + radius), x + width - radius)
    cy = min(max(py, y + radius), y + height - radius)
    return (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius


def render(rects):
    pixels = bytearray(SIZE * SIZE * 4)
    colors = [color(rect[5]) for rect in rects]
    for row in range(SIZE):
        for column in range(SIZE):
            red = green = blue = covered = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    px = column + (sx + 0.5) / SAMPLES
                    py = row + (sy + 0.5) / SAMPLES
                    sample = None
                    for rect, rgb in zip(rects, colors):
                        if inside(px, py, rect):
                            sample = rgb
                    if sample:
                        red += sample[0]
                        green += sample[1]
                        blue += sample[2]
                        covered += 1
            offset = (row * SIZE + column) * 4
            if covered:
                pixels[offset:offset + 4] = bytes((red // covered, green // covered, blue // covered,
                                                   covered * 255 // (SAMPLES * SAMPLES)))
    return bytes(pixels)


def png(pixels):
    raw = b"".join(b"\x00" + pixels[row * SIZE * 4:(row + 1) * SIZE * 4] for row in range(SIZE))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    image = png(render(parse_rects(SVG.read_text(encoding="utf-8"))))
    (ROOT / "resources" / "midiworkbench.png").write_bytes(image)
    icon = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(image), 22) + image
    (ROOT / "resources" / "midiworkbench.ico").write_bytes(icon)
    print("wrote resources/midiworkbench.png and resources/midiworkbench.ico")


if __name__ == "__main__":
    main()
