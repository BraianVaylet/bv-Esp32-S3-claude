#!/usr/bin/env python3
"""
Generates the project logo for the bv family.

Kept as a generator rather than a checked-in binary so the geometry and the
palette stay reviewable, and so a size or colour tweak is a one-line edit
instead of a round trip through an image editor. No third-party dependencies:
the PNG is written straight out with zlib.

    python tools/make_logo.py

Family conventions, measured from the existing logos in bv-landing rather than
guessed: 1600x1600 canvas, rounded rectangle with a 331px corner radius,
#1F1E1D background, #C96442 terracotta subject, and one lighter accent.
"""
import struct
import zlib
from pathlib import Path

# ---- family palette, sampled from bv-landing/public/logos/projects ----
BG        = (0x1F, 0x1E, 0x1D)
TERRACOTA = (0xC9, 0x64, 0x42)
OFFWHITE  = (0xF4, 0xF2, 0xEC)
TRACK     = (0x33, 0x2F, 0x2C)

SIZE      = 1600
RADIUS    = 331          # measured: the flat top edge runs x=331..1269
SS        = 3            # supersampling per axis

# The same 16x9 grid the firmware rasterises onto the panel, so the logo and
# the device show the identical mark. Keep in sync with src/claude_logo.cpp.
CLAWD = [
    "....##########..",
    "....##########..",
    "....##.####.##..",
    "....##.####.##..",
    "..##############",
    "..##############",
    "....##########..",
    ".....#.#..#.#...",
    ".....#.#..#.#...",
]
COLS, ROWS = 16, 9

# ---- layout -----------------------------------------------------------------
BLOCK      = 68
ART_W      = COLS * BLOCK          # 1088
ART_H      = ROWS * BLOCK          # 612
BAR_H      = 84
GAP        = 110
BAR_FILL   = 0.62                  # a plausible mid-usage reading

CONTENT_H  = ART_H + GAP + BAR_H
ART_X      = (SIZE - ART_W) // 2
ART_Y      = (SIZE - CONTENT_H) // 2
BAR_X      = ART_X
BAR_Y      = ART_Y + ART_H + GAP


def in_round_rect(x, y, w, h, r):
    if x < 0 or y < 0 or x > w or y > h:
        return False
    cx = r if x < r else (w - r if x > w - r else x)
    cy = r if y < r else (h - r if y > h - r else y)
    if cx == x or cy == y:
        return True
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_capsule(x, y, bx, by, bw, bh):
    return in_round_rect(x - bx, y - by, bw, bh, bh / 2)


def in_clawd(x, y):
    gx = int((x - ART_X) // BLOCK)
    gy = int((y - ART_Y) // BLOCK)
    if not (0 <= gx < COLS and 0 <= gy < ROWS):
        return False
    return CLAWD[gy][gx] == '#'


def sample(x, y):
    """Colour at a point, or None outside the tile."""
    if not in_round_rect(x, y, SIZE, SIZE, RADIUS):
        return None
    if in_clawd(x, y):
        return TERRACOTA
    if in_capsule(x, y, BAR_X, BAR_Y, ART_W, BAR_H):
        return OFFWHITE if x <= BAR_X + ART_W * BAR_FILL else TRACK
    return BG


def render(size):
    scale = size / SIZE
    out = bytearray(size * size * 4)
    n = SS * SS

    for py in range(size):
        for px in range(size):
            r = g = b = opaque = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px + (sx + 0.5) / SS) / scale
                    y = (py + (sy + 0.5) / SS) / scale
                    c = sample(x, y)
                    if c:
                        r += c[0]; g += c[1]; b += c[2]; opaque += 1
            i = (py * size + px) * 4
            if opaque:
                out[i]     = r // opaque
                out[i + 1] = g // opaque
                out[i + 2] = b // opaque
                out[i + 3] = opaque * 255 // n
    return bytes(out)


def write_png(path, size, rgba):
    raw = b''.join(b'\x00' + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    Path(path).write_bytes(png)
    return len(png)


def write_svg(path):
    def hx(c):
        return '#%02X%02X%02X' % c

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}" '
        f'width="{SIZE}" height="{SIZE}" role="img" aria-label="Claude usage dashboard">',
        f'<rect width="{SIZE}" height="{SIZE}" rx="{RADIUS}" fill="{hx(BG)}"/>',
        f'<g fill="{hx(TERRACOTA)}">',
    ]
    # Merge each run of filled cells into one rect so the file stays small.
    for gy, row in enumerate(CLAWD):
        gx = 0
        while gx < COLS:
            if row[gx] != '#':
                gx += 1
                continue
            run = 0
            while gx + run < COLS and row[gx + run] == '#':
                run += 1
            parts.append(f'<rect x="{ART_X + gx * BLOCK}" y="{ART_Y + gy * BLOCK}" '
                         f'width="{run * BLOCK}" height="{BLOCK}"/>')
            gx += run
    parts.append('</g>')
    parts.append(f'<rect x="{BAR_X}" y="{BAR_Y}" width="{ART_W}" height="{BAR_H}" '
                 f'rx="{BAR_H // 2}" fill="{hx(TRACK)}"/>')
    parts.append(f'<rect x="{BAR_X}" y="{BAR_Y}" width="{round(ART_W * BAR_FILL)}" '
                 f'height="{BAR_H}" rx="{BAR_H // 2}" fill="{hx(OFFWHITE)}"/>')
    parts.append('</svg>')

    Path(path).write_text('\n'.join(parts), encoding='utf-8')
    return len('\n'.join(parts))


if __name__ == '__main__':
    out = Path(__file__).resolve().parent.parent / 'assets' / 'logo'
    out.mkdir(parents=True, exist_ok=True)

    for size in (1600, 512):
        name = out / f'bv-esp32-s3-claude{"" if size == 1600 else "-512"}.png'
        n = write_png(name, size, render(size))
        print(f'{name.name:34} {size}x{size}  {n // 1024}KB')

    svg = out / 'bv-esp32-s3-claude.svg'
    print(f'{svg.name:34} {write_svg(svg)} bytes')
