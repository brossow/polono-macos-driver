# How the driver works

The PL60, PL70e-BT and PL420 all speak **TSPL**, the label-printer language
published by TSC, each in its own dialect. macOS turns whatever you print into a
CUPS raster: an uncompressed picture of each label, made by `cgpdftoraster` at
203 dpi. A CUPS *filter* then converts that raster into printer commands. This
driver is that filter, `rastertotspl`. The USB connection to the printer is
handled by macOS's own USB backend, as for any printer.

One filter serves all three printers. Each printer's PPD names its job format
with a `*PolonoProfile` line (`PL60`, `PL70e` or `PL420`).

| Printer | Made by | Polono's filter | This driver's profile |
|---|---|---|---|
| PL60 | HPRT | `raster-tspl` | [PL60](#pl60) |
| PL70e-BT | Beeprt | `rastertolabel` (ShippingPrinter) | [PL70e](#pl70e-bt) |
| PL420 | Beeprt | `rastertolabel` (LabelPrinter) | [PL420](#pl420) |

## How it was checked

Each printer's job format was taken from Polono's own Mac driver, which is
known to print correctly. Polono's filters are Intel-only, but run under Rosetta
they can write to a file instead of a printer. Each was given the same rasters
as this driver, and the two outputs were compared byte for byte:

| Printer | Jobs compared | Covering | Real printer |
|---|---|---|---|
| PL60 | 35, all identical | label sizes, every option, copies, multi-page jobs, combinations | Labels came out the same as with Polono's driver |
| PL70e-BT | 1,604, all identical | every choice of every option one at a time (all 511 Brightness values and all 256 Threshold values among them), every label size, custom sizes, copies, multi-page jobs, combinations of options, dithering and rotation | Not tried yet |
| PL420 | 174, all identical | every choice of every option, every label size, copies, multi-page jobs | Not tried yet |

For the PL60, the same job sent through macOS's real print system was also
byte-identical, for both the Apple Silicon and the Intel build. The image rules
(which grey prints, what Brightness does, each dithering pattern) were worked
out with synthetic test images, such as a 0 to 255 ramp, run through Polono's
filters, and then checked on the label captures. The PL70e-BT and PL420 PPDs
were also checked to make macOS produce exactly the same rasters as Polono's.

`tests/cases/<printer>` holds this filter's output for each printer (34, 42 and
25 cases), and `make test` checks that a build still produces them exactly.
Every PL70e-BT and PL420 case was also checked against Polono's filter.
Polono's driver output isn't distributed here.

## PL60

### The job

`\r\n` (CR LF) ends every command.

```
512 × 0x00                                   once per job
\r\n
SIZE <w> mm, <h> mm\r\n
[SET RIBBON OFF\r\n]                         Print Method = Direct thermal
[GAP 2 mm, 0 mm | BLINE 2 mm, 0 mm | GAP 0 mm, 0 mm\r\n]   Media Type
[OFFSET <n> mm\r\n]                          Stop Position Offset
[SHIFT <dots>\r\n]                           Vertical Start Position
[SPEED <n>\r\n]                              Print Speed
[DENSITY <n>\r\n]                            Darkness
SET TEAR ON\r\n  SET CUTTER OFF\r\n  SET PEEL OFF\r\n   After Printing = tear-off (default)
  or
SET TEAR OFF\r\n SET PEEL OFF\r\n  SET CUTTER OFF\r\n   After Printing = do nothing
then, for each label:
CLS\r\n
BITMAP 0,0,<width_bytes>,<height_dots>,1,<bitmap>\r\n
PRINT 1,1\r\n
```

Bracketed commands are sent only when that option is set to something other
than "Printer setting", and always in this order. The setup commands go once per
job, before the first label.

### SIZE

`SIZE` comes from the raster's `cupsPageSize` (points), converted as
`points / 72 × 25.4` and written with one decimal place. So the 4 × 6 preset
(283 × 425 pt) goes out as `SIZE 99.8 mm, 149.9 mm`, exactly as Polono's driver
sends it. Rotation doesn't change `SIZE`.

### BITMAP

- Always at `0,0`, mode 1, with the whole label as one bitmap and no compression.
- `width_bytes = ceil(cupsWidth / 8)`, `height_dots = cupsHeight`. Both swap after a 90° or 270° rotation.
- 1 bit per dot, leftmost dot in the most significant bit, rows top to bottom.
- **A 0 bit prints a dot. A 1 bit is blank.** Unused bits at the end of each row are 1.
- With Grey Handling = Sharp (8-bit raster, 255 = black), a dot prints when the value is **128 or more**. There's no dithering.
- With Grey Handling = Dithered (1-bit raster), macOS has already dithered the image, and set bits print.
- Transforms are applied in this order: rotate (1 = 90° clockwise, 2 = 180°, 3 = 90° anticlockwise), mirror left–right, invert.

### Copies

The filter always sends `PRINT 1,1`. Copies come from the raster:

- One-page jobs, and uncollated multi-page jobs: each raster page has
  `NumCopies = N` in its header, and the filter repeats that label N times. Page
  order is 1, 1, 2, 2.
- Collated multi-page jobs: macOS writes the pages out N times with
  `NumCopies = 1`. Page order is 1, 2, 1, 2.

### Options

The option keywords are the same as in Polono's PPD, so a printer switched over
from Polono's driver keeps its saved settings.

| Keyword | Values | TSPL |
|---|---|---|
| `PageSize` | presets or `Custom.WxHmm` | `SIZE` |
| `ColorOption` | `None` (8-bit) / `GrayScale` (1-bit) | bitmap rules above |
| `Orientation` | 0–3 | rotates the bitmap |
| `MediaMethod` | `None` / `1` | `1` → `SET RIBBON OFF` |
| `PaperType` | `None` / `1` / `2` / `3` | `GAP 2 mm, 0 mm` / `BLINE 2 mm, 0 mm` / `GAP 0 mm, 0 mm` |
| `PrintSpeed` | `None` / 2–6 | `SPEED n` |
| `PrintDarkness` | `None` / 0–15 | `DENSITY n` |
| `ShiftMove` | `None` / −12…12 mm | `SHIFT dots`, dots = (int)(mm × 203 / 25.4), truncated toward zero (5 → 39) |
| `FowardOffset` (sic) | `None` / −30…30 mm | `OFFSET n mm` |
| `PostAction` | `1` / `None` | tear/cutter/peel block, order as above |
| `MirrorImage` | `True` / `False` | mirrors the bitmap |
| `NegativeImage` | `True` / `False` | inverts the bitmap |

The copies argument (argv[4]) is ignored, as it is by Polono's driver.

## PL70e-BT

### The job

There's no preamble. The setup is sent again before every label, and every
command ends in CR LF.

```
SIZE <w> mm,<h> mm
SET RIBBON OFF
REFERENCE <x>,<y>
GAP 3 mm,0 mm | BLINE 0 mm,0 mm | GAP 0 mm,0 mm      Media Type: gaps / black marks / continuous
OFFSET <n> mm                                         Stop Position Offset
[DENSITY <n>]                                         Darkness, unless "Printer setting"
[SPEED <n>]                                           Print Speed, unless "Printer setting"
SETC AUTODOTTED OFF | ON
SETC PAUSEKEY ON
SETC WATERMARK ON
CLS
BITMAP 0,0,<width_bytes>,<height>,1,<bitmap>
PRINT 1,1
```

- `SIZE` is the raster's size in whole millimetres at 8 dots per mm, rounded up:
  `ceil(cupsWidth / 8)` by `ceil(cupsHeight / 8)`, swapped for a 90° or 270°
  rotation. So 100 × 150 mm (798 × 1198 dots) is `SIZE 100 mm,150 mm`, and 4 × 6 in
  (812 × 1218) is `SIZE 102 mm,153 mm`.
- `REFERENCE` is Horizontal Offset × 64 and Vertical Offset × 64, in dots. Polono's
  PPD calls each step a millimetre, but 64 dots is 8 mm. This driver sends the
  same, and its menus give the real distance.
- Copies are made by macOS (the PPD has `cupsManualCopies: True`), so each copy
  arrives as its own raster page and gets its own setup and `PRINT 1,1`.

### The bitmap

- Each row is padded to a multiple of 4 bytes: `width_bytes = ceil(ceil(w / 8) / 4) × 4`.
  A 0 bit prints. The pad bits are 1 (blank).
- The raster is 8-bit grey with 0 = black and 255 = white (`cupsColorSpace 0`).
- **Brightness** b (−255 to 255) comes first. Each grey level v becomes
  `v + (v − 28) × b / 100`, rounded half up and clamped to 0 to 255. It's a
  stretch around grey level 28, not a shift: +100 doubles every level's distance
  from 28. Pure black (0) and pure white (255) are never changed. This was
  checked against Polono's filter for all 511 values.
- **Image Filter** then decides the dots:
  - *Black and white* (the default): a dot prints when its level is at or below
    **Threshold** (175 by default).
  - *Error diffusion* (Floyd–Steinberg, Burkes, Stucki, Jarvis–Judice–Ninke,
    Sierra, Stevenson–Arce): the textbook weights, left to right and top to
    bottom. A dot prints when the running value is 128 or less. The difference
    from 0 or 255 is spread to the neighbours as `err × weight / divisor`,
    truncated toward zero, and each neighbour is clamped to 0 to 255.
  - *Ordered* (Ordered 4 × 4, Bayer 4 × 4, 8 × 8, 16 × 16): a dot prints when its
    level is below the matrix entry for its position. The matrices are in
    `src/rastertotspl.c`. They were read off Polono's output.
  - Threshold has no effect while dithering.
- **Rotation** happens last. Polono numbers it 1 = 180°, 2 = 90° anticlockwise,
  3 = 90° clockwise. The 180° turn is exact. The 90° and 270° turns are off by one
  in Polono's driver: they read one dot past the edge, which is clamped, so the
  first output row (90°) or column (270°) repeats its neighbour, and one row or
  column of the original is lost. This driver does the same.

### Options left out

Contrast and the four Crop Edge options change nothing in Polono's output.
Print Method ("Direct thermal") changes nothing either: `SET RIBBON OFF` is
always sent. All three are left out.

## PL420

### The job

Each label is sent as a complete job:

```
000…0 (78 ASCII "0") and a space, CR LF
SIZE <w> mm,<h> mm
REFERENCE <x>,<y>
DIRECTION <n>,0
SET GAP ON + GAP 3 mm,0 mm | SET GAP OFF              Media Type: gaps / continuous
OFFSET <n> mm
[DENSITY <n>]
[SPEED <n>]                                           8 by default
SETC AUTODOTTED OFF | ON
SETC PAUSEKEY ON
SETC WATERMARK OFF
CLS
BITMAP 0,0,<width_bytes>,<height>,1,<bitmap> LF       (a bare LF, not CR LF)
PRINT 1,1
```

- `SIZE` follows the PL70e-BT rule: `ceil(cupsWidth / 8)` by `ceil(cupsHeight / 8)` mm.
- `REFERENCE` is Horizontal and Vertical Offset × 8 dots (1 mm per step).
- `DIRECTION` is the Rotate option as it is. The printer does the turning, and
  the bitmap is never rotated. Polono's default is `DIRECTION 1,0`.
- The bitmap is `ceil(w / 8)` bytes per row, with no further padding. A dot
  prints when its grey level is **200 or less** (0 = black). There's no
  dithering. Checked with a 0 to 255 ramp and random images.

### Polono's PPD quirks

- Its default label size, `w283h432`, isn't one of its own sizes. macOS falls
  back to US Letter, so a printer left on the default gets a 216 × 280 mm job.
  This driver's default is 100 × 150 mm. A switched printer that was on
  Polono's default moves to 100 × 150 mm.
- "Black Line" media sends the same gap sensing as "Gap", so it's left out.
- Print Method changes nothing, and is left out.
- Polono's filter refuses to run unless the CUPS queue's device URI names a
  Beeprt or "Thermal Label" printer. That's why checking it needed a
  temporary, disabled print queue.

## Not supported: PL80E and PM220S

Polono's PL80E and PM220S drivers (another vendor, "TSPL2") were captured and
studied, but aren't supported. They don't just convert the image: they do
things to it that look like bugs. On test images:

- The PL80E drops about one blank row in five, which shrinks the white space
  between lines. It can declare a bitmap height that doesn't match the data it
  sends, and it aborted on some small images. Its black-and-white cut-off moves
  with the content of the page.
- The PM220S crops the label to its content, but never places it lower than
  row 80, so content further down the label moves up.

On the bordered test label used for the captures, none of this shows. Copying
these behaviours byte for byte isn't a sensible goal, and approximating them
would break the "same bytes as Polono" promise. These models need a decision,
or an owner with a printer.
