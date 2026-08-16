"""
Synthetic Image Pattern and Raw Pixel Stream Generators.
Generates deterministic gradients, high-frequency test patterns, noise, and geometric benchmarks.
Modern Python 3.12+ implementation using structural pattern matching and vectorized byte generators.
"""

from __future__ import annotations

import math
import struct


def create_bmp_bytes(width: int, height: int, pattern_type: str = "gradient", bpp: int = 24) -> bytes:
    """
    Generates an uncompressed Windows Bitmap (BMP) in memory with fast binary encoding.
    Supports 24-bit BGR and 32-bit BGRA.
    """
    bytes_per_pixel = bpp // 8
    row_stride = ((width * bytes_per_pixel + 3) // 4) * 4
    pixel_data_size = row_stride * height
    file_header_size = 14
    info_header_size = 40
    total_file_size = file_header_size + info_header_size + pixel_data_size

    # BITMAPFILEHEADER
    file_header = struct.pack('<2sIHHI', b'BM', total_file_size, 0, 0, file_header_size + info_header_size)

    # BITMAPINFOHEADER
    info_header = struct.pack('<IIIHHIIIIII', info_header_size, width, height, 1, bpp, 0, pixel_data_size, 2835, 2835, 0, 0)

    padding_bytes = b'\x00' * (row_stride - width * bytes_per_pixel)

    match pattern_type:
        case "gradient":
            num_patterns = min(256, height)
            cached_rows: list[bytes] = []
            for step in range(num_patterns):
                y_ratio = step / max(1, num_patterns - 1)
                row = bytearray(width * bytes_per_pixel)
                for x in range(width):
                    x_ratio = x / max(1, width - 1)
                    b = int(255 * x_ratio)
                    g = int(255 * y_ratio)
                    r = int(255 * (1.0 - (x_ratio + y_ratio) * 0.5))
                    offset = x * bytes_per_pixel
                    row[offset] = b
                    row[offset + 1] = g
                    row[offset + 2] = r
                    if bytes_per_pixel == 4:
                        row[offset + 3] = 255
                cached_rows.append(bytes(row) + padding_bytes)

            rows = [cached_rows[int(y * num_patterns / height)] for y in range(height)]
            return file_header + info_header + b''.join(rows)

        case "checkerboard":
            cell_size = max(16, min(width, height) // 32)
            row_even = bytearray(width * bytes_per_pixel)
            row_odd = bytearray(width * bytes_per_pixel)
            for x in range(width):
                c = (x // cell_size) % 2
                v_even = 240 if c == 0 else 30
                v_odd = 30 if c == 0 else 240
                offset = x * bytes_per_pixel
                row_even[offset] = v_even
                row_even[offset + 1] = v_even
                row_even[offset + 2] = v_even
                row_odd[offset] = v_odd
                row_odd[offset + 1] = v_odd
                row_odd[offset + 2] = v_odd
                if bytes_per_pixel == 4:
                    row_even[offset + 3] = 255
                    row_odd[offset + 3] = 255

            b_row_even = bytes(row_even) + padding_bytes
            b_row_odd = bytes(row_odd) + padding_bytes
            rows = [b_row_even if ((y // cell_size) % 2 == 0) else b_row_odd for y in range(height)]
            return file_header + info_header + b''.join(rows)

        case "high_frequency":
            row_a = bytearray(width * bytes_per_pixel)
            row_b = bytearray(width * bytes_per_pixel)
            for x in range(width):
                val_a = 255 if (x % 2 == 0) else 0
                val_b = 0 if (x % 2 == 0) else 255
                offset = x * bytes_per_pixel
                row_a[offset] = val_a
                row_a[offset + 1] = val_a
                row_a[offset + 2] = val_a
                row_b[offset] = val_b
                row_b[offset + 1] = val_b
                row_b[offset + 2] = val_b
                if bytes_per_pixel == 4:
                    row_a[offset + 3] = 255
                    row_b[offset + 3] = 255
            b_row_a = bytes(row_a) + padding_bytes
            b_row_b = bytes(row_b) + padding_bytes
            rows = [b_row_a if (y % 2 == 0) else b_row_b for y in range(height)]
            return file_header + info_header + b''.join(rows)

        case _:
            row = bytearray(width * bytes_per_pixel)
            for x in range(width):
                offset = x * bytes_per_pixel
                row[offset] = 64
                row[offset + 1] = 128
                row[offset + 2] = 192
                if bytes_per_pixel == 4:
                    row[offset + 3] = 255
            b_row = bytes(row) + padding_bytes
            return file_header + info_header + (b_row * height)


def create_tga_bytes(width: int, height: int, pattern_type: str = "gradient") -> bytes:
    """Generates an uncompressed truecolor TGA image."""
    header = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, width, height, 24, 0x20)

    num_patterns = min(128, height)
    cached_rows: list[bytes] = []
    for step in range(num_patterns):
        y_ratio = step / max(1, num_patterns - 1)
        row = bytearray(width * 3)
        for x in range(width):
            x_ratio = x / max(1, width - 1)
            b = int(255 * math.sin(x_ratio * math.pi) ** 2)
            g = int(255 * math.cos(y_ratio * math.pi) ** 2)
            r = int(255 * ((x_ratio + y_ratio) * 0.5))
            offset = x * 3
            row[offset] = b
            row[offset + 1] = g
            row[offset + 2] = r
        cached_rows.append(bytes(row))

    rows = [cached_rows[int(y * num_patterns / height)] for y in range(height)]
    return header + b''.join(rows)


def create_qoi_bytes(width: int, height: int, pattern_type: str = "gradient") -> bytes:
    """Generates a Quite OK Image (QOI) format file directly."""
    eff_w = min(width, 1024)
    eff_h = min(height, 1024)
    header = struct.pack('>4sIIBB', b'qoif', eff_w, eff_h, 4, 0)

    bytes_out = bytearray(header)
    pixel_count = eff_w * eff_h
    r, g, b, a = 100, 150, 200, 255
    bytes_out.append(0xff)  # QOI_OP_RGBA
    bytes_out.extend([r, g, b, a])

    remaining = pixel_count - 1
    while remaining > 0:
        chunk = min(62, remaining)
        bytes_out.append(0xc0 | (chunk - 1))  # QOI_OP_RUN
        remaining -= chunk

    bytes_out.extend(b'\x00\x00\x00\x00\x00\x00\x00\x01')
    return bytes(bytes_out)
