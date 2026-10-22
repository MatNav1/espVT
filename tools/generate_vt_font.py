#!/usr/bin/env python3
"""Generate a compact, antialiased monospace font asset for the VT renderer."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FONT_SIZES = ((8, 12), (12, 16), (16, 24), (32, 48), (48, 64))
ENCODINGS = ("iso-8859-1", "iso-8859-2", "iso-8859-15")


def supported_codepoints() -> list[int]:
    codepoints = set(range(0x20, 0x7F))
    for encoding in ENCODINGS:
        for value in range(0xA0, 0x100):
            codepoints.add(ord(bytes((value,)).decode(encoding)))
    return sorted(codepoints)


def render_glyph(font_path: Path, codepoint: int, width: int, height: int) -> bytes:
    supersampling = 4
    canvas_width = width * supersampling
    canvas_height = height * supersampling
    font = ImageFont.truetype(str(font_path), height * supersampling)
    image = Image.new("L", (canvas_width, canvas_height), 0)
    draw = ImageDraw.Draw(image)
    draw.text(
        (canvas_width // 2, canvas_height // 2),
        chr(codepoint),
        font=font,
        fill=255,
        anchor="mm",
        stroke_width=0,
    )
    image = image.resize((width, height), Image.Resampling.LANCZOS)

    packed = bytearray((width * height + 3) // 4)
    for index, value in enumerate(image.getdata()):
        alpha = min(3, (int(value) + 42) // 85)
        packed[index // 4] |= alpha << ((index % 4) * 2)
    return bytes(packed)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    codepoints = supported_codepoints()
    header_size = 12 + len(codepoints) * 2 + len(FONT_SIZES) * 12
    offsets: list[tuple[int, int, int, int]] = []
    payload = bytearray()

    for width, height in FONT_SIZES:
        stride = (width * height + 3) // 4
        offsets.append((width, height, header_size + len(payload), stride))
        for codepoint in codepoints:
            payload.extend(render_glyph(args.font, codepoint, width, height))

    output = bytearray(struct.pack("<4sHHHH", b"VTF1", 1, len(codepoints), len(FONT_SIZES), 0))
    output.extend(struct.pack(f"<{len(codepoints)}H", *codepoints))
    for entry in offsets:
        output.extend(struct.pack("<HHII", *entry))
    output.extend(payload)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f"Generated {args.output}: {len(codepoints)} glyphs, {len(output)} bytes")


if __name__ == "__main__":
    main()
