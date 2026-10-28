#!/usr/bin/env python3
"""Generate a small synthetic star-field FITS sequence for SIRIL smoke tests.

Pure Python (no astropy): writes 16-bit integer FITS images with a handful of
Gaussian stars, a light gradient and read noise.  Each frame is shifted by a
few pixels so registration has something to solve.

    python3 scripts/make-test-fits.py --out /tmp/siril-test --count 5
"""

import argparse
import math
import random
import struct
from pathlib import Path


def card(key, value, comment=""):
    if isinstance(value, str):
        body = f"{key:<8}= '{value:<68}'"
    elif isinstance(value, bool):
        body = f"{key:<8}= {'T' if value else 'F':>20}"
    else:
        body = f"{key:<8}= {value:>20}"
    text = (body + (f" / {comment}" if comment else "")).ljust(80)[:80]
    return text.encode("ascii")


def make_frame(size, stars, shift, rng, background=800.0):
    data = [[0] * size for _ in range(size)]
    # gradient + noise
    for y in range(size):
        row = data[y]
        for x in range(size):
            row[x] = background + 0.08 * (x + y)
    for (sx, sy, amp, sigma) in stars:
        cx = sx + shift
        cy = sy + shift * 0.6
        radius = int(sigma * 4) + 1
        for dy in range(-radius, radius + 1):
            yy = int(cy) + dy
            if not (0 <= yy < size):
                continue
            for dx in range(-radius, radius + 1):
                xx = int(cx) + dx
                if not (0 <= xx < size):
                    continue
                value = amp * math.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * sigma * sigma))
                data[yy][xx] += value
    for y in range(size):
        row = data[y]
        for x in range(size):
            row[x] += rng.gauss(0.0, 6.0)
            row[x] = max(0, min(65535, int(row[x])))
    return data


def write_fits(path, data, header_extra):
    height = len(data)
    width = len(data[0])
    cards = [
        card("SIMPLE", True, "conforms to FITS standard"),
        card("BITPIX", 16, "16-bit integers"),
        card("NAXIS", 2, "two-dimensional image"),
        card("NAXIS1", width),
        card("NAXIS2", height),
        card("BZERO", 32768, "unsigned 16-bit offset"),
        card("BSCALE", 1),
    ]
    for key, value in header_extra.items():
        cards.append(card(key, value))
    cards.append(card("END", ""))
    header = b"".join(cards)
    header += b" " * ((2880 - len(header) % 2880) % 2880)

    payload = bytearray()
    for row in data:
        payload.extend(struct.pack(f">{width}h", *[v - 32768 for v in row]))
    payload.extend(b"\x00" * ((2880 - len(payload) % 2880) % 2880))
    Path(path).write_bytes(header + bytes(payload))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=5)
    ap.add_argument("--size", type=int, default=512)
    ap.add_argument("--stars", type=int, default=45)
    ap.add_argument("--seed", type=int, default=20261028)
    ap.add_argument("--prefix", default="light")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    stars = []
    for _ in range(args.stars):
        x = rng.uniform(40, args.size - 40)
        y = rng.uniform(40, args.size - 40)
        amp = rng.uniform(1500, 9000)
        sigma = rng.uniform(1.2, 2.6)
        stars.append((x, y, amp, sigma))

    for index in range(args.count):
        shift = index * 1.7
        data = make_frame(args.size, stars, shift, rng)
        header = {
            "EXPTIME": round(10.0 + index, 1),
            "DATE-OBS": f"2026-10-28T18:{40 + index:02d}:00",
            "INSTRUME": "SYNTHETIC",
            "OBJECT": "SIRIL-OHOS-TEST",
            "GAIN": 1.0,
        }
        write_fits(out / f"{args.prefix}_{index + 1:04d}.fit", data, header)
    print(f"wrote {args.count} frames to {out}")


if __name__ == "__main__":
    main()
