#!/usr/bin/env python3
"""Decode a TSPL job as written by the Polono raster-tspl filter.

Usage: decode_tspl.py JOB.tspl [--pbm OUTPREFIX] [--ascii COLS]

Prints every command in order. BITMAP payloads are shown as a summary
(position, size, byte count, share of black dots) instead of raw bytes.
--pbm writes each bitmap as a PBM image (black = printed dot).
--ascii prints a downsampled sketch of each bitmap ('#' = printed).

Bit polarity: TSPL BITMAP data is 1 bit per dot, MSB = leftmost dot,
and a 0 bit is a printed (black) dot, 1 is blank. This is checked
against the test pattern's black corner triangle, not assumed.
"""
import re
import sys

BITMAP_RE = re.compile(rb"BITMAP (\d+),(\d+),(\d+),(\d+),(\d+),")


def parse(data):
    """Yield ('nul', count) | ('cmd', text) | ('bitmap', x, y, wbytes, h, mode, payload)."""
    i, n = 0, len(data)
    while i < n:
        if data[i] == 0:
            j = i
            while j < n and data[j] == 0:
                j += 1
            yield ("nul", j - i)
            i = j
            continue
        if data[i] in b"\r\n":
            i += 1
            continue
        m = BITMAP_RE.match(data, i)
        if m:
            x, y, wb, h, mode = map(int, m.groups())
            start = m.end()
            payload = data[start:start + wb * h]
            yield ("bitmap", x, y, wb, h, mode, payload)
            i = start + wb * h
            continue
        j = data.find(b"\r\n", i)
        if j < 0:
            j = n
        yield ("cmd", data[i:j].decode("latin-1"))
        i = j


def dots(payload, wb, h):
    """Rows of booleans, True = printed dot (0 bit)."""
    rows = []
    for r in range(h):
        row = payload[r * wb:(r + 1) * wb]
        bits = []
        for byte in row:
            for b in range(7, -1, -1):
                bits.append(not (byte >> b) & 1)
        rows.append(bits)
    return rows


def ascii_art(rows, cols):
    h, w = len(rows), len(rows[0]) if rows else 0
    step = max(1, w // cols)
    vstep = step * 2  # terminal cells are ~2:1
    out = []
    for r in range(0, h, vstep):
        line = []
        for c in range(0, w, step):
            block = [rows[rr][cc] for rr in range(r, min(r + vstep, h))
                     for cc in range(c, min(c + step, w))]
            frac = sum(block) / len(block)
            line.append("#" if frac > 0.5 else "+" if frac > 0.15 else "." if frac > 0 else " ")
        out.append("".join(line).rstrip())
    return "\n".join(out)


def main():
    args = sys.argv[1:]
    path = args[0]
    pbm = args[args.index("--pbm") + 1] if "--pbm" in args else None
    cols = int(args[args.index("--ascii") + 1]) if "--ascii" in args else 0
    data = open(path, "rb").read()
    k = 0
    for item in parse(data):
        if item[0] == "nul":
            print(f"<{item[1]} x NUL>")
        elif item[0] == "cmd":
            print(item[1])
        else:
            _, x, y, wb, h, mode, payload = item
            rows = dots(payload, wb, h)
            black = sum(map(sum, rows))
            short = " (TRUNCATED)" if len(payload) < wb * h else ""
            print(f"BITMAP {x},{y},{wb},{h},{mode},<{len(payload)} bytes = {wb * 8} x {h} dots, "
                  f"{100 * black / (wb * 8 * h):.1f}% printed>{short}")
            if pbm:
                with open(f"{pbm}-{k}.pbm", "wb") as f:
                    f.write(b"P4\n%d %d\n" % (wb * 8, h))
                    f.write(bytes(b ^ 0xFF for b in payload))  # PBM: 1 = black
            if cols:
                print(ascii_art(rows, cols))
            k += 1


if __name__ == "__main__":
    main()
