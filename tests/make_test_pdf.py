#!/usr/bin/env python3
"""Write a test-label PDF with no third-party dependencies.

Usage: make_test_pdf.py WIDTH_MM HEIGHT_MM OUT.pdf [PAGES]

Each page carries features that make the decoded bitmap easy to check:
  - a 1 pt border on the page edge (shows clipping/offset),
  - a solid black triangle in the top-left corner (shows rotation/mirroring),
  - a 10-step grey ramp, 0 % to 100 % black (shows the 8-bit -> 1-bit
    threshold or dither),
  - the label size and page number as text.
"""
import sys

MM = 72 / 25.4


def page_stream(w, h, label, page_no):
    ops = ["0 g 0 G 1 w"]
    ops.append(f"0.5 0.5 {w - 1:.2f} {h - 1:.2f} re S")
    t = min(w, h) * 0.25
    ops.append(f"0 {h:.2f} m {t:.2f} {h:.2f} l 0 {h - t:.2f} l f")
    # grey ramp across the middle third
    rx, rw = w * 0.1, w * 0.8
    ry, rh = h * 0.35, h * 0.15
    step = rw / 10
    for i in range(10):
        grey = 1 - (i + 1) / 10  # PDF gray: 1 = white, 0 = black
        ops.append(f"{grey:.2f} g {rx + i * step:.2f} {ry:.2f} {step:.2f} {rh:.2f} re f")
    ops.append("0 g")
    size = max(6, min(w, h) * 0.09)
    ops.append(f"BT /F1 {size:.1f} Tf {w * 0.3:.2f} {h * 0.75:.2f} Td ({label}) Tj ET")
    ops.append(f"BT /F1 {size:.1f} Tf {w * 0.1:.2f} {h * 0.12:.2f} Td (page {page_no}) Tj ET")
    return "\n".join(ops).encode()


def build(w_mm, h_mm, pages):
    w, h = w_mm * MM, h_mm * MM
    label = f"{w_mm:g} x {h_mm:g} mm"
    objs = {1: b"<< /Type /Catalog /Pages 2 0 R >>",
            3: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"}
    kids = []
    n = 4
    for p in range(1, pages + 1):
        stream = page_stream(w, h, label, p)
        objs[n + 1] = b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream"
        objs[n] = (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {w:.2f} {h:.2f}] "
                   f"/Resources << /Font << /F1 3 0 R >> >> /Contents {n + 1} 0 R >>").encode()
        kids.append(f"{n} 0 R")
        n += 2
    objs[2] = f"<< /Type /Pages /Kids [{' '.join(kids)}] /Count {pages} >>".encode()

    out = bytearray(b"%PDF-1.4\n")
    offsets = {}
    for i in sorted(objs):
        offsets[i] = len(out)
        out += b"%d 0 obj\n" % i + objs[i] + b"\nendobj\n"
    xref = len(out)
    count = max(objs) + 1
    out += b"xref\n0 %d\n0000000000 65535 f \n" % count
    for i in range(1, count):
        out += b"%010d 00000 n \n" % offsets[i]
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (count, xref)
    return bytes(out)


if __name__ == "__main__":
    w_mm, h_mm, path = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    pages = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    with open(path, "wb") as f:
        f.write(build(w_mm, h_mm, pages))
