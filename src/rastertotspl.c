/*
 * rastertotspl: CUPS raster -> TSPL filter for Polono label printers.
 *
 * An open-source, independent replacement for Polono's Intel-only drivers,
 * written from the printers' observed behaviour (docs/TSPL.md), not from
 * Polono's code. Not affiliated with or endorsed by Polono.
 *
 * Copyright (c) 2026 Brent Rossow. MIT licence, see LICENSE.
 *
 * Usage (as CUPS runs it): rastertotspl job user title copies options [file]
 * Needs the PPD environment variable. The PPD's *PolonoProfile attribute
 * picks the model's job format:
 *
 *   PL60   Polono PL60 (HPRT)
 *   PL70e  Polono PL70e-BT (Beeprt)
 *   PL420  Polono PL420 (Beeprt)
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Value of a PPD option after defaults + job options are marked.
 * Returns NULL when the option is missing or set to "None" (or, in the
 * PL70e's PPD, "Default"), which means "use the printer's current
 * setting": nothing is sent. */
static const char *choice(ppd_file_t *ppd, const char *keyword)
{
  ppd_choice_t *c = ppdFindMarkedChoice(ppd, keyword);

  if (!c || !strcmp(c->choice, "None") || !strcmp(c->choice, "Default"))
    return NULL;
  return c->choice;
}

static int choice_is(ppd_file_t *ppd, const char *keyword, const char *value)
{
  const char *v = choice(ppd, keyword);

  return v && !strcmp(v, value);
}

static int choice_int(ppd_file_t *ppd, const char *keyword, int fallback)
{
  const char *v = choice(ppd, keyword);

  return v ? atoi(v) : fallback;
}

static void *xmalloc(size_t n)
{
  void *p = malloc(n);

  if (!p)
  {
    fputs("ERROR: out of memory\n", stderr);
    exit(1);
  }
  return p;
}

/* ------------------------------------------------------------------ */
/* PL60                                                                */
/* ------------------------------------------------------------------ */

/* Job-wide setup, sent once before the first page. Order matters: it is the
 * order the original filter uses (see the combo-all capture). */
static void pl60_setup(ppd_file_t *ppd, const cups_page_header2_t *h)
{
  const char *v;

  static const char nul[512];
  fwrite(nul, 1, sizeof(nul), stdout);
  fputs("\r\n", stdout);

  printf("SIZE %.1f mm, %.1f mm\r\n",
         h->cupsPageSize[0] * 25.4 / 72.0, h->cupsPageSize[1] * 25.4 / 72.0);

  if (choice_is(ppd, "MediaMethod", "1"))
    fputs("SET RIBBON OFF\r\n", stdout);

  if ((v = choice(ppd, "PaperType")) != NULL)
  {
    if (!strcmp(v, "1"))
      fputs("GAP 2 mm, 0 mm\r\n", stdout);
    else if (!strcmp(v, "2"))
      fputs("BLINE 2 mm, 0 mm\r\n", stdout);
    else if (!strcmp(v, "3"))
      fputs("GAP 0 mm, 0 mm\r\n", stdout);
  }

  if ((v = choice(ppd, "FowardOffset")) != NULL)   /* sic, as in the PPD */
    printf("OFFSET %d mm\r\n", atoi(v));

  /* SHIFT is in dots at 203 dpi, truncated toward zero (5 mm -> 39). */
  if ((v = choice(ppd, "ShiftMove")) != NULL)
    printf("SHIFT %d\r\n", (int)(atoi(v) * 203.0 / 25.4));

  if ((v = choice(ppd, "PrintSpeed")) != NULL)
    printf("SPEED %d\r\n", atoi(v));

  if ((v = choice(ppd, "PrintDarkness")) != NULL)
    printf("DENSITY %d\r\n", atoi(v));

  if (choice_is(ppd, "PostAction", "1"))
    fputs("SET TEAR ON\r\nSET CUTTER OFF\r\nSET PEEL OFF\r\n", stdout);
  else
    fputs("SET TEAR OFF\r\nSET PEEL OFF\r\nSET CUTTER OFF\r\n", stdout);
}

/* Read one page into a dot map, 1 byte per dot, 1 = printed.
 * 8-bit K (255 = black): printed when K >= 128. 1-bit: a set bit prints. */
static unsigned char *pl60_read_page(cups_raster_t *ras, const cups_page_header2_t *h)
{
  unsigned w = h->cupsWidth, ht = h->cupsHeight, bpl = h->cupsBytesPerLine;
  unsigned char *line = xmalloc(bpl);
  unsigned char *dots = calloc((size_t)w * ht, 1);

  if (!dots)
  {
    fputs("ERROR: out of memory\n", stderr);
    exit(1);
  }

  for (unsigned y = 0; y < ht; y++)
  {
    if (cupsRasterReadPixels(ras, line, bpl) != bpl)
    {
      fputs("ERROR: short raster page\n", stderr);
      exit(1);
    }
    unsigned char *row = dots + (size_t)y * w;
    for (unsigned x = 0; x < w; x++)
      row[x] = h->cupsBitsPerColor == 8 ? line[x] >= 128
                                        : (line[x / 8] >> (7 - x % 8)) & 1;
  }
  free(line);
  return dots;
}

/* Rotate (Orientation 1 = 90 deg clockwise, 2 = 180, 3 = 90 anticlockwise),
 * then mirror left-right, then invert, then pack MSB-first with a 0 bit for
 * a printed dot. Pad bits at the end of each row stay 1 (blank). */
static void pl60_write_page(const unsigned char *dots, unsigned w, unsigned h,
                            int orientation, int mirror, int negative)
{
  unsigned ow = (orientation == 1 || orientation == 3) ? h : w;
  unsigned oh = (orientation == 1 || orientation == 3) ? w : h;
  unsigned wb = (ow + 7) / 8;
  unsigned char *out = xmalloc((size_t)wb * oh);

  memset(out, 0xFF, (size_t)wb * oh);

  for (unsigned r = 0; r < oh; r++)
    for (unsigned c = 0; c < ow; c++)
    {
      unsigned cc = mirror ? ow - 1 - c : c;  /* column before mirroring */
      unsigned sx, sy;                        /* source dot */

      switch (orientation)
      {
        case 1:  sy = h - 1 - cc; sx = r;          break;
        case 2:  sy = h - 1 - r;  sx = w - 1 - cc; break;
        case 3:  sy = cc;         sx = w - 1 - r;  break;
        default: sy = r;          sx = cc;         break;
      }
      if (dots[(size_t)sy * w + sx] != negative)
        out[(size_t)r * wb + c / 8] &= ~(0x80 >> (c % 8));
    }

  printf("CLS\r\nBITMAP 0,0,%u,%u,1,", wb, oh);
  fwrite(out, 1, (size_t)wb * oh, stdout);
  fputs("\r\nPRINT 1,1\r\n", stdout);
  free(out);
}

static int pl60_job(ppd_file_t *ppd, cups_raster_t *ras)
{
  const char *o = choice(ppd, "Orientation");
  int orientation = o ? atoi(o) : 0;
  int mirror = choice_is(ppd, "MirrorImage", "True");
  int negative = choice_is(ppd, "NegativeImage", "True");
  cups_page_header2_t header;
  int page = 0;

  while (cupsRasterReadHeader2(ras, &header))
  {
    if (header.cupsBitsPerColor != 1 && header.cupsBitsPerColor != 8)
    {
      fprintf(stderr, "ERROR: unsupported raster depth %u\n", header.cupsBitsPerColor);
      return 0;
    }
    if (page++ == 0)
      pl60_setup(ppd, &header);
    fprintf(stderr, "INFO: printing page %d\n", page);

    unsigned char *dots = pl60_read_page(ras, &header);
    unsigned copies = header.NumCopies ? header.NumCopies : 1;
    for (unsigned i = 0; i < copies; i++)
      pl60_write_page(dots, header.cupsWidth, header.cupsHeight, orientation, mirror, negative);
    free(dots);
  }
  return page;
}

/* ------------------------------------------------------------------ */
/* Beeprt models (PL70e-BT, PL420)                                      */
/* ------------------------------------------------------------------ */

/* Read one 8-bit page as grey levels, 0 = black and 255 = white (the
 * PPDs set cupsColorSpace 0), mapped through lut. */
static unsigned char *read_grey(cups_raster_t *ras, const cups_page_header2_t *h,
                                const unsigned char lut[256])
{
  unsigned w = h->cupsWidth, ht = h->cupsHeight, bpl = h->cupsBytesPerLine;
  unsigned char *grey = xmalloc((size_t)w * ht);
  unsigned char *line = xmalloc(bpl);

  for (unsigned y = 0; y < ht; y++)
  {
    if (cupsRasterReadPixels(ras, line, bpl) != bpl)
    {
      fputs("ERROR: short raster page\n", stderr);
      exit(1);
    }
    for (unsigned x = 0; x < w; x++)
      grey[(size_t)y * w + x] = lut[h->cupsColorSpace == CUPS_CSPACE_K ? 255 - line[x] : line[x]];
  }
  free(line);
  return grey;
}

/* ------------------------------------------------------------------ */
/* PL70e-BT                                                            */
/* ------------------------------------------------------------------ */

/* Setup, sent before every page (not once per job). SIZE is the raster's
 * size in whole millimetres at 8 dots/mm, rounded up, swapped when the
 * label is turned 90 degrees. */
static void pl70e_setup(ppd_file_t *ppd, unsigned ow, unsigned oh)
{
  const char *v;

  printf("SIZE %u mm,%u mm\r\n", (ow + 7) / 8, (oh + 7) / 8);
  fputs("SET RIBBON OFF\r\n", stdout);

  /* "AdjustHoriaontal" (sic) and AdjustVertical move the origin 64 dots
   * per step. */
  printf("REFERENCE %d,%d\r\n", choice_int(ppd, "AdjustHoriaontal", 0) * 64,
         choice_int(ppd, "AdjustVertical", 0) * 64);

  if (choice_is(ppd, "zeMediaTracking", "BLine"))
    fputs("BLINE 0 mm,0 mm\r\n", stdout);
  else if (choice_is(ppd, "zeMediaTracking", "Continuous"))
    fputs("GAP 0 mm,0 mm\r\n", stdout);
  else
    fputs("GAP 3 mm,0 mm\r\n", stdout);

  printf("OFFSET %d mm\r\n", choice_int(ppd, "FeedOffset", 0));

  if ((v = choice(ppd, "Darkness")) != NULL)
    printf("DENSITY %d\r\n", atoi(v));
  if ((v = choice(ppd, "zePrintRate")) != NULL)
    printf("SPEED %d\r\n", atoi(v));

  printf("SETC AUTODOTTED %s\r\n", choice_is(ppd, "AutoDotted", "1") ? "ON" : "OFF");
  fputs("SETC PAUSEKEY ON\r\nSETC WATERMARK ON\r\n", stdout);
}

/* Grey level after Brightness: v + (v - 28) * b / 100, rounded half up and
 * clamped to 0..255. Pure black (0) and pure white (255) are left alone. */
static unsigned char pl70e_brightness(unsigned char v, int b)
{
  if (v == 0 || v == 255 || b == 0)
    return v;

  int n = 100 * v + (v - 28) * b + 50;
  int q = n >= 0 ? n / 100 : -((-n + 99) / 100);   /* floor division */
  return q < 0 ? 0 : q > 255 ? 255 : (unsigned char)q;
}

/* ImageFilter 2, 8, 9 and 10: ordered dithering. A dot prints when its
 * grey level is below the matrix entry for its position on the page. */
static const unsigned short pl70e_ordered4[4][4] = {
  { 16, 144,  48, 176},
  {208,  80, 240, 112},
  { 64, 192,  32, 160},
  {255, 128, 224,  96},
};
static const unsigned short pl70e_bayer4[4][4] = {
  {  1, 193,  49, 241},
  {129,  65, 177, 113},
  { 33, 225,  17, 209},
  {161,  97, 145,  81},
};
static const unsigned short pl70e_bayer8[8][8] = {
  {  4, 192,  48, 240,  12, 204,  60, 252},
  {128,  64, 176, 112, 140,  76, 188, 124},
  { 32, 224,  16, 208,  44, 236,  28, 220},
  {160,  96, 144,  80, 172, 108, 156,  92},
  {  8, 200,  56, 248,   4, 196,  52, 244},
  {136,  72, 184, 120, 132,  68, 180, 116},
  { 40, 232,  24, 216,  36, 228,  20, 212},
  {168, 104, 152,  88, 164, 100, 148,  84},
};
static const unsigned short pl70e_bayer16[16][16] = {
  {  2, 130,  34, 162,  10, 138,  42, 170,   4, 132,  36, 164,  12, 140,  44, 172},
  {236,  66, 194,  98, 226,  74, 202, 106, 234,  68, 196, 100, 228,  76, 204, 108},
  { 60, 188,  18, 146,  50, 178,  26, 154,  58, 186,  20, 148,  52, 180,  28, 156},
  {220, 124, 252,  82, 210, 114, 242,  90, 218, 122, 250,  84, 212, 116, 244,  92},
  { 16, 144,  48, 176,   6, 134,  38, 166,  14, 142,  46, 174,   8, 136,  40, 168},
  {232,  80, 208, 112, 240,  70, 198, 102, 230,  78, 206, 110, 238,  72, 200, 104},
  { 56, 184,  32, 160,  64, 192,  22, 150,  54, 182,  30, 158,  62, 190,  24, 152},
  {216, 120, 248,  96, 224, 128, 256,  86, 214, 118, 246,  94, 222, 126, 254,  88},
  {  3, 131,  35, 163,  11, 139,  43, 171,   1, 129,  33, 161,   9, 137,  41, 169},
  {233,  67, 195,  99, 227,  75, 203, 107, 235,  65, 193,  97, 225,  73, 201, 105},
  { 57, 185,  19, 147,  51, 179,  27, 155,  59, 187,  17, 145,  49, 177,  25, 153},
  {217, 121, 249,  83, 211, 115, 243,  91, 219, 123, 251,  81, 209, 113, 241,  89},
  { 13, 141,  45, 173,   7, 135,  39, 167,  15, 143,  47, 175,   5, 133,  37, 165},
  {229,  77, 205, 109, 237,  71, 199, 103, 231,  79, 207, 111, 239,  69, 197, 101},
  { 53, 181,  29, 157,  61, 189,  23, 151,  55, 183,  31, 159,  63, 191,  21, 149},
  {213, 117, 245,  93, 221, 125, 253,  87, 215, 119, 247,  95, 223, 127, 255,  85},
};

/* ImageFilter 1 and 3-7: error diffusion, left to right, top to bottom.
 * A dot prints when the running value is 128 or less (then 0, else 255);
 * the difference is spread to the neighbours as err * weight / divisor,
 * truncated toward zero, and each neighbour is clamped to 0..255. */
struct kernel { int div, n; signed char tap[12][3]; };   /* row, col, weight */
static const struct kernel pl70e_kernels[8] = {
  [1] = {16, 4, {{0,1,7},{1,-1,3},{1,0,5},{1,1,1}}},                         /* Floyd-Steinberg */
  [3] = {32, 7, {{0,1,8},{0,2,4},{1,-2,2},{1,-1,4},{1,0,8},{1,1,4},{1,2,2}}}, /* Burkes */
  [4] = {42, 12, {{0,1,8},{0,2,4},{1,-2,2},{1,-1,4},{1,0,8},{1,1,4},{1,2,2},
                  {2,-2,1},{2,-1,2},{2,0,4},{2,1,2},{2,2,1}}},                 /* Stucki */
  [5] = {48, 12, {{0,1,7},{0,2,5},{1,-2,3},{1,-1,5},{1,0,7},{1,1,5},{1,2,3},
                  {2,-2,1},{2,-1,3},{2,0,5},{2,1,3},{2,2,1}}},                 /* Jarvis-Judice-Ninke */
  [6] = {32, 10, {{0,1,5},{0,2,3},{1,-2,2},{1,-1,4},{1,0,5},{1,1,4},{1,2,2},
                  {2,-1,2},{2,0,3},{2,1,2}}},                                  /* Sierra */
  [7] = {200, 12, {{0,2,32},{1,-3,12},{1,-1,26},{1,1,30},{1,3,16},{2,-2,12},
                   {2,0,26},{2,2,12},{3,-3,5},{3,-1,12},{3,1,12},{3,3,5}}},    /* Stevenson-Arce */
};

/* Turn grey levels (0 = black, 255 = white, after Brightness) into dots,
 * 1 = printed, in place. */
static void pl70e_dots(unsigned char *g, unsigned w, unsigned h, int filter, int threshold)
{
  size_t n = (size_t)w * h;

  if (filter >= 1 && filter <= 7 && filter != 2)
  {
    const struct kernel *k = &pl70e_kernels[filter];
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
      {
        unsigned char *p = g + (size_t)y * w + x;
        int old = *p, black = old <= 128, err = old - (black ? 0 : 255);
        *p = (unsigned char)black;
        for (int t = 0; t < k->n; t++)
        {
          long yy = (long)y + k->tap[t][0], xx = (long)x + k->tap[t][1];
          if (yy >= (long)h || xx < 0 || xx >= (long)w)
            continue;
          unsigned char *q = g + (size_t)yy * w + xx;
          int v = *q + err * k->tap[t][2] / k->div;
          *q = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
      }
    return;
  }

  if (filter == 2 || (filter >= 8 && filter <= 10))
  {
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++)
      {
        unsigned char *p = g + (size_t)y * w + x;
        unsigned m = filter == 2  ? pl70e_ordered4[y % 4][x % 4]
                   : filter == 8  ? pl70e_bayer4[y % 4][x % 4]
                   : filter == 9  ? pl70e_bayer8[y % 8][x % 8]
                                  : pl70e_bayer16[y % 16][x % 16];
        *p = *p < m;
      }
    return;
  }

  for (size_t i = 0; i < n; i++)                   /* 0: plain threshold */
    g[i] = g[i] <= threshold;
}

static int pl70e_job(ppd_file_t *ppd, cups_raster_t *ras)
{
  int rotate = choice_int(ppd, "Rotate", 0);   /* 1 = 180, 2 = 90, 3 = 270 */
  int threshold = choice_int(ppd, "Threshold", 175);
  int brightness = choice_int(ppd, "Brightness", 0);
  int filter = choice_int(ppd, "ImageFilter", 0);
  cups_page_header2_t h;
  int page = 0;
  unsigned char lut[256];

  for (int v = 0; v < 256; v++)
    lut[v] = pl70e_brightness((unsigned char)v, brightness);

  while (cupsRasterReadHeader2(ras, &h))
  {
    if (h.cupsBitsPerColor != 8 || h.cupsBitsPerPixel != 8)
    {
      fprintf(stderr, "ERROR: unsupported raster depth %u\n", h.cupsBitsPerColor);
      return 0;
    }
    fprintf(stderr, "INFO: printing page %d\n", ++page);

    /* Brightness first, then ImageFilter; with ImageFilter 0 (the default)
     * a dot prints when its value is <= Threshold. */
    unsigned w = h.cupsWidth, ht = h.cupsHeight;
    unsigned char *dots = read_grey(ras, &h, lut);
    pl70e_dots(dots, w, ht, filter, threshold);

    /* Rotation, as the original does it. The 90 and 270 degree turns are
     * off by one: they read one dot beyond the edge, which is clamped, so
     * the first output row (90) or column (270) repeats its neighbour. */
    int quarter = rotate == 2 || rotate == 3;
    unsigned ow = quarter ? ht : w, oh = quarter ? w : ht;
    unsigned wb = ((ow + 7) / 8 + 3) / 4 * 4;    /* rows padded to 4 bytes */
    unsigned char *out = xmalloc((size_t)wb * oh);
    memset(out, 0xFF, (size_t)wb * oh);
    for (unsigned r = 0; r < oh; r++)
      for (unsigned c = 0; c < ow; c++)
      {
        unsigned sx, sy;
        switch (rotate)
        {
          case 1:  sy = ht - 1 - r; sx = w - 1 - c; break;
          case 2:  sy = c; sx = r ? w - r : w - 1; break;
          case 3:  sy = c ? ht - c : ht - 1; sx = r; break;
          default: sy = r; sx = c; break;
        }
        if (dots[(size_t)sy * w + sx])
          out[(size_t)r * wb + c / 8] &= ~(0x80 >> (c % 8));
      }
    free(dots);

    unsigned copies = h.NumCopies ? h.NumCopies : 1;
    for (unsigned i = 0; i < copies; i++)
    {
      pl70e_setup(ppd, ow, oh);
      printf("CLS\r\nBITMAP 0,0,%u,%u,1,", wb, oh);
      fwrite(out, 1, (size_t)wb * oh, stdout);
      fputs("\r\nPRINT 1,1\r\n", stdout);
    }
    free(out);
  }
  return page;
}

/* ------------------------------------------------------------------ */
/* PL420                                                               */
/* ------------------------------------------------------------------ */

/* Each page is a whole job: a run of 78 ASCII '0' and a space, then the
 * setup, then the bitmap. The printer does the rotation (DIRECTION), so
 * the bitmap is the raster as it is. A dot prints when its grey level is
 * 200 or less. Rows are ceil(width / 8) bytes, and the bitmap ends with a
 * bare LF rather than CR LF. */
static int pl420_job(ppd_file_t *ppd, cups_raster_t *ras)
{
  cups_page_header2_t h;
  int page = 0;
  unsigned char lut[256];
  const char *v;

  for (int i = 0; i < 256; i++)
    lut[i] = i <= 200;

  while (cupsRasterReadHeader2(ras, &h))
  {
    if (h.cupsBitsPerColor != 8 || h.cupsBitsPerPixel != 8)
    {
      fprintf(stderr, "ERROR: unsupported raster depth %u\n", h.cupsBitsPerColor);
      return 0;
    }
    fprintf(stderr, "INFO: printing page %d\n", ++page);

    unsigned w = h.cupsWidth, ht = h.cupsHeight, wb = (w + 7) / 8;
    unsigned char *dots = read_grey(ras, &h, lut);
    unsigned char *out = xmalloc((size_t)wb * ht);
    memset(out, 0xFF, (size_t)wb * ht);
    for (unsigned r = 0; r < ht; r++)
      for (unsigned c = 0; c < w; c++)
        if (dots[(size_t)r * w + c])
          out[(size_t)r * wb + c / 8] &= ~(0x80 >> (c % 8));
    free(dots);

    unsigned copies = h.NumCopies ? h.NumCopies : 1;
    for (unsigned i = 0; i < copies; i++)
    {
      for (int z = 0; z < 78; z++)
        putchar('0');
      fputs(" \r\n", stdout);
      printf("SIZE %u mm,%u mm\r\n", (w + 7) / 8, (ht + 7) / 8);
      /* "AdjustHoriaontal" (sic): 8 dots (1 mm) per step. */
      printf("REFERENCE %d,%d\r\n", choice_int(ppd, "AdjustHoriaontal", 0) * 8,
             choice_int(ppd, "AdjustVertical", 0) * 8);
      printf("DIRECTION %d,0\r\n", choice_int(ppd, "Rotate", 1));
      if (choice_is(ppd, "zeMediaTracking", "Continuous"))
        fputs("SET GAP OFF\r\n", stdout);
      else
        fputs("SET GAP ON\r\nGAP 3 mm,0 mm\r\n", stdout);
      printf("OFFSET %d mm\r\n", choice_int(ppd, "FeedOffset", 0));
      if ((v = choice(ppd, "Darkness")) != NULL)
        printf("DENSITY %d\r\n", atoi(v));
      if ((v = choice(ppd, "zePrintRate")) != NULL)
        printf("SPEED %d\r\n", atoi(v));
      printf("SETC AUTODOTTED %s\r\n", choice_is(ppd, "AutoDotted", "1") ? "ON" : "OFF");
      fputs("SETC PAUSEKEY ON\r\nSETC WATERMARK OFF\r\n", stdout);
      printf("CLS\r\nBITMAP 0,0,%u,%u,1,", wb, ht);
      fwrite(out, 1, (size_t)wb * ht, stdout);
      fputs("\nPRINT 1,1\r\n", stdout);
    }
    free(out);
  }
  return page;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
  if (argc < 6 || argc > 7)
  {
    fputs("ERROR: rastertotspl job-id user title copies options [file]\n", stderr);
    return 1;
  }

  const char *ppd_path = getenv("PPD");
  ppd_file_t *ppd = ppd_path ? ppdOpenFile(ppd_path) : NULL;
  if (!ppd)
  {
    fputs("ERROR: PPD environment variable not set or PPD unreadable\n", stderr);
    return 1;
  }

  ppd_attr_t *attr = ppdFindAttr(ppd, "PolonoProfile", NULL);
  const char *profile = attr && attr->value ? attr->value : "";
  int (*job)(ppd_file_t *, cups_raster_t *);
  if (!strcmp(profile, "PL60"))
    job = pl60_job;
  else if (!strcmp(profile, "PL70e"))
    job = pl70e_job;
  else if (!strcmp(profile, "PL420"))
    job = pl420_job;
  else
  {
    fprintf(stderr, "ERROR: the PPD has no known *PolonoProfile (\"%s\")\n", profile);
    return 1;
  }

  /* Copies (argv[4]) is ignored on purpose, as the originals do. Copies
   * come from the raster: each page header's NumCopies (one-page jobs), or
   * pages already repeated by cgpdftoraster (collated multi-page jobs). */
  cups_option_t *options = NULL;
  int num_options = cupsParseOptions(argv[5], 0, &options);
  ppdMarkDefaults(ppd);
  cupsMarkOptions(ppd, num_options, options);

  int fd = 0;
  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0)
  {
    perror("ERROR: unable to open raster file");
    return 1;
  }

  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  int pages = job(ppd, ras);

  cupsRasterClose(ras);
  if (fd)
    close(fd);
  cupsFreeOptions(num_options, options);
  ppdClose(ppd);
  fflush(stdout);
  return pages ? 0 : 1;
}
