#!/usr/bin/env python3
"""Convert a BMP logo to a cropped RGB565 C++ header using only stdlib."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


WHITE_THRESHOLD = 248


def read_bmp(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise ValueError("Expected a BMP file")

    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError("Unsupported BMP header")

    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits_per_pixel = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if width <= 0 or signed_height == 0 or planes != 1:
        raise ValueError("Invalid BMP dimensions")
    if bits_per_pixel not in (24, 32) or compression != 0:
        raise ValueError("Only uncompressed 24-bit or 32-bit BMP input is supported")

    height = abs(signed_height)
    bytes_per_pixel = bits_per_pixel // 8
    row_stride = ((width * bits_per_pixel + 31) // 32) * 4
    if offset + row_stride * height > len(data):
        raise ValueError("Truncated BMP pixel data")

    pixels: list[tuple[int, int, int]] = []
    for output_y in range(height):
        source_y = output_y if signed_height < 0 else height - output_y - 1
        row_start = offset + source_y * row_stride
        for x in range(width):
            pixel_start = row_start + x * bytes_per_pixel
            blue, green, red = data[pixel_start : pixel_start + 3]
            pixels.append((red, green, blue))
    return width, height, pixels


def crop_to_content(
    width: int, height: int, pixels: list[tuple[int, int, int]], padding: int
) -> tuple[int, int, list[tuple[int, int, int]]]:
    occupied = [
        (index % width, index // width)
        for index, (red, green, blue) in enumerate(pixels)
        if min(red, green, blue) < WHITE_THRESHOLD
    ]
    if not occupied:
        raise ValueError("Logo contains no non-white pixels")

    x_values, y_values = zip(*occupied)
    left = max(0, min(x_values) - padding)
    top = max(0, min(y_values) - padding)
    right = min(width, max(x_values) + padding + 1)
    bottom = min(height, max(y_values) + padding + 1)
    cropped_width = right - left
    cropped_height = bottom - top
    cropped = [
        pixels[y * width + x]
        for y in range(top, bottom)
        for x in range(left, right)
    ]
    return cropped_width, cropped_height, cropped


def resize(
    width: int,
    height: int,
    pixels: list[tuple[int, int, int]],
    max_width: int,
    max_height: int,
) -> tuple[int, int, list[tuple[int, int, int]]]:
    scale = min(max_width / width, max_height / height, 1.0)
    target_width = max(1, int(round(width * scale)))
    target_height = max(1, int(round(height * scale)))
    if (target_width, target_height) == (width, height):
        return width, height, pixels

    resized: list[tuple[int, int, int]] = []
    for target_y in range(target_height):
        source_y = min(height - 1, int((target_y + 0.5) * height / target_height))
        for target_x in range(target_width):
            source_x = min(width - 1, int((target_x + 0.5) * width / target_width))
            resized.append(pixels[source_y * width + source_x])
    return target_width, target_height, resized


def write_header(path: Path, width: int, height: int, pixels: list[tuple[int, int, int]]) -> None:
    values = [((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3) for red, green, blue in pixels]
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace logo_asset",
        "{",
        f"inline constexpr int WIDTH = {width};",
        f"inline constexpr int HEIGHT = {height};",
        "alignas(4) inline constexpr std::uint16_t PIXELS[] = {",
    ]
    for start in range(0, len(values), 12):
        chunk = ", ".join(f"0x{value:04X}" for value in values[start : start + 12])
        lines.append(f"    {chunk},")
    lines.extend(["};", "}", ""])
    path.write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-width", type=int, required=True)
    parser.add_argument("--max-height", type=int, required=True)
    parser.add_argument("--padding", type=int, default=12)
    args = parser.parse_args()

    width, height, pixels = read_bmp(args.input)
    width, height, pixels = crop_to_content(width, height, pixels, args.padding)
    width, height, pixels = resize(width, height, pixels, args.max_width, args.max_height)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_header(args.output, width, height, pixels)
    print(f"Generated {args.output.name}: {width}x{height} RGB565")


if __name__ == "__main__":
    main()
