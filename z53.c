/*@z53.c:SVG Back End:SVG_BackEnd@********************************************/
/*                                                                           */
/*  THE LOUT DOCUMENT FORMATTING SYSTEM (VERSION 3.43)                       */
/*  COPYRIGHT (C) 2026 James Clements III                                    */
/*                                                                           */
/*  This program is free software; you can redistribute it and/or modify     */
/*  it under the terms of the GNU General Public License as published by     */
/*  the Free Software Foundation; either Version 3, or (at your option)      */
/*  any later version.                                                       */
/*                                                                           */
/*  This program is distributed in the hope that it will be useful,          */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of           */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            */
/*  GNU General Public License for more details.                             */
/*                                                                           */
/*  FILE:         z53.c                                                      */
/*  MODULE:       SVG Back End                                               */
/*  EXTERNS:      SVG_BackEnd, SVG_NullBackEnd                               */
/*                                                                           */
/*  STATUS:       Implements three coordinated visual-parity fixes vs.       */
/*                the PostScript back end: (1) a small PS-to-SVG drawing-op  */
/*                translator inside SVG_PrintGraphicObject for the common    */
/*                primitives used by @Box/@Rule/page borders/simple tables;  */
/*                (2) a single page-level Y-flip group so that translated/   */
/*                rotated coordinates compose correctly with text;           */
/*                (3) PostScript glyph-name -> Unicode mapping in            */
/*                SVG_PrintWord, via the font's LCM vector.                  */
/*                                                                           */
/*  ANSI C ONLY: this module must compile with tcc.  No mid-block decls,     */
/*  no // comments, no designated initialisers.                              */
/*                                                                           */
/*****************************************************************************/
#include "externs.h"
#include <math.h>


/*****************************************************************************/
/*                                                                           */
/*  State variables for this module                                          */
/*                                                                           */
/*****************************************************************************/

#define SVG_MAX_GS            64
#define SVG_GRAPHIC_BUF_SIZE  262144
#define SVG_PS_STACK_DEPTH    512
#define SVG_PS_GS_DEPTH       32
#define SVG_PS_DICT_STACK_DEPTH 32
#define SVG_PS_DICT_POOL      1024
#define SVG_PS_DICT_ENTRIES   512   /* must be a power of 2: open-addressing */
#define SVG_PS_DICT_MASK      (SVG_PS_DICT_ENTRIES - 1)
#define SVG_PS_MAX_RECURSION  256
#define SVG_PATH_BUF_SIZE     16384
#define SVG_DASH_BUF_SIZE     128

#ifndef SVG_PI
#define SVG_PI 3.14159265358979323846
#endif

static FILE        *out_fp;        /* output file (HTML/SVG sink)            */
static BOOLEAN     encapsulated;   /* reserved for future use                */
static int         pagecount;      /* total number of pages emitted          */
static BOOLEAN     page_open;      /* TRUE while a <svg> page is open        */
static FULL_LENGTH page_h;         /* current page horizontal extent (Lout)  */
static FULL_LENGTH page_v;         /* current page vertical extent (Lout)    */
static int         gs_groups[SVG_MAX_GS]; /* <g> tags opened per Save level  */
static int         gs_top;         /* top of the gs_groups stack (-1 empty)  */

/* Module-level CTM mirroring the cumulative effect of the SVG <g transform>  */
/* chain that wraps the @Graphic body about to be processed.  Updated by    */
/* SVG_CoordTranslate / SVG_CoordRotate / SVG_CoordScale (the same calls    */
/* that emit <g transform="...">) and saved/restored by                   */
/* SVG_SaveGraphicState / SVG_RestoreGraphicState.  Each fresh interpreter  */
/* invocation seeds its CTM from this matrix so PS-side transform/         */
/* itransform round-trips see the actual page-level coordinate frame.      */
static double      svg_outer_ctm[6];
static double      svg_outer_ctm_stack[SVG_MAX_GS][6];

/* The most-recent values for xsize/ysize/xmark/ymark/loutf/loutv/louts as   */
/* would have been bound by `LoutGraphic` in PS mode.  Captured by           */
/* SVG_DefineGraphicNames before each @Graphic body is interpreted.          */
static FULL_LENGTH cur_gr_xsize, cur_gr_ysize;
static FULL_LENGTH cur_gr_xmark, cur_gr_ymark;
static FULL_LENGTH cur_gr_loutf, cur_gr_loutv, cur_gr_louts;
static BOOLEAN     cur_gr_set;


/*****************************************************************************/
/*                                                                           */
/*  static void SVG_PrintInitialize(FILE *fp, BOOLEAN enc)                   */
/*                                                                           */
/*  Initialise back-end state and emit the XML preamble and root <svg>.      */
/*                                                                           */
/*****************************************************************************/

/* Forward declarations for the PS interpreter init/shutdown -- defined     */
/* later in this file but called from PrintInitialize/PrintAfterLastPage.   */
static void svg_psinterp_init(void);
static void svg_psinterp_shutdown(void);
static void svg_emit_pattern_defs(void);

/* Forward decl: FNV-1a hash used by the dict lookup, op-id dispatch, and    */
/* (since perf round 3) the glyph-name->unicode lookup.  Defined alongside  */
/* the dict helpers further down.                                            */
static unsigned int svg_name_hash(const char *s);

/* Forward decl: the per-document font-face flag cache (defined alongside    */
/* SVG_PrintWord); SVG_PrintInitialize resets the cache between back-to-     */
/* back document builds inside the same process.                             */
static void svg_face_cache_clear(void);

/* Glyph-outline service implemented in z53_glyph.c.  Returns 1 if the      */
/* font + glyph are known (and the callbacks have been called to lay down   */
/* the outline), 0 to fall back to the caller's bbox approximation.  The    */
/* PS back end (z49.c) never calls this; it lives only for charpath in this */
/* module.                                                                   */
extern int svg_glyph_emit_outline(
  const char *ps_font_name,
  const char *glyph_name,
  double font_size_units,
  double x0, double y0,
  double *advance_out,
  void *user,
  void (*cb_move)(void *, double, double),
  void (*cb_line)(void *, double, double),
  void (*cb_curve)(void *, double, double, double, double, double, double),
  void (*cb_close)(void *));

/* Static 128 KB buffer for out_fp.  Kept here (not on the stack inside     */
/* SVG_PrintInitialize) because setvbuf documents that the supplied buffer */
/* must outlive the stream.  glibc's default for regular files is 4 KB;    */
/* the user-guide SVG is ~16 MB, so a larger buffer cuts the write-syscall */
/* count by roughly 32x and helps wall-clock more than user-CPU.           */
#define SVG_OUTBUF_SIZE (128 * 1024)
static char svg_outbuf[SVG_OUTBUF_SIZE];

static void SVG_PrintInitialize(FILE *fp, BOOLEAN enc)
{
  out_fp = fp;
  encapsulated = enc;
  pagecount = 0;
  page_open = FALSE;
  page_h = 0;
  page_v = 0;
  gs_top = -1;
  cur_gr_set = FALSE;
  cur_gr_xsize = cur_gr_ysize = cur_gr_xmark = cur_gr_ymark = 0;
  cur_gr_loutf = 12 * PT;
  cur_gr_loutv = 12 * PT;
  cur_gr_louts = 4 * PT;
  /* Identity outer CTM */
  svg_outer_ctm[0] = 1.0; svg_outer_ctm[1] = 0.0;
  svg_outer_ctm[2] = 0.0; svg_outer_ctm[3] = 1.0;
  svg_outer_ctm[4] = 0.0; svg_outer_ctm[5] = 0.0;
  /* Persistent PS-interpreter dictionary stack lives across the entire     */
  /* document, mirroring PostScript's own state.                            */
  svg_psinterp_init();
  svg_face_cache_clear();
  if( out_fp != NULL )
  {
    /* Larger fully-buffered I/O.  Safe to call before any writes; the      */
    /* return value is ignored because failure (e.g. fp is a pipe with no   */
    /* buffering support) is harmless -- glibc retains the default buffer. */
    (void) setvbuf(out_fp, svg_outbuf, _IOFBF, SVG_OUTBUF_SIZE);
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", out_fp);
    fputs("<!-- generated by Lout SVG back end (z53.c) -->\n", out_fp);
  }
} /* end SVG_PrintInitialize */


/*****************************************************************************/
/*                                                                           */
/*  static void svg_open_page(FULL_LENGTH h, FULL_LENGTH v, FULL_CHAR *label) */
/*                                                                           */
/*  Open one <svg> element for a page of h x v Lout units, plus a top-level  */
/*  <g> that flips Y so that drawing operations can use literal Lout (bottom-*/
/*  left) coordinates throughout the page.  Text wraps each <text> in its    */
/*  own counter-flip to keep glyphs upright.                                 */
/*                                                                           */
/*****************************************************************************/

static void svg_open_page(FULL_LENGTH h, FULL_LENGTH v, FULL_CHAR *label)
{
  int w_pt, h_pt;
  if( out_fp == NULL )
    return;
  w_pt = h / PT;
  h_pt = v / PT;
  page_h = h;
  page_v = v;
  fprintf(out_fp,
    "<svg xmlns=\"http://www.w3.org/2000/svg\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "version=\"1.1\" "
    "class=\"lout-page\" "
    "data-page=\"%d\" "
    "data-label=\"%s\" "
    "width=\"%dpt\" height=\"%dpt\" "
    "viewBox=\"0 0 %d %d\">\n",
    pagecount, label == NULL ? "" : (const char *) label,
    w_pt, h_pt, w_pt, h_pt);
  /* Page-level <defs> with hard-coded SVG <pattern> definitions for every  */
  /* named Lout texture; emitted up here (before the Y-flip group) so the   */
  /* patternUnits="userSpaceOnUse" tile sizes are in non-flipped pt units.  */
  svg_emit_pattern_defs();
  /* Single page-level Y-flip so paths/rules/transforms can use literal     */
  /* Lout (bottom-left) coordinates inside the page.                        */
  fprintf(out_fp, "<g transform=\"matrix(1 0 0 -1 0 %d)\">\n", h_pt);
  page_open = TRUE;
}


/*****************************************************************************/
/*                                                                           */
/*  static void svg_close_page(void)                                         */
/*                                                                           */
/*****************************************************************************/

static void svg_close_page(void)
{
  if( out_fp != NULL && page_open )
  {
    fputs("</g>\n", out_fp);
    fputs("</svg>\n", out_fp);
    page_open = FALSE;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  static const char *svg_colour_rgb(COLOUR_NUM cnum, char *buf)            */
/*                                                                           */
/*  Parse Lout's ColourCommand output for cnum (a PostScript-syntax string   */
/*  such as "0.5 0.5 0.5 setrgbcolor" or "0.5 setgray") and write an SVG-    */
/*  compatible "rgb(R,G,B)" string into buf (must hold at least 24 bytes).   */
/*  Returns buf on success, or NULL if cnum is zero/invalid.                 */
/*                                                                           */
/*****************************************************************************/

static const char *svg_colour_rgb(COLOUR_NUM cnum, char *buf)
{
  FULL_CHAR *cmd;
  double r, g, b;
  int rr, gg, bb;
  if( cnum == 0 )
    return NULL;
  cmd = ColourCommand(cnum);
  if( cmd == NULL )
    return NULL;
  if( sscanf((const char *) cmd, "%lf %lf %lf setrgbcolor", &r, &g, &b) == 3 )
  {
    rr = (int) (r * 255.0 + 0.5);
    gg = (int) (g * 255.0 + 0.5);
    bb = (int) (b * 255.0 + 0.5);
  }
  else if( sscanf((const char *) cmd, "%lf setgray", &r) == 1 )
  {
    rr = gg = bb = (int) (r * 255.0 + 0.5);
  }
  else
    return NULL;
  if( rr < 0 )   rr = 0;
  if( rr > 255 ) rr = 255;
  if( gg < 0 )   gg = 0;
  if( gg > 255 ) gg = 255;
  if( bb < 0 )   bb = 0;
  if( bb > 255 ) bb = 255;
  sprintf(buf, "rgb(%d,%d,%d)", rr, gg, bb);
  return buf;
}


/*****************************************************************************/
/*                                                                           */
/*  Glyph-name -> Unicode codepoint lookup                                   */
/*                                                                           */
/*  Maps a PostScript / Adobe glyph name (as found in an LCM vector entry)   */
/*  to a Unicode codepoint.  Returns 0 if not found.  Covers the glyphs that */
/*  Lout uses for smart quotes, dashes, ligatures, symbols and the common    */
/*  Latin-1 accented characters which appear in the user guide.             */
/*                                                                           */
/*****************************************************************************/

struct svg_glyph_map { const char *name; unsigned int cp; };

static const struct svg_glyph_map svg_glyph_table[] = {
  /* ASCII names */
  { "space",          0x0020 },
  { "exclam",         0x0021 },
  { "quotedbl",       0x0022 },
  { "numbersign",     0x0023 },
  { "dollar",         0x0024 },
  { "percent",        0x0025 },
  { "ampersand",      0x0026 },
  { "quotesingle",    0x0027 },
  { "parenleft",      0x0028 },
  { "parenright",     0x0029 },
  { "asterisk",       0x002A },
  { "plus",           0x002B },
  { "comma",          0x002C },
  { "hyphen",         0x002D },
  { "period",         0x002E },
  { "slash",          0x002F },
  { "zero",           0x0030 },
  { "one",            0x0031 },
  { "two",            0x0032 },
  { "three",          0x0033 },
  { "four",           0x0034 },
  { "five",           0x0035 },
  { "six",            0x0036 },
  { "seven",          0x0037 },
  { "eight",          0x0038 },
  { "nine",           0x0039 },
  { "colon",          0x003A },
  { "semicolon",      0x003B },
  { "less",           0x003C },
  { "equal",          0x003D },
  { "greater",        0x003E },
  { "question",       0x003F },
  { "at",             0x0040 },
  { "bracketleft",    0x005B },
  { "backslash",      0x005C },
  { "bracketright",   0x005D },
  { "asciicircum",    0x005E },
  { "underscore",     0x005F },
  { "grave",          0x0060 },
  { "braceleft",      0x007B },
  { "bar",            0x007C },
  { "braceright",     0x007D },
  { "asciitilde",     0x007E },
  /* Punctuation and symbols (Lout uses these heavily) */
  { "quoteleft",      0x2018 },
  { "quoteright",     0x2019 },
  { "quotedblleft",   0x201C },
  { "quotedblright",  0x201D },
  { "quotesinglbase", 0x201A },
  { "quotedblbase",   0x201E },
  { "guillemotleft",  0x00AB },
  { "guillemotright", 0x00BB },
  { "guilsinglleft",  0x2039 },
  { "guilsinglright", 0x203A },
  { "endash",         0x2013 },
  { "emdash",         0x2014 },
  { "bullet",         0x2022 },
  { "ellipsis",       0x2026 },
  { "dagger",         0x2020 },
  { "daggerdbl",      0x2021 },
  { "section",        0x00A7 },
  { "paragraph",      0x00B6 },
  { "copyright",      0x00A9 },
  { "copyrightsans",  0x00A9 },
  { "copyrightserif", 0x00A9 },
  { "registered",     0x00AE },
  { "registersans",   0x00AE },
  { "registerserif",  0x00AE },
  { "trademark",      0x2122 },
  { "trademarksans",  0x2122 },
  { "trademarkserif", 0x2122 },
  { "perthousand",    0x2030 },
  { "florin",         0x0192 },
  { "fraction",       0x2044 },
  { "Euro",           0x20AC },
  { "fi",             0xFB01 },
  { "fl",             0xFB02 },
  { "OE",             0x0152 },
  { "oe",             0x0153 },
  { "germandbls",     0x00DF },
  { "Scaron",         0x0160 },
  { "scaron",         0x0161 },
  { "Zcaron",         0x017D },
  { "zcaron",         0x017E },
  { "Ydieresis",      0x0178 },
  { "exclamdown",     0x00A1 },
  { "questiondown",   0x00BF },
  { "cent",           0x00A2 },
  { "sterling",       0x00A3 },
  { "currency",       0x00A4 },
  { "yen",            0x00A5 },
  { "brokenbar",      0x00A6 },
  { "dieresis",       0x00A8 },
  { "ordfeminine",    0x00AA },
  { "logicalnot",     0x00AC },
  { "hyphen",         0x002D },
  { "macron",         0x00AF },
  { "degree",         0x00B0 },
  { "plusminus",      0x00B1 },
  { "twosuperior",    0x00B2 },
  { "threesuperior",  0x00B3 },
  { "acute",          0x00B4 },
  { "mu",             0x00B5 },
  { "periodcentered", 0x00B7 },
  { "cedilla",        0x00B8 },
  { "onesuperior",    0x00B9 },
  { "ordmasculine",   0x00BA },
  { "onequarter",     0x00BC },
  { "onehalf",        0x00BD },
  { "threequarters",  0x00BE },
  { "circumflex",     0x02C6 },
  { "tilde",          0x02DC },
  { "caron",          0x02C7 },
  { "breve",          0x02D8 },
  { "dotaccent",      0x02D9 },
  { "ring",           0x02DA },
  { "ogonek",         0x02DB },
  { "hungarumlaut",   0x02DD },
  { "minus",          0x2212 },
  { "multiply",       0x00D7 },
  { "divide",         0x00F7 },
  /* Accented Latin-1 capitals */
  { "Agrave",         0x00C0 },
  { "Aacute",         0x00C1 },
  { "Acircumflex",    0x00C2 },
  { "Atilde",         0x00C3 },
  { "Adieresis",      0x00C4 },
  { "Aring",          0x00C5 },
  { "AE",             0x00C6 },
  { "Ccedilla",       0x00C7 },
  { "Egrave",         0x00C8 },
  { "Eacute",         0x00C9 },
  { "Ecircumflex",    0x00CA },
  { "Edieresis",      0x00CB },
  { "Igrave",         0x00CC },
  { "Iacute",         0x00CD },
  { "Icircumflex",    0x00CE },
  { "Idieresis",      0x00CF },
  { "Eth",            0x00D0 },
  { "Ntilde",         0x00D1 },
  { "Ograve",         0x00D2 },
  { "Oacute",         0x00D3 },
  { "Ocircumflex",    0x00D4 },
  { "Otilde",         0x00D5 },
  { "Odieresis",      0x00D6 },
  { "Oslash",         0x00D8 },
  { "Ugrave",         0x00D9 },
  { "Uacute",         0x00DA },
  { "Ucircumflex",    0x00DB },
  { "Udieresis",      0x00DC },
  { "Yacute",         0x00DD },
  { "Thorn",          0x00DE },
  /* Accented Latin-1 lower-case */
  { "agrave",         0x00E0 },
  { "aacute",         0x00E1 },
  { "acircumflex",    0x00E2 },
  { "atilde",         0x00E3 },
  { "adieresis",      0x00E4 },
  { "aring",          0x00E5 },
  { "ae",             0x00E6 },
  { "ccedilla",       0x00E7 },
  { "egrave",         0x00E8 },
  { "eacute",         0x00E9 },
  { "ecircumflex",    0x00EA },
  { "edieresis",      0x00EB },
  { "igrave",         0x00EC },
  { "iacute",         0x00ED },
  { "icircumflex",    0x00EE },
  { "idieresis",      0x00EF },
  { "eth",            0x00F0 },
  { "ntilde",         0x00F1 },
  { "ograve",         0x00F2 },
  { "oacute",         0x00F3 },
  { "ocircumflex",    0x00F4 },
  { "otilde",         0x00F5 },
  { "odieresis",      0x00F6 },
  { "oslash",         0x00F8 },
  { "ugrave",         0x00F9 },
  { "uacute",         0x00FA },
  { "ucircumflex",    0x00FB },
  { "udieresis",      0x00FC },
  { "yacute",         0x00FD },
  { "thorn",          0x00FE },
  { "ydieresis",      0x00FF },
  /* Misc */
  { "nbspace",        0x00A0 },
  { "softhyphen",     0x00AD },

  /* ================================================================== */
  /* Adobe Symbol font (Symb.LCM): Greek letters, mathematical / set /  */
  /* logic operators, arrows, fences, and miscellaneous symbols.  Code  */
  /* points follow Adobe's published "Symbol" character set (the same  */
  /* mapping used by Adobe's symbol-glyph list and PDF text extraction). */
  /* ================================================================== */

  /* Greek upper case */
  { "Alpha",          0x0391 },
  { "Beta",           0x0392 },
  { "Gamma",          0x0393 },
  { "Delta",          0x0394 },
  { "Epsilon",        0x0395 },
  { "Zeta",           0x0396 },
  { "Eta",            0x0397 },
  { "Theta",          0x0398 },
  { "Iota",           0x0399 },
  { "Kappa",          0x039A },
  { "Lambda",         0x039B },
  { "Mu",             0x039C },
  { "Nu",             0x039D },
  { "Xi",             0x039E },
  { "Omicron",        0x039F },
  { "Pi",             0x03A0 },
  { "Rho",            0x03A1 },
  { "Sigma",          0x03A3 },
  { "Tau",            0x03A4 },
  { "Upsilon",        0x03A5 },
  { "Phi",            0x03A6 },
  { "Chi",            0x03A7 },
  { "Psi",            0x03A8 },
  { "Omega",          0x03A9 },
  /* Greek lower case */
  { "alpha",          0x03B1 },
  { "beta",           0x03B2 },
  { "gamma",          0x03B3 },
  { "delta",          0x03B4 },
  { "epsilon",        0x03B5 },
  { "zeta",           0x03B6 },
  { "eta",            0x03B7 },
  { "theta",          0x03B8 },
  { "iota",           0x03B9 },
  { "kappa",          0x03BA },
  { "lambda",         0x03BB },
  /* "mu" already mapped above as 0x00B5 (micro sign); Adobe Symbol     */
  /* expects U+03BC (Greek small letter mu) at code 0x6D.  Both glyphs  */
  /* look identical; we keep the existing 0x00B5 entry to avoid breaking */
  /* the Latin-1 mu and let Symb mu fall through to the same binding.    */
  { "nu",             0x03BD },
  { "xi",             0x03BE },
  { "omicron",        0x03BF },
  { "pi",             0x03C0 },
  { "rho",            0x03C1 },
  { "sigma",          0x03C3 },
  { "tau",            0x03C4 },
  { "upsilon",        0x03C5 },
  { "phi",            0x03C6 },
  { "chi",            0x03C7 },
  { "psi",            0x03C8 },
  { "omega",          0x03C9 },
  /* Greek variants used by Symbol font */
  { "theta1",         0x03D1 },  /* GREEK THETA SYMBOL */
  { "phi1",           0x03D5 },  /* GREEK PHI SYMBOL   */
  { "sigma1",         0x03C2 },  /* GREEK SMALL LETTER FINAL SIGMA */
  { "omega1",         0x03D6 },  /* GREEK PI SYMBOL (Adobe's "omega1") */
  { "Upsilon1",       0x03D2 },  /* GREEK UPSILON WITH HOOK SYMBOL */

  /* Mathematical operators */
  { "universal",      0x2200 },  /* FOR ALL */
  { "existential",    0x2203 },  /* THERE EXISTS */
  { "suchthat",       0x220B },  /* CONTAINS AS MEMBER */
  { "asteriskmath",   0x2217 },  /* ASTERISK OPERATOR */
  { "congruent",      0x2245 },  /* APPROXIMATELY EQUAL TO */
  { "therefore",      0x2234 },  /* THEREFORE */
  { "perpendicular",  0x22A5 },  /* UP TACK */
  { "radicalex",      0x203E },  /* OVERLINE (radical extension) */
  { "minute",         0x2032 },  /* PRIME */
  { "second",         0x2033 },  /* DOUBLE PRIME */
  { "lessequal",      0x2264 },  /* LESS-THAN OR EQUAL TO */
  { "greaterequal",   0x2265 },  /* GREATER-THAN OR EQUAL TO */
  { "infinity",       0x221E },  /* INFINITY */
  { "notequal",       0x2260 },  /* NOT EQUAL TO */
  { "approxequal",    0x2248 },  /* ALMOST EQUAL TO */
  { "equivalence",    0x2261 },  /* IDENTICAL TO */
  { "proportional",   0x221D },  /* PROPORTIONAL TO */
  { "partialdiff",    0x2202 },  /* PARTIAL DIFFERENTIAL */
  { "similar",        0x223C },  /* TILDE OPERATOR */
  { "aleph",          0x2135 },  /* ALEF SYMBOL */
  { "Ifraktur",       0x2111 },  /* BLACK-LETTER CAPITAL I */
  { "Rfraktur",       0x211C },  /* BLACK-LETTER CAPITAL R */
  { "weierstrass",    0x2118 },  /* SCRIPT CAPITAL P (Weierstrass p) */
  { "emptyset",       0x2205 },  /* EMPTY SET */
  { "gradient",       0x2207 },  /* NABLA */
  { "product",        0x220F },  /* N-ARY PRODUCT */
  { "summation",      0x2211 },  /* N-ARY SUMMATION */
  { "integral",       0x222B },  /* INTEGRAL */
  { "dotmath",        0x22C5 },  /* DOT OPERATOR */
  { "radical",        0x221A },  /* SQUARE ROOT */
  { "lozenge",        0x25CA },  /* LOZENGE */
  { "angle",          0x2220 },  /* ANGLE */
  /* angleleft / angleright are the math-bra fences used in symbol font */
  { "angleleft",      0x27E8 },  /* MATHEMATICAL LEFT ANGLE BRACKET */
  { "angleright",     0x27E9 },  /* MATHEMATICAL RIGHT ANGLE BRACKET */

  /* Set and logic operators */
  { "element",        0x2208 },  /* ELEMENT OF */
  { "notelement",     0x2209 },  /* NOT AN ELEMENT OF */
  { "intersection",   0x2229 },  /* INTERSECTION */
  { "union",          0x222A },  /* UNION */
  { "propersubset",   0x2282 },  /* SUBSET OF */
  { "propersuperset", 0x2283 },  /* SUPERSET OF */
  { "reflexsubset",   0x2286 },  /* SUBSET OF OR EQUAL TO */
  { "reflexsuperset", 0x2287 },  /* SUPERSET OF OR EQUAL TO */
  { "notsubset",      0x2284 },  /* NOT A SUBSET OF */
  { "logicaland",     0x2227 },  /* LOGICAL AND */
  { "logicalor",      0x2228 },  /* LOGICAL OR */
  { "circleplus",     0x2295 },  /* CIRCLED PLUS */
  { "circlemultiply", 0x2297 },  /* CIRCLED TIMES */

  /* Arrows */
  { "arrowleft",      0x2190 },  /* LEFTWARDS ARROW */
  { "arrowup",        0x2191 },  /* UPWARDS ARROW */
  { "arrowright",     0x2192 },  /* RIGHTWARDS ARROW */
  { "arrowdown",      0x2193 },  /* DOWNWARDS ARROW */
  { "arrowboth",      0x2194 },  /* LEFT RIGHT ARROW */
  { "arrowdblleft",   0x21D0 },  /* LEFTWARDS DOUBLE ARROW */
  { "arrowdblup",     0x21D1 },  /* UPWARDS DOUBLE ARROW */
  { "arrowdblright",  0x21D2 },  /* RIGHTWARDS DOUBLE ARROW */
  { "arrowdbldown",   0x21D3 },  /* DOWNWARDS DOUBLE ARROW */
  { "arrowdblboth",   0x21D4 },  /* LEFT RIGHT DOUBLE ARROW */
  /* Arrow body extensions (used by Symbol for tall arrows); the */
  /* vertical/horizontal "ex" pieces don't have dedicated Unicode  */
  /* points -- approximate with box-drawing verticals/horizontals.  */
  { "arrowvertex",    0x23D0 },  /* VERTICAL LINE EXTENSION (arrow body) */
  { "arrowhorizex",   0x23AF },  /* HORIZONTAL LINE EXTENSION */

  /* Card suits and miscellany */
  { "club",           0x2663 },
  { "diamond",        0x2666 },  /* (note: also matched by Symbol-font users) */
  { "heart",          0x2665 },
  { "spade",          0x2660 },
  { "carriagereturn", 0x21B5 },  /* DOWNWARDS ARROW WITH CORNER LEFTWARDS */

  /* Stretched / tall fences (multi-glyph: top/middle/bottom variants).      */
  /* Unicode encodes these as the "bracket pieces" U+239B..U+23AE; map each */
  /* Adobe variant to its canonical piece so KaTeX/text renderers can lay   */
  /* out big delimiters when the font is set to Symbol.                     */
  { "parenlefttp",    0x239B },
  { "parenleftex",    0x239C },
  { "parenleftbt",    0x239D },
  { "parenrighttp",   0x239E },
  { "parenrightex",   0x239F },
  { "parenrightbt",   0x23A0 },
  { "bracketlefttp",  0x23A1 },
  { "bracketleftex",  0x23A2 },
  { "bracketleftbt",  0x23A3 },
  { "bracketrighttp", 0x23A4 },
  { "bracketrightex", 0x23A5 },
  { "bracketrightbt", 0x23A6 },
  { "bracelefttp",    0x23A7 },
  { "braceleftmid",   0x23A8 },
  { "braceleftbt",    0x23A9 },
  { "bracerighttp",   0x23AB },
  { "bracerightmid",  0x23AC },
  { "bracerightbt",   0x23AD },
  { "braceex",        0x23AA },
  { "integraltp",     0x2320 },  /* TOP HALF INTEGRAL */
  { "integralex",     0x23AE },  /* INTEGRAL EXTENSION */
  { "integralbt",     0x2321 },  /* BOTTOM HALF INTEGRAL */

  /* ================================================================== */
  /* Adobe Zapf Dingbats (Ding.LCM): glyph names a1..a206 map into the   */
  /* Unicode "Dingbats" block U+2700..U+27BF.  The names below follow    */
  /* the Adobe Glyph List for the Zapf Dingbats font; codepoints come   */
  /* from Unicode 6.0's official ITC Zapf Dingbats mapping.              */
  /* Only names that appear in lout/maps/Ding.LCM are listed.            */
  /* ================================================================== */
  { "a1",   0x2701 }, { "a2",   0x2702 }, { "a202", 0x2703 },
  { "a3",   0x2704 }, { "a4",   0x260E }, { "a5",   0x2706 },
  { "a119", 0x2707 }, { "a118", 0x2708 }, { "a117", 0x2709 },
  { "a11",  0x261B }, { "a12",  0x261E }, { "a13",  0x270C },
  { "a14",  0x270D }, { "a15",  0x270E }, { "a16",  0x270F },
  { "a105", 0x2710 }, { "a17",  0x2711 }, { "a18",  0x2712 },
  { "a19",  0x2713 }, { "a20",  0x2714 }, { "a21",  0x2715 },
  { "a22",  0x2716 }, { "a23",  0x2717 }, { "a24",  0x2718 },
  { "a25",  0x2719 }, { "a26",  0x271A }, { "a27",  0x271B },
  { "a28",  0x271C }, { "a6",   0x271D }, { "a7",   0x271E },
  { "a8",   0x271F }, { "a9",   0x2720 }, { "a10",  0x2721 },
  { "a29",  0x2722 }, { "a30",  0x2723 }, { "a31",  0x2724 },
  { "a32",  0x2725 }, { "a33",  0x2726 }, { "a34",  0x2727 },
  { "a35",  0x2605 }, { "a36",  0x2729 }, { "a37",  0x272A },
  { "a38",  0x272B }, { "a39",  0x272C }, { "a40",  0x272D },
  { "a41",  0x272E }, { "a42",  0x272F }, { "a43",  0x2730 },
  { "a44",  0x2731 }, { "a45",  0x2732 }, { "a46",  0x2733 },
  { "a47",  0x2734 }, { "a48",  0x2735 }, { "a49",  0x2736 },
  { "a50",  0x2737 }, { "a51",  0x2738 }, { "a52",  0x2739 },
  { "a53",  0x273A }, { "a54",  0x273B }, { "a55",  0x273C },
  { "a56",  0x273D }, { "a57",  0x273E }, { "a58",  0x273F },
  { "a59",  0x2740 }, { "a60",  0x2741 }, { "a61",  0x2742 },
  { "a62",  0x2743 }, { "a63",  0x2744 }, { "a64",  0x2745 },
  { "a65",  0x2746 }, { "a66",  0x2747 }, { "a67",  0x2748 },
  { "a68",  0x2749 }, { "a69",  0x274A }, { "a70",  0x274B },
  { "a71",  0x25CF }, { "a72",  0x274D }, { "a73",  0x25A0 },
  { "a74",  0x274F }, { "a203", 0x2750 }, { "a75",  0x2751 },
  { "a204", 0x2752 }, { "a76",  0x25B2 }, { "a77",  0x25BC },
  { "a78",  0x25C6 }, { "a79",  0x2756 }, { "a81",  0x25D7 },
  { "a82",  0x2758 }, { "a83",  0x2759 }, { "a84",  0x275A },
  { "a97",  0x275B }, { "a98",  0x275C }, { "a99",  0x275D },
  { "a100", 0x275E }, { "a101", 0x2761 }, { "a102", 0x2762 },
  { "a103", 0x2763 }, { "a104", 0x2764 }, { "a106", 0x2765 },
  { "a107", 0x2766 }, { "a108", 0x2767 }, { "a112", 0x2663 },
  { "a111", 0x2666 }, { "a110", 0x2665 }, { "a109", 0x2660 },
  { "a120", 0x2460 }, { "a121", 0x2461 }, { "a122", 0x2462 },
  { "a123", 0x2463 }, { "a124", 0x2464 }, { "a125", 0x2465 },
  { "a126", 0x2466 }, { "a127", 0x2467 }, { "a128", 0x2468 },
  { "a129", 0x2469 }, { "a130", 0x2776 }, { "a131", 0x2777 },
  { "a132", 0x2778 }, { "a133", 0x2779 }, { "a134", 0x277A },
  { "a135", 0x277B }, { "a136", 0x277C }, { "a137", 0x277D },
  { "a138", 0x277E }, { "a139", 0x277F }, { "a140", 0x2780 },
  { "a141", 0x2781 }, { "a142", 0x2782 }, { "a143", 0x2783 },
  { "a144", 0x2784 }, { "a145", 0x2785 }, { "a146", 0x2786 },
  { "a147", 0x2787 }, { "a148", 0x2788 }, { "a149", 0x2789 },
  { "a150", 0x278A }, { "a151", 0x278B }, { "a152", 0x278C },
  { "a153", 0x278D }, { "a154", 0x278E }, { "a155", 0x278F },
  { "a156", 0x2790 }, { "a157", 0x2791 }, { "a158", 0x2792 },
  { "a159", 0x2793 }, { "a160", 0x2794 }, { "a161", 0x2192 },
  { "a163", 0x2194 }, { "a164", 0x2195 }, { "a196", 0x2798 },
  { "a165", 0x2799 }, { "a192", 0x279A }, { "a166", 0x279B },
  { "a167", 0x279C }, { "a168", 0x279D }, { "a169", 0x279E },
  { "a170", 0x279F }, { "a171", 0x27A0 }, { "a172", 0x27A1 },
  { "a173", 0x27A2 }, { "a162", 0x27A3 }, { "a174", 0x27A4 },
  { "a175", 0x27A5 }, { "a176", 0x27A6 }, { "a177", 0x27A7 },
  { "a178", 0x27A8 }, { "a179", 0x27A9 }, { "a193", 0x27AA },
  { "a180", 0x27AB }, { "a199", 0x27AC }, { "a181", 0x27AD },
  { "a200", 0x27AE }, { "a182", 0x27AF }, { "a201", 0x27B1 },
  { "a183", 0x27B2 }, { "a184", 0x27B3 }, { "a197", 0x27B4 },
  { "a185", 0x27B5 }, { "a194", 0x27B6 }, { "a198", 0x27B7 },
  { "a186", 0x27B8 }, { "a195", 0x27B9 }, { "a187", 0x27BA },
  { "a188", 0x27BB }, { "a189", 0x27BC }, { "a190", 0x27BD },
  { "a191", 0x27BE },

  { NULL,             0      }
};


/* Glyph-name -> Unicode lookup: open-addressed hash over svg_glyph_table.   */
/* The plain linear scan that lived here previously cost one strcmp per     */
/* (entry, character) pair, with ~380 entries the table is short but the    */
/* User's Guide goes through hundreds of thousands of characters -- the    */
/* scan dominated svg_emit_word_text once the dict and op-dispatch hashes   */
/* landed.  Lazily-built power-of-two open-addressing table (FNV-1a) keyed  */
/* on the entry name; lookup hits in 1-2 probes on average.                 */
#define SVG_GLYPH_HASH_SIZE 1024
#define SVG_GLYPH_HASH_MASK (SVG_GLYPH_HASH_SIZE - 1)
struct svg_glyph_hash_entry {
  const char    *name;
  unsigned int   hash;
  unsigned int   cp;
};
static struct svg_glyph_hash_entry svg_glyph_hash[SVG_GLYPH_HASH_SIZE];
static int svg_glyph_hash_built = 0;

static void svg_glyph_hash_build(void)
{
  int i;
  unsigned int h, slot;
  for( i = 0; i < SVG_GLYPH_HASH_SIZE; i++ )
  {
    svg_glyph_hash[i].name = NULL;
    svg_glyph_hash[i].hash = 0;
    svg_glyph_hash[i].cp   = 0;
  }
  for( i = 0; svg_glyph_table[i].name != NULL; i++ )
  {
    h = svg_name_hash(svg_glyph_table[i].name);
    slot = h & (unsigned int) SVG_GLYPH_HASH_MASK;
    while( svg_glyph_hash[slot].name != NULL )
      slot = (slot + 1) & (unsigned int) SVG_GLYPH_HASH_MASK;
    svg_glyph_hash[slot].name = svg_glyph_table[i].name;
    svg_glyph_hash[slot].hash = h;
    svg_glyph_hash[slot].cp   = svg_glyph_table[i].cp;
  }
  svg_glyph_hash_built = 1;
}

static unsigned int svg_glyph_to_unicode(const char *name)
{
  unsigned int h, slot;
  const struct svg_glyph_hash_entry *e;
  if( name == NULL || name[0] == '\0' )
    return 0;
  if( name[0] == '-' && strcmp(name, "-none-") == 0 )
    return 0;
  if( !svg_glyph_hash_built )
    svg_glyph_hash_build();
  h = svg_name_hash(name);
  slot = h & (unsigned int) SVG_GLYPH_HASH_MASK;
  for( ;; )
  {
    e = &svg_glyph_hash[slot];
    if( e->name == NULL )
      return 0;
    if( e->hash == h && strcmp(e->name, name) == 0 )
      return e->cp;
    slot = (slot + 1) & (unsigned int) SVG_GLYPH_HASH_MASK;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  static void svg_emit_utf8(unsigned int cp)                                */
/*                                                                           */
/*  Emit codepoint cp as 1-4 UTF-8 bytes, XML-escaping the seven well-known  */
/*  reserved ASCII characters.                                               */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_utf8(unsigned int cp)
{
  if( cp < 0x80 )
  {
    if( cp == '<' )       fputs("&lt;",   out_fp);
    else if( cp == '>' )  fputs("&gt;",   out_fp);
    else if( cp == '&' )  fputs("&amp;",  out_fp);
    else if( cp == '"' )  fputs("&quot;", out_fp);
    else if( cp == '\'' ) fputs("&apos;", out_fp);
    else                  fputc((int) cp, out_fp);
  }
  else if( cp < 0x800 )
  {
    fputc((int) (0xC0 | (cp >> 6)),   out_fp);
    fputc((int) (0x80 | (cp & 0x3F)), out_fp);
  }
  else if( cp < 0x10000 )
  {
    fputc((int) (0xE0 | (cp >> 12)),         out_fp);
    fputc((int) (0x80 | ((cp >> 6) & 0x3F)), out_fp);
    fputc((int) (0x80 | (cp & 0x3F)),        out_fp);
  }
  else
  {
    fputc((int) (0xF0 | (cp >> 18)),          out_fp);
    fputc((int) (0x80 | ((cp >> 12) & 0x3F)), out_fp);
    fputc((int) (0x80 | ((cp >>  6) & 0x3F)), out_fp);
    fputc((int) (0x80 | (cp & 0x3F)),         out_fp);
  }
}


/*****************************************************************************/
/*                                                                           */
/*  static void svg_emit_xml_escaped(const FULL_CHAR *s)                     */
/*                                                                           */
/*  Write s to out_fp with XML special characters escaped.  Bytes >= 0x80    */
/*  are expanded as Latin-1 -> 2-byte UTF-8 sequences.  Fallback path when   */
/*  no LCM mapping is available.                                             */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_xml_escaped(const FULL_CHAR *s)
{
  const FULL_CHAR *p;
  unsigned int c;
  if( s == NULL )
    return;
  for( p = s; *p != '\0'; p++ )
  {
    c = (unsigned int) *p;
    svg_emit_utf8(c);
  }
}


/*****************************************************************************/
/*                                                                           */
/*  static unsigned int svg_byte_to_codepoint(MAP_VEC mv, unsigned int c)    */
/*                                                                           */
/*  Helper: map one input byte to a Unicode codepoint through mv, falling    */
/*  back to direct Latin-1 -> UTF-8 when the byte has no glyph-name entry    */
/*  in mv or mv is NULL.                                                     */
/*                                                                           */
/*****************************************************************************/

static unsigned int svg_byte_to_codepoint(MAP_VEC mv, unsigned int c)
{
  unsigned int cp;
  OBJECT name_obj;
  FULL_CHAR *gname;

  cp = 0;
  if( mv != NULL )
  {
    name_obj = mv->vector[c];
    if( name_obj != NULL && is_word(type(name_obj)) )
    {
      gname = string(name_obj);
      if( gname != NULL )
        cp = svg_glyph_to_unicode((const char *) gname);
    }
  }
  if( cp == 0 )
    cp = c;       /* fallback: Latin-1 -> UTF-8 */
  return cp;
}


/*****************************************************************************/
/*                                                                           */
/*  static void svg_emit_word_text(FONT_NUM fnum, FULL_CHAR *s, OBJECT x,    */
/*                                 double size_pt)                           */
/*                                                                           */
/*  Emit the text of a word by mapping each byte through the font's LCM     */
/*  vector to a glyph name, then to Unicode.  Falls back to direct          */
/*  Latin-1 -> UTF-8 if no mapping exists.                                  */
/*                                                                           */
/*  When the font's AFM kern table has an entry for an adjacent pair of     */
/*  glyphs, the second glyph is wrapped in a <tspan dx="..."> so the SVG    */
/*  consumer matches the spacing PostScript would have achieved via the    */
/*  font's `show` operator.  FontKernLength returns the kern value in Lout  */
/*  internal units, already scaled to the current font size, with the same  */
/*  sign convention SVG dx uses: negative tightens, positive loosens.  So   */
/*  the dx emitted is simply ksize / PT.                                    */
/*                                                                           */
/*  Kerning is suppressed when:                                              */
/*    - size_pt < 6  (sub-resolution spacing not worth the byte cost)        */
/*    - no LCM mapping is active (no unacc_map available for FontKernLength */
/*      and Symbol/Dingbats fonts ship without useful kern tables anyway)    */
/*    - the font has no kern table (kern_sizes == NULL)                     */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_word_text(FONT_NUM fnum, FULL_CHAR *s, OBJECT x,
  double size_pt)
{
  MAPPING m;
  MAP_VEC mv;
  FULL_CHAR *unacc;
  const FULL_CHAR *p;
  unsigned int c, cp;
  BOOLEAN do_kern;
  FULL_LENGTH ksize;
  double dx_pt;

  if( s == NULL )
    return;

  mv = NULL;
  unacc = NULL;
  do_kern = FALSE;
  m = FontMapping(fnum, &fpos(x));
  if( m != 0 && MapTable != NULL && MapTable[m] != NULL )
  {
    mv = MapTable[m];
    unacc = MapTable[m]->map[MAP_UNACCENTED];
    if( unacc != NULL && size_pt >= 6.0 &&
        finfo[fnum].kern_sizes != (FULL_LENGTH *) NULL )
      do_kern = TRUE;
  }

  if( !do_kern )
  {
    /* fast path: no kerning, just emit codepoints */
    for( p = s; *p != '\0'; p++ )
    {
      cp = svg_byte_to_codepoint(mv, (unsigned int) *p);
      svg_emit_utf8(cp);
    }
    return;
  }

  /* slow path: emit a <tspan dx="..."> at every kern point */
  for( p = s; *p != '\0'; p++ )
  {
    c = (unsigned int) *p;
    if( p != s )
    {
      ksize = FontKernLength(fnum, unacc, (FULL_CHAR) *(p-1), (FULL_CHAR) *p);
      if( ksize != 0 )
      {
        /* ksize is the AFM kern value scaled to the current font size, in   */
        /* Lout internal units (PT = 20 units/pt).  The same value is added  */
        /* to fwd(x, COLM) in FontWordSize, so a negative ksize tightens the */
        /* pair (e.g. KPX A V -135 in Times) and a positive value loosens.   */
        /* SVG tspan dx follows the same sign convention -- positive dx     */
        /* widens, negative tightens -- so emit ksize/PT directly.           */
        dx_pt = ((double) ksize) / PT;
        fprintf(out_fp, "<tspan dx=\"%.4f\">", dx_pt);
        cp = svg_byte_to_codepoint(mv, c);
        svg_emit_utf8(cp);
        fputs("</tspan>", out_fp);
        continue;
      }
    }
    cp = svg_byte_to_codepoint(mv, c);
    svg_emit_utf8(cp);
  }
}


/*****************************************************************************/
/*                                                                           */
/*  static void SVG_PrintLength(FULL_CHAR *buff, int length, int length_dim) */
/*                                                                           */
/*****************************************************************************/

static void SVG_PrintLength(FULL_CHAR *buff, int length, int length_dim)
{
  sprintf( (char *) buff, "%.3fc", (float) length/CM);
} /* end SVG_PrintLength */


/*****************************************************************************/
/*                                                                           */
/*  Stub callbacks: unused or trivial.                                       */
/*                                                                           */
/*****************************************************************************/

static void SVG_PrintPageSetupForFont(OBJECT face, int font_curr_page,
  FULL_CHAR *font_name, FULL_CHAR *first_size_str)
{}

static void SVG_PrintPageResourceForFont(FULL_CHAR *font_name, BOOLEAN first)
{}

static void SVG_PrintMapping(MAPPING m)
{}

/* Forward declaration; defined after the PS interpreter section.           */
static void svg_ingest_prepend_files(void);

static void SVG_PrintBeforeFirstPage(FULL_LENGTH h, FULL_LENGTH v,
  FULL_CHAR *label)
{
  pagecount = 1;
  svg_open_page(h, v, label);
  svg_ingest_prepend_files();
}

static void SVG_PrintBetweenPages(FULL_LENGTH h, FULL_LENGTH v,
  FULL_CHAR *label)
{
  svg_close_page();
  pagecount++;
  svg_open_page(h, v, label);
}

static void SVG_PrintAfterLastPage(void)
{
  svg_close_page();
  svg_psinterp_shutdown();
  /* Flush the 128 KB out_fp buffer set in SVG_PrintInitialize so the file  */
  /* is fully committed before Lout proper exits (some callers rely on the */
  /* file being readable immediately after lout returns).                  */
  if( out_fp != NULL )
    fflush(out_fp);
}


/*****************************************************************************/
/*                                                                           */
/*  SVG_PrintWord                                                            */
/*                                                                           */
/*  In the page-level y-flipped frame the literal Lout coordinates can be    */
/*  used directly.  Wrap the <text> in a counter-flip <g> so the glyphs      */
/*  stay upright.                                                            */
/*                                                                           */
/*****************************************************************************/

/* Per-font face-flag cache.  FontFace() returns a stable pointer keyed on   */
/* fnum, and FontFamily()'s string never changes either, but SVG_PrintWord  */
/* fires for every word in the document (~99k times on the User's Guide) -- */
/* each call previously ran 4 strstr() probes against the face string to    */
/* decide font-weight/font-style.  Hash by fnum % SVG_FACE_CACHE_SIZE with  */
/* linear probing on collision; on hit we replay the cached flags and the   */
/* pre-formatted "<g transform...><text ..." opening so the per-word cost   */
/* drops to a single fputs of an already-prepared header plus the variable   */
/* x/y/colour/size pieces.                                                   */
#define SVG_FACE_CACHE_SIZE 64
#define SVG_FACE_CACHE_MASK (SVG_FACE_CACHE_SIZE - 1)
struct svg_face_cache_entry {
  FONT_NUM    fnum;          /* 0 means unused                              */
  BOOLEAN     is_bold;
  BOOLEAN     is_italic;
  const char *family;        /* cached FontFamily() pointer                  */
  /* pre-formatted attribute fragment for the <text> open tag covering the  */
  /* font-family / weight / style attributes (everything that depends only  */
  /* on fnum, i.e. not on x/y/size/colour).  Pre-rendered as a single chunk */
  /* so SVG_PrintWord can fwrite it whole instead of issuing 3-5 stdio      */
  /* calls per word.                                                         */
  char        attr_chunk[160];
  int         attr_len;
};
static struct svg_face_cache_entry svg_face_cache[SVG_FACE_CACHE_SIZE];

static struct svg_face_cache_entry *svg_face_cache_get(FONT_NUM fnum)
{
  unsigned int slot, probes;
  struct svg_face_cache_entry *e;
  FULL_CHAR *fname, *fface;
  int n;

  slot = (unsigned int) fnum & (unsigned int) SVG_FACE_CACHE_MASK;
  for( probes = 0; probes < SVG_FACE_CACHE_SIZE; probes++ )
  {
    e = &svg_face_cache[slot];
    if( e->fnum == fnum )
      return e;
    if( e->fnum == 0 )
    {
      /* fill the slot */
      e->fnum    = fnum;
      fname      = FontFamily(fnum);
      fface      = FontFace(fnum);
      e->family  = fname == NULL ? "serif" : (const char *) fname;
      e->is_bold = FALSE;
      e->is_italic = FALSE;
      if( fface != NULL )
      {
        if( strstr((const char *) fface, "Bold") != NULL )
          e->is_bold = TRUE;
        if( strstr((const char *) fface, "Italic") != NULL ||
            strstr((const char *) fface, "Slope")  != NULL ||
            strstr((const char *) fface, "Oblique") != NULL )
          e->is_italic = TRUE;
      }
      /* Pre-render only the family attribute; weight/style are appended    */
      /* after font-size in the emit step to keep the SVG attribute order    */
      /* byte-for-byte identical to the pre-cache emit.                      */
      n = sprintf(e->attr_chunk, " font-family=\"%s\"", e->family);
      e->attr_len = n;
      return e;
    }
    slot = (slot + 1) & (unsigned int) SVG_FACE_CACHE_MASK;
  }
  /* table full -- shouldn't happen in practice (~50 fonts max in User's    */
  /* Guide); fall through with a stack-local non-cached entry the caller     */
  /* will read but won't be re-found on next probe.                          */
  return NULL;
}

static void svg_face_cache_clear(void)
{
  int i;
  for( i = 0; i < SVG_FACE_CACHE_SIZE; i++ )
  {
    svg_face_cache[i].fnum     = 0;
    svg_face_cache[i].attr_len = 0;
  }
}

static void SVG_PrintWord(OBJECT x, int hpos, int vpos)
{
  FONT_NUM fnum;
  FULL_LENGTH fsize, fhxh;
  double x_pt, y_pt, size_pt;
  char colourbuf[32];
  const char *colour_str;
  struct svg_face_cache_entry *fc;
  BOOLEAN is_bold, is_italic;
  const char *family_attr;       /* always begins with " font-family=..."   */
  char family_attr_local[200];

  if( out_fp == NULL || !page_open )
    return;

  fnum   = word_font(x);
  fsize  = FontSize(fnum, x);
  fhxh   = FontHalfXHeight(fnum);

  fc = svg_face_cache_get(fnum);
  if( fc != NULL )
  {
    family_attr = fc->attr_chunk;
    is_bold     = fc->is_bold;
    is_italic   = fc->is_italic;
  }
  else
  {
    /* Cache full -- rebuild attrs on the stack.  Extremely rare in practice */
    /* (SVG_FACE_CACHE_SIZE is 64; the User's Guide uses ~50 fonts at most). */
    FULL_CHAR *fname = FontFamily(fnum);
    FULL_CHAR *fface = FontFace(fnum);
    const char *fam  = fname == NULL ? "serif" : (const char *) fname;
    is_bold   = (fface != NULL && strstr((const char *) fface, "Bold") != NULL);
    is_italic = (fface != NULL &&
                 (strstr((const char *) fface, "Italic")  != NULL ||
                  strstr((const char *) fface, "Slope")   != NULL ||
                  strstr((const char *) fface, "Oblique") != NULL));
    sprintf(family_attr_local, " font-family=\"%s\"", fam);
    family_attr = family_attr_local;
  }

  /* Lout vpos marks the x-height midline; shift down by half-xheight to    */
  /* reach the baseline.  Coordinates are in the Lout (bottom-left) frame   */
  /* of the enclosing flipped group: text baseline at y = vpos - fhxh.      */
  x_pt    = (double) hpos          / PT;
  y_pt    = (double) (vpos - fhxh) / PT;
  size_pt = (double) fsize         / PT;

  colour_str = svg_colour_rgb(word_colour(x), colourbuf);

  /* Counter-flip wrapper so the glyph is upright inside the page-level Y-  */
  /* flip group.  The text origin lives at (x_pt, y_pt) in flipped coords; */
  /* after scale(1,-1) the local frame is back to top-left, so x="0" y="0" */
  /* anchors the baseline.  Attribute order matches the pre-cache emit       */
  /* exactly so the SVG is byte-for-byte identical to baseline.              */
  fprintf(out_fp,
    "<g transform=\"translate(%.3f,%.3f) scale(1,-1)\">"
    "<text x=\"0\" y=\"0\"%s font-size=\"%.3f\"%s%s%s%s%s>",
    x_pt, y_pt,
    family_attr, size_pt,
    is_bold   ? " font-weight=\"bold\""  : "",
    is_italic ? " font-style=\"italic\"" : "",
    colour_str != NULL ? " fill=\""     : "",
    colour_str != NULL ? colour_str     : "",
    colour_str != NULL ? "\""            : "");
  svg_emit_word_text(fnum, string(x), x, size_pt);
  fputs("</text></g>\n", out_fp);
}

static void SVG_PrintPlainGraphic(OBJECT x, FULL_LENGTH xmk, FULL_LENGTH ymk,
  OBJECT z)
{}


/*****************************************************************************/
/*                                                                           */
/*  SVG_PrintUnderline - draw a horizontal underline at ymk - underline_pos. */
/*  Coordinates are emitted in the page-level flipped frame, so no per-call */
/*  Y inversion is required.                                                 */
/*                                                                           */
/*****************************************************************************/

static void SVG_PrintUnderline(FONT_NUM fnum, COLOUR_NUM col, TEXTURE_NUM pat,
  FULL_LENGTH xstart, FULL_LENGTH xstop, FULL_LENGTH ymk)
{
  double y_svg, thick;
  char colourbuf[32];
  const char *colour_str;
  if( out_fp == NULL || !page_open )
    return;
  y_svg = (double) (ymk - finfo[fnum].underline_pos) / PT;
  thick = (double) finfo[fnum].underline_thick / PT;
  colour_str = svg_colour_rgb(col, colourbuf);
  fprintf(out_fp,
    "<line x1=\"%.3f\" y1=\"%.3f\" x2=\"%.3f\" y2=\"%.3f\" "
    "stroke=\"%s\" stroke-width=\"%.3f\"/>\n",
    (double) xstart / PT, y_svg,
    (double) xstop  / PT, y_svg,
    colour_str == NULL ? "currentColor" : colour_str, thick);
}


/*****************************************************************************/
/*                                                                           */
/*  Graphics state and coordinate transforms                                 */
/*                                                                           */
/*  Coordinates inside the page-level flipped group use Lout's (bottom-left) */
/*  convention directly, so translate/rotate are emitted unnegated.         */
/*                                                                           */
/*****************************************************************************/

static void svg_open_transform_g(const char *transform)
{
  if( out_fp == NULL || gs_top < 0 )
    return;
  fprintf(out_fp, "<g transform=\"%s\">\n", transform);
  gs_groups[gs_top]++;
}

/* Multiply two 6-element affine matrices: out = a * b.  (Module-level    */
/* helper for the outer CTM tracker; the interpreter has its own copy.)   */
static void svg_outer_mat_mul(const double *a, const double *b, double *out)
{
  double r[6];
  r[0] = a[0]*b[0] + a[1]*b[2];
  r[1] = a[0]*b[1] + a[1]*b[3];
  r[2] = a[2]*b[0] + a[3]*b[2];
  r[3] = a[2]*b[1] + a[3]*b[3];
  r[4] = a[4]*b[0] + a[5]*b[2] + b[4];
  r[5] = a[4]*b[1] + a[5]*b[3] + b[5];
  out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
  out[3] = r[3]; out[4] = r[4]; out[5] = r[5];
}

/* Forward declarations: the C-side graphics-state callbacks below mirror   */
/* their effects onto the module-persistent PS interpreter state defined    */
/* further down in the file.                                                */
static void svg_ps_module_gsave(void);
static void svg_ps_module_grestore(void);
static void svg_ps_module_translate(double tx, double ty);
static void svg_ps_module_rotate(double angle_deg);
static void svg_ps_module_scale(double sx, double sy);

static void SVG_SaveGraphicState(OBJECT x)
{
  int i;
  if( out_fp == NULL )
    return;
  if( gs_top + 1 >= SVG_MAX_GS )
  {
    Error(53, 1, "graphics state stack overflow in SVG back end (max %d)",
      FATAL, &fpos(x), SVG_MAX_GS);
    return;
  }
  gs_top++;
  gs_groups[gs_top] = 0;
  /* Snapshot the outer CTM so SVG_RestoreGraphicState can revert it.       */
  for( i = 0; i < 6; i++ )
    svg_outer_ctm_stack[gs_top][i] = svg_outer_ctm[i];
  /* Mirror onto the persistent PS interpreter: gsave its gs stack so       */
  /* the per-interpreter CTM tracks the SVG <g> chain through nested       */
  /* @Graphic invocations.                                                  */
  svg_ps_module_gsave();
}

static void SVG_RestoreGraphicState(void)
{
  int i;
  if( out_fp == NULL || gs_top < 0 )
    return;
  for( i = 0; i < gs_groups[gs_top]; i++ )
    fputs("</g>\n", out_fp);
  /* Revert the outer CTM to its pre-save state.                            */
  for( i = 0; i < 6; i++ )
    svg_outer_ctm[i] = svg_outer_ctm_stack[gs_top][i];
  gs_top--;
  svg_ps_module_grestore();
}

static void SVG_CoordTranslate(FULL_LENGTH xdist, FULL_LENGTH ydist)
{
  char buf[96];
  double t[6];
  sprintf(buf, "translate(%.3f,%.3f)",
    (double) xdist / PT, (double) ydist / PT);
  svg_open_transform_g(buf);
  /* Update outer CTM: CTM' = translate(xdist, ydist) * CTM.  We track     */
  /* operands in Lout internal units (multiples of PT) to match what       */
  /* PS-interpreter code expects to see for transform/itransform.          */
  t[0] = 1.0; t[1] = 0.0; t[2] = 0.0; t[3] = 1.0;
  t[4] = (double) xdist; t[5] = (double) ydist;
  svg_outer_mat_mul(t, svg_outer_ctm, svg_outer_ctm);
  /* Mirror into the persistent PS interpreter's CTM AND baseline_m so    */
  /* paths drawn AFTER this point have an identity path-delta with respect */
  /* to the SVG <g transform> just opened.                                 */
  svg_ps_module_translate((double) xdist, (double) ydist);
}

static void SVG_CoordRotate(FULL_LENGTH amount)
{
  char buf[64];
  double rad, c, sn;
  double t[6];
  /* Both Lout and SVG (inside a Y-flipped group) rotate counter-clockwise  */
  /* for positive angles, so no negation is needed.                         */
  sprintf(buf, "rotate(%.4f)", (double) amount / DG);
  svg_open_transform_g(buf);
  rad = ((double) amount / DG) * SVG_PI / 180.0;
  c = cos(rad); sn = sin(rad);
  t[0] = c;   t[1] = sn;
  t[2] = -sn; t[3] = c;
  t[4] = 0.0; t[5] = 0.0;
  svg_outer_mat_mul(t, svg_outer_ctm, svg_outer_ctm);
  svg_ps_module_rotate((double) amount / DG);
}

static void SVG_CoordScale(float hfactor, float vfactor)
{
  char buf[64];
  double t[6];
  sprintf(buf, "scale(%.4f,%.4f)", (double) hfactor, (double) vfactor);
  svg_open_transform_g(buf);
  t[0] = (double) hfactor; t[1] = 0.0;
  t[2] = 0.0; t[3] = (double) vfactor;
  t[4] = 0.0; t[5] = 0.0;
  svg_outer_mat_mul(t, svg_outer_ctm, svg_outer_ctm);
  svg_ps_module_scale((double) hfactor, (double) vfactor);
}

static void SVG_CoordHMirror(void)
{
  svg_open_transform_g("matrix(-1,0,0,1,0,0)");
}

static void SVG_CoordVMirror(void)
{
  svg_open_transform_g("matrix(1,0,0,-1,0,0)");
}


/*****************************************************************************/
/*                                                                           */
/*  Concatenate @Graphic content into one flat buffer.                       */
/*                                                                           */
/*  Unlike the previous stub, GAP_OBJ separators are turned into spaces so    */
/*  the PS tokeniser can find operator boundaries.                           */
/*                                                                           */
/*****************************************************************************/

static int svg_graphic_concat(OBJECT x, char *buf, int pos, int buf_size)
{
  OBJECT y, link;
  FULL_CHAR *s;
  int slen;
  if( x == NULL || pos >= buf_size - 2 )
    return pos;
  switch( type(x) )
  {
    case WORD:
    case QWORD:
      s = string(x);
      if( s == NULL )
        break;
      slen = (int) strlen((const char *) s);
      if( pos + slen >= buf_size - 2 )
        slen = buf_size - 2 - pos;
      memcpy(buf + pos, s, slen);
      pos += slen;
      /* always end a WORD with a separating space so adjacent words don't */
      /* fuse into bogus tokens                                            */
      buf[pos++] = ' ';
      buf[pos] = '\0';
      break;
    case ACAT:
      for( link = Down(x); link != x; link = NextDown(link) )
      {
        Child(y, link)
          ;
        if( type(y) == GAP_OBJ )
        {
          if( pos < buf_size - 2 )
          {
            buf[pos++] = ' ';
            buf[pos] = '\0';
          }
          continue;
        }
        if( is_word(type(y)) || type(y) == ACAT )
          pos = svg_graphic_concat(y, buf, pos, buf_size);
      }
      break;
    default:
      break;
  }
  return pos;
}


/*****************************************************************************/
/*                                                                           */
/*  PostScript-to-SVG drawing-op interpreter                                 */
/*                                                                           */
/*  A small stack-machine that recognises the subset of operators emitted   */
/*  by Lout's standard library for boxes, rules, page borders, table grids   */
/*  and trivially-shaped @Graphic clauses.  More elaborate prologues (Diag,  */
/*  Fig, Eq, Graph) define their own procedures with /name { ... } def and  */
/*  use the dictionary stack, arrays and control flow operators -- those     */
/*  are intentionally NOT implemented here.  Unknown tokens are skipped so   */
/*  no comment/exception is raised; the resulting SVG simply omits the     */
/*  fancy primitive rather than crashing.                                   */
/*                                                                           */
/*****************************************************************************/

/* per-interpreter graphics state */
typedef struct svg_gstate {
  char  stroke_rgb[24];         /* "rgb(...)" or empty for default */
  char  fill_rgb[24];           /* "rgb(...)" or empty for default */
  double line_width;            /* in PT; 0 means unset */
  char  dasharray[SVG_DASH_BUF_SIZE];  /* "" means none */
  /* CTM: 6-element affine matrix [a b c d tx ty] mapping user space to     */
  /* device space.  Used to round-trip points through transform/itransform  */
  /* (needed by @Diag's ldiagpointdef so link endpoints stay anchored to   */
  /* their owning circle's coordinate frame).                              */
  double m[6];
  /* baseline_m: the CTM as it was at the start of the current externally-  */
  /* visible coordinate frame (i.e. just after the last LoutGr2 / gsave    */
  /* whose translate is mirrored by an SVG <g transform="..."> group).     */
  /* The "path-delta" applied to emitted SVG path coords is CTM relative   */
  /* to baseline_m -- this is what hasn't already been baked in by the     */
  /* surrounding <g> chain.  LoutGr2 snaps baseline_m up to its new CTM    */
  /* after applying its translate, so the path-delta stays zero across    */
  /* externally-mirrored translates.  PS-internal translate/scale/rotate  */
  /* (e.g. inside ldiagsetarc) move CTM without touching baseline_m, so  */
  /* their effect appears in the path-delta and is baked into path coords. */
  double baseline_m[6];
  /* clip_empty: set to 1 when a `clip` operator was executed with an     */
  /* empty current path -- such a clip masks out all subsequent drawing  */
  /* in the current graphics state and any gsave'd children of it.  Used */
  /* by the @Diag prologue (ldiagdolinkdraw's `newpath clip gsave`       */
  /* idiom) to gate the spurious back-arrowhead node when `backarrow` is */
  /* not requested: the prologue still emits the node, but wraps it in   */
  /* an empty-clip region so it renders invisibly in PS.  Without        */
  /* honouring this in SVG we'd see a stray arrowhead glyph at the      */
  /* origin.  The flag is copied by gsave (full gstate copy) and        */
  /* restored by grestore (gs_top--), exactly mirroring PS clip-stack   */
  /* semantics.                                                          */
  int    clip_empty;
  /* texture_kind: 0 = solid (default), otherwise an index into the     */
  /* svg_tex_names[] table.  Set by LoutSetTexture when its argument    */
  /* carries a recognised texture kind; consumed by svg_ps_emit_path   */
  /* during fills to substitute fill="url(#lout-tex-NAME)" for the     */
  /* plain colour.  Copied by gsave/restored by grestore via the       */
  /* struct-copy/pop pair, exactly like fill_rgb.                       */
  int    texture_kind;
  /* Active font state for the `show` operator.  Tracks the most-recent  */
  /* /Name findfont (font_name) and the most-recent scalefont scalar    */
  /* (font_size).  setfont activates a font dict but in our minimal    */
  /* model the dict is opaque, so the gstate just carries the name/size */
  /* directly.  Both fields are copied by gsave / restored by grestore. */
  char   font_name[64];
  double font_size;             /* in internal units (multiples of PT)  */
  /* Line-style attributes recorded by setlinecap / setlinejoin /        */
  /* setmiterlimit and emitted on the next stroke.  PS encodings:        */
  /*    setlinecap:  0=butt, 1=round, 2=square                            */
  /*    setlinejoin: 0=miter, 1=round, 2=bevel                            */
  /* line_cap / line_join < 0 means "unset" (omit attribute on stroke,   */
  /* falling back to the SVG default of butt / miter).  miter_limit < 0  */
  /* same convention.  Both fields are copied by gsave/restored by       */
  /* grestore via the struct-copy that GSAVE/GRESTORE already perform.   */
  int    line_cap;
  int    line_join;
  double miter_limit;
} svg_gstate;

/* Named textures recognised by the proc-body scanner inside              */
/* LoutMakeTexture.  Index 0 is the implicit "solid" (no pattern); the    */
/* remaining entries are emitted into a <defs> block once at the top of   */
/* every SVG page so any later fill that references "url(#lout-tex-XXX)"  */
/* resolves correctly.                                                    */
enum {
  SVG_TEX_SOLID = 0,
  SVG_TEX_STRIPED,
  SVG_TEX_GRID,
  SVG_TEX_DOTTED,
  SVG_TEX_CHESSBOARD,
  SVG_TEX_BRICKWORK,
  SVG_TEX_HONEYCOMB,
  SVG_TEX_TRIANGULAR,
  SVG_TEX_STRING,
  SVG_TEX_COUNT
};
static const char * const svg_tex_names[SVG_TEX_COUNT] = {
  "solid", "striped", "grid", "dotted", "chessboard",
  "brickwork", "honeycomb", "triangular", "string"
};


/***************************************************************************/
/*                                                                         */
/*  Typed PostScript value system.                                         */
/*                                                                         */
/*  The interpreter operates on tagged values pushed onto an operand       */
/*  stack.  Procedures and arrays are represented as malloc'd vectors of   */
/*  values; we track all allocations in a singly-linked arena so they can */
/*  be freed in one shot at SVG_PrintAfterLastPage.                        */
/*                                                                         */
/***************************************************************************/

enum {
  SVG_VK_NULL = 0,
  SVG_VK_NUM,
  SVG_VK_BOOL,
  SVG_VK_NAME,        /* executable name           */
  SVG_VK_LITNAME,     /* literal name (/foo)       */
  SVG_VK_STRING,      /* PostScript (...) string   */
  SVG_VK_PROC,        /* { ... } procedure         */
  SVG_VK_ARRAY,       /* [ ... ] array             */
  SVG_VK_MARK,        /* the '[' mark sentinel     */
  SVG_VK_DICT         /* dictionary handle         */
};

typedef struct svg_value {
  int    kind;
  double num;                   /* NUM / BOOL */
  char  *name;                  /* NAME / LITNAME / STRING (owned by arena) */
  struct svg_value *items;      /* PROC / ARRAY (owned by arena)            */
  int    nitems;
  int    dict_id;               /* DICT */
} svg_value;

typedef struct svg_alloc {
  void *p;
  int   kind;                   /* 0 = name buffer, 1 = items array */
  struct svg_alloc *next;
} svg_alloc;

typedef struct svg_dict_entry {
  char         *name;           /* arena-owned; NULL == empty slot         */
  svg_value     value;
  int           used;           /* 1 iff name != NULL (kept for clarity)   */
  unsigned int  hash;           /* cached FNV-1a hash of name              */
} svg_dict_entry;

typedef struct svg_dict {
  svg_dict_entry entries[SVG_PS_DICT_ENTRIES];
  int            in_use;
  int            count;         /* number of occupied slots                */
} svg_dict;


/***************************************************************************/
/*                                                                         */
/*  Module-static interpreter state -- persistent across @Graphic blocks   */
/*  for the lifetime of the document, mirroring the way the PostScript    */
/*  back end emits prologue procedures once and re-invokes them per page. */
/*                                                                         */
/***************************************************************************/

static svg_alloc *svg_arena_head = NULL;
static svg_dict   svg_dict_pool[SVG_PS_DICT_POOL];
static int        svg_dict_stack[SVG_PS_DICT_STACK_DEPTH];
static int        svg_dict_top;       /* index of current top dict in stack */
static int        svg_dict_pool_used;

static int        svg_warn_unknown_count;
#define SVG_WARN_MAX 8

static int        svg_recursion_depth;

/* sentinels for ifelse/loop control flow */
static int        svg_exit_flag;
static int        svg_stop_flag;


typedef struct svg_ps_state {
  svg_value  stack[SVG_PS_STACK_DEPTH];
  int        top;
  svg_gstate gs[SVG_PS_GS_DEPTH];
  int        gs_top;
  /* path accumulator */
  char   path[SVG_PATH_BUF_SIZE];
  int    plen;
  double cur_x, cur_y;
  /* last emitted path endpoint in device-pt (the same frame the SVG path     */
  /* coords are written in).  Tracked separately from cur_x/cur_y because the */
  /* latter live in user-space and become stale across CTM changes (e.g.     */
  /* ldiagsetarc does a translate+scale, calls arc, then setmatrix -- the    */
  /* arc's stored cur_x/cur_y belong to the translated frame and don't match */
  /* the next arc's start in the restored frame, even though in device space */
  /* the two endpoints coincide).  Used by svg_ps_arc to suppress redundant  */
  /* moveto commands that would split a single filled outline into multiple  */
  /* disconnected subpaths (the @Diag arrowhead bug).                         */
  double last_xp, last_yp;
  BOOLEAN last_pt_valid;
  BOOLEAN have_cp;
  BOOLEAN had_geom;             /* TRUE if a path command was added */
} svg_ps_state;

/* xsize/ysize/xmark/ymark/loutf/loutv/louts mirror the corresponding PS-   */
/* level variables; they persist across SVG_PrintGraphicObject calls just  */
/* like the PS interpreter's userdict bindings do.                         */
static double svg_var_xsize, svg_var_ysize;
static double svg_var_xmark, svg_var_ymark;
static double svg_var_loutf, svg_var_loutv, svg_var_louts;

/* Module-persistent PS interpreter state.  Carries gs[].m, baseline_m,    */
/* and the gsave/grestore stack across separate SVG_PrintGraphicObject     */
/* invocations so the @Diag prologue's PS-side translates (e.g. the       */
/* arrowhead-position translate that runs inside the parent @Diag's body  */
/* before a child arrowhead @Graphic body executes) survive the trip      */
/* through C-side layout recursion and reach the child invocation's gs   */
/* stack.  The dictionary stack already lives in svg_dict_pool, mirroring */
/* PostScript's userdict; this completes the persistence story for the    */
/* parts of PS execution state that the standard library relies on        */
/* spanning multiple back-end calls.                                      */
static svg_ps_state g_psstate;


/* Forward */
static void svg_ps_exec_value(svg_ps_state *s, const svg_value *v);
static void svg_ps_exec_proc(svg_ps_state *s, const svg_value *proc);
static void svg_ps_call(svg_ps_state *s, const svg_value *v);


/***************************************************************************/
/*  Arena helpers                                                          */
/***************************************************************************/

static void *svg_arena_alloc(size_t n, int kind)
{
  svg_alloc *a;
  void *p = malloc(n);
  if( p == NULL )
    return NULL;
  a = (svg_alloc *) malloc(sizeof *a);
  if( a == NULL ) { free(p); return NULL; }
  a->p = p;
  a->kind = kind;
  a->next = svg_arena_head;
  svg_arena_head = a;
  return p;
}

static char *svg_arena_strdup(const char *s, int len)
{
  char *out;
  if( s == NULL )
    return NULL;
  out = (char *) svg_arena_alloc((size_t) len + 1, 0);
  if( out == NULL )
    return NULL;
  memcpy(out, s, (size_t) len);
  out[len] = '\0';
  return out;
}

static void svg_arena_free_all(void)
{
  svg_alloc *a, *n;
  for( a = svg_arena_head; a != NULL; a = n )
  {
    n = a->next;
    if( a->p != NULL )
      free(a->p);
    free(a);
  }
  svg_arena_head = NULL;
}


/***************************************************************************/
/*  Dictionary helpers                                                     */
/***************************************************************************/

/* FNV-1a 32-bit hash over a NUL-terminated byte string.  Small, branchless,  */
/* well-distributed on short ASCII PostScript names; ANSI C clean.            */
static unsigned int svg_name_hash(const char *s)
{
  unsigned int h = 2166136261u;
  while( *s )
  {
    h ^= (unsigned char) *s++;
    h *= 16777619u;
  }
  return h;
}

static void svg_dict_clear(svg_dict *d)
{
  int i;
  d->in_use = 1;
  d->count = 0;
  for( i = 0; i < SVG_PS_DICT_ENTRIES; i++ )
  {
    d->entries[i].used = 0;
    d->entries[i].name = NULL;
    d->entries[i].hash = 0;
  }
}

static int svg_dict_alloc(void)
{
  int i;
  for( i = 0; i < SVG_PS_DICT_POOL; i++ )
  {
    if( !svg_dict_pool[i].in_use )
    {
      svg_dict_clear(&svg_dict_pool[i]);
      svg_dict_pool_used++;
      return i;
    }
  }
  return -1;
}

/* Forward decl: defined after svg_ps_state's full type is available. */
struct svg_ps_state;
static void svg_dict_try_free_anonymous(int did, struct svg_ps_state *s);

/* Open-addressed insert / update.  Linear probing on FNV-1a(name).  Empty   */
/* slot iff entries[slot].name == NULL.  No deletes ever happen at the      */
/* entry level (the only way to drop entries is svg_dict_clear, which       */
/* zeroes the whole table), so we do not need tombstones.                    */
static void svg_dict_def(int did, const char *name, const svg_value *v)
{
  unsigned int h, mask, slot;
  int probes;
  svg_dict *d;
  svg_dict_entry *e;
  if( did < 0 || did >= SVG_PS_DICT_POOL )
    return;
  d = &svg_dict_pool[did];
  h = svg_name_hash(name);
  mask = (unsigned int) SVG_PS_DICT_MASK;
  slot = h & mask;
  for( probes = 0; probes < SVG_PS_DICT_ENTRIES; probes++ )
  {
    e = &d->entries[slot];
    if( e->name == NULL )
    {
      /* empty -> insert */
      e->used = 1;
      e->hash = h;
      e->name = svg_arena_strdup(name, (int) strlen(name));
      e->value = *v;
      d->count++;
      return;
    }
    if( e->hash == h && strcmp(e->name, name) == 0 )
    {
      /* update existing */
      e->value = *v;
      return;
    }
    slot = (slot + 1) & mask;
  }
  /* table full: silently drop the define (matches previous behaviour: a    */
  /* completely full table also dropped the insert).                        */
}

static int svg_dict_lookup(int did, const char *name, svg_value *out)
{
  unsigned int h, mask, slot;
  int probes;
  svg_dict *d;
  svg_dict_entry *e;
  if( did < 0 || did >= SVG_PS_DICT_POOL )
    return 0;
  d = &svg_dict_pool[did];
  if( d->count == 0 )
    return 0;
  h = svg_name_hash(name);
  mask = (unsigned int) SVG_PS_DICT_MASK;
  slot = h & mask;
  for( probes = 0; probes < SVG_PS_DICT_ENTRIES; probes++ )
  {
    e = &d->entries[slot];
    if( e->name == NULL )
      return 0;                                /* empty: not found        */
    if( e->hash == h && strcmp(e->name, name) == 0 )
    {
      *out = e->value;
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

/* walk dict stack top -> bottom */
static int svg_dict_stack_lookup(const char *name, svg_value *out)
{
  int i;
  for( i = svg_dict_top; i >= 0; i-- )
  {
    if( svg_dict_lookup(svg_dict_stack[i], name, out) )
      return 1;
  }
  return 0;
}

static void svg_dict_stack_def(const char *name, const svg_value *v)
{
  if( svg_dict_top < 0 )
    return;
  svg_dict_def(svg_dict_stack[svg_dict_top], name, v);
}


/***************************************************************************/
/*  Stack helpers                                                          */
/***************************************************************************/

/* Recursive sweep: does value `v` reference dict `did` (directly, or via    */
/* any nested array/proc items)?  Used by svg_dict_try_free_anonymous to    */
/* detect dicts that are stashed inside captured procedure bodies (the      */
/* Lout prologue builds executable arrays containing `N dict` literals).    */
static int svg_value_refs_dict(const svg_value *v, int did)
{
  int i;
  if( v == NULL ) return 0;
  if( v->kind == SVG_VK_DICT && v->dict_id == did ) return 1;
  if( (v->kind == SVG_VK_ARRAY || v->kind == SVG_VK_PROC) && v->items != NULL )
  {
    for( i = 0; i < v->nitems; i++ )
      if( svg_value_refs_dict(&v->items[i], did) ) return 1;
  }
  return 0;
}

/* If dict `did` is anonymous (not on the dict stack, not referenced by any   */
/* other live dict slot, and not present on the operand stack), reclaim it    */
/* into the dict pool.  Called on `end` -- without this, the Lout prologue's */
/* heavy use of the `N dict begin ... end` idiom (one per @Diag node, link,  */
/* arrow-head, label) leaks a pool slot per node and exhausts the pool       */
/* after the first few dozen diagrams in a large document.                   */
static void svg_dict_try_free_anonymous(int did, struct svg_ps_state *s)
{
  int i, j;
  if( did <= 0 || did >= SVG_PS_DICT_POOL ) return;
  if( !svg_dict_pool[did].in_use ) return;
  /* Still on the dict stack? */
  for( i = 0; i <= svg_dict_top; i++ )
    if( svg_dict_stack[i] == did ) return;
  /* Referenced by some other live dict's slot (recursing into arrays/procs)? */
  for( i = 0; i < SVG_PS_DICT_POOL; i++ )
  {
    if( i == did || !svg_dict_pool[i].in_use ) continue;
    for( j = 0; j < SVG_PS_DICT_ENTRIES; j++ )
    {
      if( !svg_dict_pool[i].entries[j].used ) continue;
      if( svg_value_refs_dict(&svg_dict_pool[i].entries[j].value, did) )
        return;
    }
  }
  /* Referenced by a live value on the operand stack? */
  if( s != NULL )
  {
    for( i = 0; i < s->top; i++ )
      if( svg_value_refs_dict(&s->stack[i], did) ) return;
  }
  /* Safe to reclaim. */
  svg_dict_pool[did].in_use = 0;
  if( svg_dict_pool_used > 0 ) svg_dict_pool_used--;
}

/* Mark a dict and (transitively) any dicts it references via its slot       */
/* values.  Used by svg_dict_gc_sweep to compute reachability from the dict  */
/* stack and the operand stack.                                              */
static void svg_dict_mark(int did, char *marks)
{
  int j;
  svg_dict *d;
  if( did < 0 || did >= SVG_PS_DICT_POOL ) return;
  if( marks[did] ) return;
  if( !svg_dict_pool[did].in_use ) return;
  marks[did] = 1;
  d = &svg_dict_pool[did];
  for( j = 0; j < SVG_PS_DICT_ENTRIES; j++ )
  {
    int k;
    const svg_value *v;
    if( !d->entries[j].used ) continue;
    v = &d->entries[j].value;
    if( v->kind == SVG_VK_DICT )
      svg_dict_mark(v->dict_id, marks);
    /* nested array/proc may capture dicts */
    if( (v->kind == SVG_VK_ARRAY || v->kind == SVG_VK_PROC) && v->items != NULL )
    {
      for( k = 0; k < v->nitems; k++ )
        if( v->items[k].kind == SVG_VK_DICT )
          svg_dict_mark(v->items[k].dict_id, marks);
    }
  }
}

static void svg_value_mark(const svg_value *v, char *marks)
{
  int k;
  if( v == NULL ) return;
  if( v->kind == SVG_VK_DICT )
    svg_dict_mark(v->dict_id, marks);
  else if( (v->kind == SVG_VK_ARRAY || v->kind == SVG_VK_PROC) &&
           v->items != NULL )
  {
    for( k = 0; k < v->nitems; k++ )
      svg_value_mark(&v->items[k], marks);
  }
}

/* Reclaim dict pool slots that are unreachable from the dict stack and the   */
/* current operand stack.  Called at the end of each svg_ps_run invocation:  */
/* without this the @Diag/@SyntaxDiag prologue's heavy use of tag-dict       */
/* push/pop idioms (where the popped dict briefly sits on the operand stack  */
/* before being discarded) leaks one or more pool slots per node/link.  The */
/* persistent svg_dict_try_free_anonymous handles only the on-end case; this */
/* sweep catches dicts that became garbage during the run but weren't        */
/* freeable at the moment of `end`.                                           */
static void svg_dict_gc_sweep(struct svg_ps_state *s)
{
  static char marks[SVG_PS_DICT_POOL];
  int i;
  for( i = 0; i < SVG_PS_DICT_POOL; i++ )
    marks[i] = 0;
  /* Roots: all dicts on the dict stack */
  for( i = 0; i <= svg_dict_top; i++ )
    svg_dict_mark(svg_dict_stack[i], marks);
  /* Roots: anything reachable from the operand stack */
  if( s != NULL )
  {
    for( i = 0; i < s->top; i++ )
      svg_value_mark(&s->stack[i], marks);
  }
  /* Sweep: anything unmarked but in_use is garbage */
  for( i = 1; i < SVG_PS_DICT_POOL; i++ )
  {
    if( svg_dict_pool[i].in_use && !marks[i] )
    {
      svg_dict_pool[i].in_use = 0;
      if( svg_dict_pool_used > 0 ) svg_dict_pool_used--;
    }
  }
}

/*****************************************************************************/
/*                                                                           */
/*  Texture-proc inspector.                                                  */
/*                                                                           */
/*  LoutMakeTexture in PostScript receives a paint-procedure body whose     */
/*  contents distinguish the named textures defined in coltex.ld (striped,   */
/*  grid, dotted, chessboard, brickwork, honeycomb, triangular, string).     */
/*  The procedure is parsed into an svg_value of kind SVG_VK_PROC whose      */
/*  items[] holds the token stream; we walk it (recursing into nested        */
/*  procs/arrays) and tally distinctive operator names, then choose the      */
/*  texture kind from those tallies.  The result is one of SVG_TEX_*; an     */
/*  unrecognised proc falls back to SVG_TEX_SOLID and prints as flat fill.   */
/*                                                                           */
/*****************************************************************************/

typedef struct svg_tex_scan {
  int has_arc;          /* "arc" -> dotted                                   */
  int has_setdash;      /* "setdash" -> brickwork/honeycomb/triangular       */
  int has_stroke;       /* "stroke"                                          */
  int has_findfont;     /* "findfont"/"show" -> string                       */
  int has_show;
  int has_closepath;
  int has_fill;
  int n_rlineto;        /* count of rlineto: honeycomb has many, brickwork few */
  int n_lineto;
  int n_moveto;
} svg_tex_scan;

static void svg_tex_scan_walk(const svg_value *items, int n, svg_tex_scan *t)
{
  int i;
  if( items == NULL ) return;
  for( i = 0; i < n; i++ )
  {
    const svg_value *v = &items[i];
    if( (v->kind == SVG_VK_NAME || v->kind == SVG_VK_LITNAME) &&
        v->name != NULL )
    {
      const char *nm = v->name;
      if(      strcmp(nm, "arc") == 0 )            t->has_arc = 1;
      else if( strcmp(nm, "setdash") == 0 )        t->has_setdash = 1;
      else if( strcmp(nm, "stroke") == 0 )         t->has_stroke = 1;
      else if( strcmp(nm, "findfont") == 0 )       t->has_findfont = 1;
      else if( strcmp(nm, "show") == 0 )           t->has_show = 1;
      else if( strcmp(nm, "closepath") == 0 )      t->has_closepath = 1;
      else if( strcmp(nm, "fill") == 0 )           t->has_fill = 1;
      else if( strcmp(nm, "rlineto") == 0 )        t->n_rlineto++;
      else if( strcmp(nm, "lineto") == 0 )         t->n_lineto++;
      else if( strcmp(nm, "moveto") == 0 )         t->n_moveto++;
    }
    if( (v->kind == SVG_VK_PROC || v->kind == SVG_VK_ARRAY) &&
        v->items != NULL )
    {
      svg_tex_scan_walk(v->items, v->nitems, t);
    }
  }
}

static int svg_tex_identify(const svg_value *proc)
{
  svg_tex_scan t;
  if( proc == NULL || proc->kind != SVG_VK_PROC || proc->items == NULL )
    return SVG_TEX_SOLID;
  t.has_arc = t.has_setdash = t.has_stroke = 0;
  t.has_findfont = t.has_show = t.has_closepath = t.has_fill = 0;
  t.n_rlineto = t.n_lineto = t.n_moveto = 0;
  svg_tex_scan_walk(proc->items, proc->nitems, &t);

  /* Reference signatures (from coltex.ld @TextureCommand bodies):           */
  /*   striped     fill, closepath, 1 moveto, 1 lineto, 2 rlineto            */
  /*   grid        fill, closepath, 1 moveto, 1 lineto, 4 rlineto            */
  /*   dotted      arc, fill                                                 */
  /*   chessboard  fill, 2 closepath, 2 moveto, 0 lineto, 6 rlineto          */
  /*   brickwork   setdash, stroke, 0 closepath, 0 lineto, ~5 moveto, ~6 rlineto */
  /*   honeycomb   setdash, stroke, 1 closepath, 1 moveto, 0 lineto, 7 rlineto */
  /*   triangular  setdash, stroke, 1 closepath, 2 moveto, 2 lineto, 2 rlineto */
  /*   string      findfont + show                                           */
  /*                                                                         */
  /* Each branch below requires positive corroborating signals so that an     */
  /* unrecognised custom user paintproc (e.g. one that happens to use `arc` */
  /* but does not look like Lout's `dotted`) falls through to SVG_TEX_SOLID  */
  /* instead of being mis-mapped to a named pattern.                         */

  /* string: must have BOTH findfont and show; nothing else uses them.       */
  if( t.has_findfont && t.has_show )            return SVG_TEX_STRING;

  /* dotted: arc + fill, no setdash, no stroke, no closepath, no lineto.    */
  if( t.has_arc && t.has_fill && !t.has_setdash && !t.has_stroke &&
      !t.has_closepath && t.n_lineto == 0 )     return SVG_TEX_DOTTED;

  /* setdash+stroke family (brickwork/honeycomb/triangular).                */
  if( t.has_setdash && t.has_stroke && !t.has_fill && !t.has_arc )
  {
    /* triangular: 1 closepath, 2 lineto, 2 rlineto.                        */
    if( t.has_closepath && t.n_lineto >= 2 && t.n_rlineto >= 1 )
      return SVG_TEX_TRIANGULAR;
    /* honeycomb: 1 closepath, 0 lineto, >=6 rlineto.                       */
    if( t.has_closepath && t.n_lineto == 0 && t.n_rlineto >= 6 )
      return SVG_TEX_HONEYCOMB;
    /* brickwork: 0 closepath, 0 lineto, >=4 rlineto, >=3 moveto.           */
    if( !t.has_closepath && t.n_lineto == 0 && t.n_rlineto >= 4 &&
        t.n_moveto >= 3 )                       return SVG_TEX_BRICKWORK;
    /* setdash+stroke proc that doesn't match any of the named patterns ->  */
    /* unknown custom texture; fall through to SOLID below.                 */
  }
  /* fill family (striped/grid/chessboard): closepath + fill, no setdash.   */
  if( t.has_closepath && t.has_fill && !t.has_setdash && !t.has_stroke &&
      !t.has_arc )
  {
    /* chessboard: 2 moveto, 0 lineto, >=4 rlineto.                         */
    if( t.n_moveto >= 2 && t.n_lineto == 0 && t.n_rlineto >= 4 )
      return SVG_TEX_CHESSBOARD;
    /* grid: 1 moveto, 1 lineto, >=4 rlineto.                               */
    if( t.n_moveto == 1 && t.n_lineto >= 1 && t.n_rlineto >= 4 )
      return SVG_TEX_GRID;
    /* striped: 1 moveto, 1 lineto, 2 rlineto.                              */
    if( t.n_moveto == 1 && t.n_lineto >= 1 && t.n_rlineto >= 1 &&
        t.n_rlineto < 4 )                       return SVG_TEX_STRIPED;
  }
  /* Unknown / custom paintproc: emit solid colour fallback.  This is the   */
  /* sentinel branch -- svg_emit_pattern_defs has no entry for SOLID, and    */
  /* svg_ps_emit_path skips the texture fill when texture_kind==SVG_TEX_SOLID, */
  /* so the surface receives its currentColor fill with no pattern overlay.  */
  return SVG_TEX_SOLID;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_emit_pattern_defs - write a <defs> block containing every named     */
/*  texture pattern, ready to be referenced from later fills via             */
/*  fill="url(#lout-tex-NAME)".  Called once at the top of each <svg> page. */
/*                                                                           */
/*  The pattern bodies use small inline tile sizes (in pt, the same units   */
/*  the page <g> uses after its Y-flip) chosen to match the default Lout   */
/*  geometries.  All strokes/fills inside use currentColor, so the surface  */
/*  using fill="url(#...)" picks up its own colour automatically.          */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_pattern_defs(void)
{
  if( out_fp == NULL ) return;
  fputs("<defs>\n", out_fp);

  /* striped: 1pt-wide horizontal bars at 2pt pitch.                       */
  fputs(
    "<pattern id=\"lout-tex-striped\" patternUnits=\"userSpaceOnUse\" "
    "width=\"2\" height=\"2\">"
    "<rect x=\"0\" y=\"0\" width=\"2\" height=\"1\" fill=\"currentColor\"/>"
    "</pattern>\n", out_fp);

  /* grid: 1pt strokes forming a 2pt cell.                                */
  fputs(
    "<pattern id=\"lout-tex-grid\" patternUnits=\"userSpaceOnUse\" "
    "width=\"2\" height=\"2\">"
    "<path d=\"M 0 0 H 2 M 0 0 V 2\" stroke=\"currentColor\" "
    "stroke-width=\"1\" fill=\"none\"/></pattern>\n", out_fp);

  /* dotted: 0.5pt-radius dots on a 2pt grid, centred in the cell.        */
  fputs(
    "<pattern id=\"lout-tex-dotted\" patternUnits=\"userSpaceOnUse\" "
    "width=\"2\" height=\"2\">"
    "<circle cx=\"1\" cy=\"1\" r=\"0.5\" fill=\"currentColor\"/>"
    "</pattern>\n", out_fp);

  /* chessboard: 2pt squares in a 4pt tile.                              */
  fputs(
    "<pattern id=\"lout-tex-chessboard\" patternUnits=\"userSpaceOnUse\" "
    "width=\"4\" height=\"4\">"
    "<rect x=\"0\" y=\"0\" width=\"2\" height=\"2\" fill=\"currentColor\"/>"
    "<rect x=\"2\" y=\"2\" width=\"2\" height=\"2\" fill=\"currentColor\"/>"
    "</pattern>\n", out_fp);

  /* brickwork: 6x2 bricks stacked with a half-brick offset between rows. */
  fputs(
    "<pattern id=\"lout-tex-brickwork\" patternUnits=\"userSpaceOnUse\" "
    "width=\"6\" height=\"4\">"
    "<path d=\"M 0 0 H 6 M 0 2 H 6 M 0 4 H 6 "
    "M 0 0 V 2 M 6 0 V 2 M 3 2 V 4\" "
    "stroke=\"currentColor\" stroke-width=\"0.5\" fill=\"none\"/>"
    "</pattern>\n", out_fp);

  /* honeycomb: regular hexagonal cells, R=2pt.  Two stacked hexagons    */
  /* per tile so the tile (6 x ~7.088) wraps cleanly.                   */
  fputs(
    "<pattern id=\"lout-tex-honeycomb\" patternUnits=\"userSpaceOnUse\" "
    "width=\"6\" height=\"7.088\">"
    "<path d=\"M 1 0 h 2 l 1 1.772 l -1 1.772 h -2 l -1 -1.772 Z "
    "M 4 3.544 h 2 l 1 1.772 l -1 1.772 h -2 l -1 -1.772 Z\" "
    "stroke=\"currentColor\" stroke-width=\"0.5\" fill=\"none\"/>"
    "</pattern>\n", out_fp);

  /* triangular: equilateral triangles, R=4pt.                          */
  fputs(
    "<pattern id=\"lout-tex-triangular\" patternUnits=\"userSpaceOnUse\" "
    "width=\"4\" height=\"7.088\">"
    "<path d=\"M 0 0 L 4 0 L 0 7.088 L 4 7.088 "
    "M 0 3.544 L 4 3.544\" "
    "stroke=\"currentColor\" stroke-width=\"0.5\" fill=\"none\"/>"
    "</pattern>\n", out_fp);

  /* string: an asterisk glyph repeated on a 12pt grid.                  */
  fputs(
    "<pattern id=\"lout-tex-string\" patternUnits=\"userSpaceOnUse\" "
    "width=\"12\" height=\"12\">"
    "<text x=\"1\" y=\"10\" font-family=\"Times\" font-size=\"10\" "
    "fill=\"currentColor\">*</text></pattern>\n", out_fp);

  fputs("</defs>\n", out_fp);
}


static void svg_ps_init(svg_ps_state *s)
{
  s->top = 0;
  s->gs_top = 0;
  s->gs[0].stroke_rgb[0] = '\0';
  s->gs[0].fill_rgb[0] = '\0';
  s->gs[0].line_width = 0.0;
  s->gs[0].dasharray[0] = '\0';
  /* identity CTM and baseline */
  s->gs[0].m[0] = 1.0; s->gs[0].m[1] = 0.0;
  s->gs[0].m[2] = 0.0; s->gs[0].m[3] = 1.0;
  s->gs[0].m[4] = 0.0; s->gs[0].m[5] = 0.0;
  s->gs[0].baseline_m[0] = 1.0; s->gs[0].baseline_m[1] = 0.0;
  s->gs[0].baseline_m[2] = 0.0; s->gs[0].baseline_m[3] = 1.0;
  s->gs[0].baseline_m[4] = 0.0; s->gs[0].baseline_m[5] = 0.0;
  s->gs[0].clip_empty = 0;
  s->gs[0].texture_kind = SVG_TEX_SOLID;
  /* Default active font: Times-Roman 10pt (in internal units).  Any   */
  /* findfont/scalefont/setfont sequence in the prologue will override.*/
  strcpy(s->gs[0].font_name, "Times-Roman");
  s->gs[0].font_size = 10.0 * (double) PT;
  /* line-style: -1 means unset (omit on stroke, fall back to SVG default). */
  s->gs[0].line_cap    = -1;
  s->gs[0].line_join   = -1;
  s->gs[0].miter_limit = -1.0;
  /* svg_var_* persist across SVG_PrintGraphicObject calls (PS userdict     */
  /* semantics); they are initialised once by svg_psinterp_init.            */
  s->path[0] = '\0';
  s->plen = 0;
  s->cur_x = 0.0;
  s->cur_y = 0.0;
  s->last_xp = 0.0;
  s->last_yp = 0.0;
  s->last_pt_valid = FALSE;
  s->have_cp = FALSE;
  s->had_geom = FALSE;
}


static void svg_ps_push(svg_ps_state *s, const svg_value *v)
{
  if( s->top < SVG_PS_STACK_DEPTH )
  {
    s->stack[s->top++] = *v;
  }
}

static void svg_ps_push_num(svg_ps_state *s, double v)
{
  svg_value val;
  val.kind = SVG_VK_NUM;
  val.num = v;
  val.name = NULL;
  val.items = NULL;
  val.nitems = 0;
  val.dict_id = 0;
  svg_ps_push(s, &val);
}

static void svg_ps_push_bool(svg_ps_state *s, int b)
{
  svg_value val;
  val.kind = SVG_VK_BOOL;
  val.num = b ? 1.0 : 0.0;
  val.name = NULL;
  val.items = NULL;
  val.nitems = 0;
  val.dict_id = 0;
  svg_ps_push(s, &val);
}

static svg_value svg_ps_pop_value(svg_ps_state *s)
{
  svg_value none;
  none.kind = SVG_VK_NULL;
  none.num = 0.0;
  none.name = NULL;
  none.items = NULL;
  none.nitems = 0;
  none.dict_id = 0;
  if( s->top > 0 )
    return s->stack[--s->top];
  return none;
}

static double svg_ps_pop(svg_ps_state *s)
{
  svg_value v = svg_ps_pop_value(s);
  if( v.kind == SVG_VK_NUM || v.kind == SVG_VK_BOOL )
    return v.num;
  return 0.0;
}


static void svg_ps_path_append(svg_ps_state *s, const char *seg)
{
  int n = (int) strlen(seg);
  if( s->plen + n >= SVG_PATH_BUF_SIZE - 1 )
    return;
  memcpy(s->path + s->plen, seg, n);
  s->plen += n;
  s->path[s->plen] = '\0';
  s->had_geom = TRUE;
}


/*****************************************************************************/
/*                                                                           */
/*  Convert a Lout (bottom-left) length value already in PT units into the   */
/*  path coordinate space.  Because the surrounding <g> has already y-       */
/*  flipped, we just pass through; the original PS coordinate system is the */
/*  same as our flipped SVG frame inside the @Graphic clause.                */
/*                                                                           */
/*****************************************************************************/

static double svg_ps_to_pt(svg_ps_state *s, double v_internal)
{
  /* Operands in @Graphic clauses are already in Lout internal units (PT).  */
  /* The interpreter just divides by PT to get points.                      */
  (void) s;
  return v_internal / (double) PT;
}


/* Forward declarations for CTM helpers defined further below.  Path-draw   */
/* primitives call them to bake the path-delta into coordinates.            */
static double *svg_ps_ctm(svg_ps_state *s);
static double *svg_ps_baseline(svg_ps_state *s);
static void svg_ps_mat_apply(const double *m, double x, double y,
  double *xd, double *yd);
static int svg_ps_mat_apply_inverse(const double *m, double xd, double yd,
  double *x, double *y);

/* Apply the path-delta to a user-space point and return the result in pt.  */
/*                                                                          */
/* The point is in PS user-space.  The SVG <g> chain wrapping this <path>  */
/* element has cumulative effect baseline_m (the page-layout-emitted       */
/* SVG translates that exactly mirror what the PS interpreter saw before   */
/* this @Graphic body started).  We want the emitted path coordinate to    */
/* be such that the SVG renderer's application of baseline_m yields the    */
/* same device coordinate as the PS interpreter's current CTM applied to   */
/* (x,y).                                                                   */
/*                                                                          */
/*   baseline(path_coord) = CTM(x, y)                                       */
/*   path_coord = inv(baseline) o CTM (x, y)                                */
/*                                                                          */
/* For points inside the LoutGr2 frame (CTM == baseline), the path-delta   */
/* is identity and coordinates pass through unchanged.  For PS-internal    */
/* translate/scale/rotate (inside ldiagsetarc, ldiagdoarrow etc.) the      */
/* delta is non-trivial and gets baked into the path data.                  */
static void svg_ps_xform_pt(svg_ps_state *s, double x, double y,
  double *xp, double *yp)
{
  double xd, yd;
  double dx, dy;
  const double *m = svg_ps_ctm(s);
  const double *b = svg_ps_baseline(s);
  svg_ps_mat_apply(m, x, y, &xd, &yd);
  if( !svg_ps_mat_apply_inverse(b, xd, yd, &dx, &dy) )
    { dx = xd; dy = yd; }
  *xp = dx / (double) PT;
  *yp = dy / (double) PT;
}

static void svg_ps_moveto(svg_ps_state *s, double x, double y)
{
  char buf[64];
  double xp, yp;
  svg_ps_xform_pt(s, x, y, &xp, &yp);
  sprintf(buf, "M %.3f %.3f ", xp, yp);
  svg_ps_path_append(s, buf);
  s->cur_x = x;
  s->cur_y = y;
  s->last_xp = xp;
  s->last_yp = yp;
  s->last_pt_valid = TRUE;
  s->have_cp = TRUE;
}


static void svg_ps_lineto(svg_ps_state *s, double x, double y)
{
  char buf[64];
  double xp, yp;
  if( !s->have_cp )
  {
    svg_ps_moveto(s, x, y);
    return;
  }
  svg_ps_xform_pt(s, x, y, &xp, &yp);
  sprintf(buf, "L %.3f %.3f ", xp, yp);
  svg_ps_path_append(s, buf);
  s->cur_x = x;
  s->cur_y = y;
  s->last_xp = xp;
  s->last_yp = yp;
  s->last_pt_valid = TRUE;
}


static void svg_ps_curveto(svg_ps_state *s,
  double x1, double y1, double x2, double y2, double x3, double y3)
{
  char buf[256];
  double xp1, yp1, xp2, yp2, xp3, yp3;
  if( !s->have_cp )
    svg_ps_moveto(s, x1, y1);
  svg_ps_xform_pt(s, x1, y1, &xp1, &yp1);
  svg_ps_xform_pt(s, x2, y2, &xp2, &yp2);
  svg_ps_xform_pt(s, x3, y3, &xp3, &yp3);
  sprintf(buf, "C %.3f %.3f %.3f %.3f %.3f %.3f ",
    xp1, yp1, xp2, yp2, xp3, yp3);
  svg_ps_path_append(s, buf);
  s->cur_x = x3;
  s->cur_y = y3;
  s->last_xp = xp3;
  s->last_yp = yp3;
  s->last_pt_valid = TRUE;
  s->have_cp = TRUE;
}


static void svg_ps_closepath(svg_ps_state *s)
{
  if( s->have_cp )
  {
    svg_ps_path_append(s, "Z ");
    /* After Z the SVG current point returns to the last subpath start; any  */
    /* subsequent drawing command should start with a fresh moveto.  Clear   */
    /* last_pt_valid so the next arc/line forces an explicit M.              */
    s->last_pt_valid = FALSE;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  Glyph-outline callback shims.  svg_glyph_emit_outline (z53_glyph.c)      */
/*  walks one Type 1 charstring and feeds the absolute path coordinates back */
/*  through these.  The void* ctx is reinterpreted as an svg_ps_state*.      */
/*                                                                           */
/*****************************************************************************/

static void svg_charpath_cb_move(void *u, double x, double y)
{ svg_ps_moveto((svg_ps_state *) u, x, y); }

static void svg_charpath_cb_line(void *u, double x, double y)
{ svg_ps_lineto((svg_ps_state *) u, x, y); }

static void svg_charpath_cb_curve(void *u,
  double x1, double y1, double x2, double y2, double x3, double y3)
{ svg_ps_curveto((svg_ps_state *) u, x1, y1, x2, y2, x3, y3); }

static void svg_charpath_cb_close(void *u)
{ svg_ps_closepath((svg_ps_state *) u); }


/*****************************************************************************/
/*                                                                           */
/*  ASCII byte (StandardEncoding) -> Adobe glyph name.  Covers letters,     */
/*  digits, and the usual punctuation used by `charpath` consumers.  Other   */
/*  bytes return NULL and the caller falls back to a bbox rectangle for     */
/*  that one character.                                                     */
/*                                                                           */
/*****************************************************************************/

static const char *svg_ascii_glyph_name(unsigned int c)
{
  static const char *letters[26] = {
    "A","B","C","D","E","F","G","H","I","J","K","L","M",
    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"
  };
  static const char *small[26] = {
    "a","b","c","d","e","f","g","h","i","j","k","l","m",
    "n","o","p","q","r","s","t","u","v","w","x","y","z"
  };
  static const char *digits[10] = {
    "zero","one","two","three","four","five","six","seven","eight","nine"
  };
  if( c >= 'A' && c <= 'Z' ) return letters[c - 'A'];
  if( c >= 'a' && c <= 'z' ) return small[c - 'a'];
  if( c >= '0' && c <= '9' ) return digits[c - '0'];
  switch( c )
  {
    case ' ': return "space";
    case '!': return "exclam";
    case '"': return "quotedbl";
    case '#': return "numbersign";
    case '$': return "dollar";
    case '%': return "percent";
    case '&': return "ampersand";
    case '\'': return "quoteright";
    case '(': return "parenleft";
    case ')': return "parenright";
    case '*': return "asterisk";
    case '+': return "plus";
    case ',': return "comma";
    case '-': return "hyphen";
    case '.': return "period";
    case '/': return "slash";
    case ':': return "colon";
    case ';': return "semicolon";
    case '<': return "less";
    case '=': return "equal";
    case '>': return "greater";
    case '?': return "question";
    case '@': return "at";
    case '[': return "bracketleft";
    case '\\': return "backslash";
    case ']': return "bracketright";
    case '^': return "asciicircum";
    case '_': return "underscore";
    case '`': return "quoteleft";
    case '{': return "braceleft";
    case '|': return "bar";
    case '}': return "braceright";
    case '~': return "asciitilde";
    default:  return NULL;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_arc - PostScript "x y r a1 a2 arc" / "arcn".                      */
/*                                                                           */
/*  Approximates the arc by a sequence of SVG A (elliptical arc) segments.   */
/*  Splits sweeps > 90 degrees so the SVG arc remains a clean elliptical    */
/*  segment (technically SVG A handles up to ~360 but renderers occasionally */
/*  glitch with full sweeps).  ccw==1 -> arc (CCW), ccw==0 -> arcn (CW).    */
/*                                                                           */
/*****************************************************************************/

#ifndef SVG_PI
#define SVG_PI 3.14159265358979323846
#endif

static void svg_ps_arc(svg_ps_state *s, double cx, double cy, double r,
  double a1, double a2, int ccw)
{
  double sa, sweep, step;
  double x0, y0, xe, ye, rpt;
  char buf[256];
  int large_arc, sweep_flag, i, nsegs;
  double seg_sweep, ca, na;

  sa = a1 * SVG_PI / 180.0;
  x0 = cx + r * cos(sa);
  y0 = cy + r * sin(sa);

  /* PostScript spec says: if there is a current point, connect it to the    */
  /* arc start with a straight line.  In practice Lout's circle-drawing      */
  /* code stages a current point that's NOT at the arc's start, so the      */
  /* implicit lineto produces spurious tangents.  But for multi-arc          */
  /* outlines (arrowheads, curved boxes) the previous arc's endpoint IS the */
  /* next arc's start, and the path must stay continuous for fill to work.  */
  /* Compromise: moveto when no current point or when the gap is large;     */
  /* otherwise (current point already at arc start) emit nothing so the     */
  /* arc segment chains into the current subpath.                            */
  /*                                                                         */
  /* The comparison must be done in device-pt space, NOT user space:        */
  /* ldiagsetarc does `translate; scale; arc; ...; setmatrix`, so each      */
  /* invocation leaves cur_x/cur_y in its own translated user frame.  The   */
  /* next arc's x0/y0 is in a different frame.  Comparing user-space coords */
  /* gives a false mismatch and inserts a spurious M that breaks fill into  */
  /* disconnected subpaths.  Instead, transform both points through the    */
  /* current CTM (svg_ps_xform_pt) and compare the resulting device coords */
  /* against the last emitted path endpoint.                                 */
  if( !s->have_cp || !s->last_pt_valid )
    svg_ps_moveto(s, x0, y0);
  else
  {
    double x0p, y0p, dx, dy;
    svg_ps_xform_pt(s, x0, y0, &x0p, &y0p);
    dx = s->last_xp - x0p;
    dy = s->last_yp - y0p;
    if( dx*dx + dy*dy > 1e-3 )
      svg_ps_moveto(s, x0, y0);
  }

  /* Compute sweep in the requested direction. */
  if( ccw )
  {
    sweep = a2 - a1;
    while( sweep <  0.0   ) sweep += 360.0;
    while( sweep > 360.0  ) sweep -= 360.0;
  }
  else
  {
    sweep = a1 - a2;
    while( sweep <  0.0   ) sweep += 360.0;
    while( sweep > 360.0  ) sweep -= 360.0;
    sweep = -sweep;
  }

  /* Break the sweep into segments of at most 180 degrees so the elliptical */
  /* arc parameters are unambiguous in SVG.                                  */
  nsegs = (int) (fabs(sweep) / 180.0) + 1;
  if( nsegs < 1 ) nsegs = 1;
  if( nsegs > 8 ) nsegs = 8;
  step = sweep / (double) nsegs;
  rpt  = svg_ps_to_pt(s, r);
  ca   = a1;
  for( i = 0; i < nsegs; i++ )
  {
    double xpe, ype;
    na = ca + step;
    seg_sweep = na - ca;
    xe = cx + r * cos(na * SVG_PI / 180.0);
    ye = cy + r * sin(na * SVG_PI / 180.0);
    large_arc = (fabs(seg_sweep) > 180.0) ? 1 : 0;
    sweep_flag = (seg_sweep > 0.0) ? 1 : 0;
    svg_ps_xform_pt(s, xe, ye, &xpe, &ype);
    sprintf(buf, "A %.3f %.3f 0 %d %d %.3f %.3f ",
      rpt, rpt, large_arc, sweep_flag, xpe, ype);
    svg_ps_path_append(s, buf);
    s->last_xp = xpe;
    s->last_yp = ype;
    s->last_pt_valid = TRUE;
    ca = na;
  }
  s->cur_x = cx + r * cos((a1 + sweep) * SVG_PI / 180.0);
  s->cur_y = cy + r * sin((a1 + sweep) * SVG_PI / 180.0);
  s->have_cp = TRUE;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_emit_path - close out the current path with a <path d=".."/>     */
/*                                                                           */
/*****************************************************************************/

static void svg_ps_emit_path(svg_ps_state *s, int do_stroke, int do_fill)
{
  svg_gstate *g;
  if( !s->had_geom )
    return;
  g = &s->gs[s->gs_top];
  /* If the current gstate is masked by an empty clip (set by `clip` with  */
  /* no current path -- see the `clip` handler), the drawing would be     */
  /* invisible in PS.  Drop the path here too, but still reset path state */
  /* so the interpreter stays in sync.                                    */
  if( g->clip_empty )
  {
    s->path[0] = '\0';
    s->plen = 0;
    s->had_geom = FALSE;
    s->have_cp = FALSE;
    s->last_pt_valid = FALSE;
    return;
  }
  fputs("<path d=\"", out_fp);
  fputs(s->path, out_fp);
  fputs("\"", out_fp);
  if( do_fill )
  {
    const char *col;
    if( g->fill_rgb[0] != '\0' )
      col = g->fill_rgb;
    else if( g->stroke_rgb[0] != '\0' )
      col = g->stroke_rgb;
    else
      col = "currentColor";
    /* If a texture is in effect on the current gstate, use the matching     */
    /* <pattern> as the fill, and set the SVG inheritable `color` property  */
    /* so the pattern's currentColor strokes/fills pick up the active hue.  */
    if( g->texture_kind > SVG_TEX_SOLID && g->texture_kind < SVG_TEX_COUNT )
    {
      fprintf(out_fp, " fill=\"url(#lout-tex-%s)\" color=\"%s\"",
        svg_tex_names[g->texture_kind], col);
    }
    else
      fprintf(out_fp, " fill=\"%s\"", col);
  }
  else
    fputs(" fill=\"none\"", out_fp);
  if( do_stroke )
  {
    const char *col = g->stroke_rgb[0] != '\0' ? g->stroke_rgb : "currentColor";
    const char *cap_name = NULL;
    const char *join_name = NULL;
    fprintf(out_fp, " stroke=\"%s\"", col);
    if( g->line_width > 0.0 )
      fprintf(out_fp, " stroke-width=\"%.3f\"", g->line_width);
    if( g->dasharray[0] != '\0' )
      fprintf(out_fp, " stroke-dasharray=\"%s\"", g->dasharray);
    /* stroke-linecap: 0=butt (SVG default), 1=round, 2=square.  PS and SVG */
    /* share encodings.  Only emit when the active gstate value differs    */
    /* from the SVG default -- emitting "butt" / "miter" / "4" on every    */
    /* stroke not only bloats the output but also subtly perturbs rsvg's   */
    /* edge antialiasing on small shapes (e.g. the appendix colour-name    */
    /* swatch grid on User's Guide page 308).  Same logic for join (default */
    /* 0=miter) and miterlimit (default 4).                                 */
    if(      g->line_cap == 1 ) cap_name = "round";
    else if( g->line_cap == 2 ) cap_name = "square";
    if(      g->line_join == 1 ) join_name = "round";
    else if( g->line_join == 2 ) join_name = "bevel";
    if( cap_name != NULL )
      fprintf(out_fp, " stroke-linecap=\"%s\"", cap_name);
    if( join_name != NULL )
      fprintf(out_fp, " stroke-linejoin=\"%s\"", join_name);
    /* miter_limit is "unset" (< 0) by default; only emit when explicitly  */
    /* set by setmiterlimit AND not equal to the SVG default of 4.         */
    if( g->miter_limit > 0.0 && g->miter_limit != 4.0 )
      fprintf(out_fp, " stroke-miterlimit=\"%.3f\"", g->miter_limit);
  }
  fputs("/>\n", out_fp);
  /* reset path */
  s->path[0] = '\0';
  s->plen = 0;
  s->had_geom = FALSE;
  s->have_cp = FALSE;
  s->last_pt_valid = FALSE;
}


/*****************************************************************************/
/*                                                                           */
/*  CTM (current transformation matrix) helpers.                             */
/*                                                                           */
/*  Matrix layout matches PostScript: [a b c d tx ty] representing the      */
/*  affine map (x,y) -> (a*x + c*y + tx, b*x + d*y + ty).  All operators    */
/*  pre-concatenate (matrix' = arg * matrix), the same convention as PS.    */
/*                                                                           */
/*****************************************************************************/

static double *svg_ps_ctm(svg_ps_state *s)
{
  return s->gs[s->gs_top].m;
}

static double *svg_ps_baseline(svg_ps_state *s)
{
  return s->gs[s->gs_top].baseline_m;
}

/* out = a * b   (6-element affine matrices)                                 */
static void svg_ps_mat_mul(const double *a, const double *b, double *out)
{
  double r[6];
  r[0] = a[0]*b[0] + a[1]*b[2];
  r[1] = a[0]*b[1] + a[1]*b[3];
  r[2] = a[2]*b[0] + a[3]*b[2];
  r[3] = a[2]*b[1] + a[3]*b[3];
  r[4] = a[4]*b[0] + a[5]*b[2] + b[4];
  r[5] = a[4]*b[1] + a[5]*b[3] + b[5];
  out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
  out[3] = r[3]; out[4] = r[4]; out[5] = r[5];
}

static void svg_ps_ctm_translate(svg_ps_state *s, double tx, double ty)
{
  double t[6];
  double *m;
  t[0] = 1.0; t[1] = 0.0; t[2] = 0.0; t[3] = 1.0; t[4] = tx; t[5] = ty;
  m = svg_ps_ctm(s);
  svg_ps_mat_mul(t, m, m);
}

static void svg_ps_ctm_scale(svg_ps_state *s, double sx, double sy)
{
  double t[6];
  double *m;
  t[0] = sx; t[1] = 0.0; t[2] = 0.0; t[3] = sy; t[4] = 0.0; t[5] = 0.0;
  m  = svg_ps_ctm(s);
  svg_ps_mat_mul(t, m, m);
}

static void svg_ps_ctm_rotate(svg_ps_state *s, double angle_deg)
{
  double t[6], c, sn, rad;
  double *m;
  rad = angle_deg * SVG_PI / 180.0;
  c = cos(rad); sn = sin(rad);
  t[0] = c;   t[1] = sn;
  t[2] = -sn; t[3] = c;
  t[4] = 0.0; t[5] = 0.0;
  m = svg_ps_ctm(s);
  svg_ps_mat_mul(t, m, m);
}

/* Pre-concatenate an arbitrary 6-element matrix m_arg into the CTM:         */
/*   CTM' = m_arg * CTM                                                       */
static void svg_ps_ctm_concat(svg_ps_state *s, const double *m_arg)
{
  double *m;
  m = svg_ps_ctm(s);
  svg_ps_mat_mul(m_arg, m, m);
}


/* Apply matrix m to point (x,y) producing device-space (xd,yd).             */
static void svg_ps_mat_apply(const double *m, double x, double y,
  double *xd, double *yd)
{
  *xd = m[0]*x + m[2]*y + m[4];
  *yd = m[1]*x + m[3]*y + m[5];
}

/* Apply inverse of matrix m to device-space point (xd,yd) -> user (x,y).    */
/* Returns 1 on success, 0 if the matrix is (numerically) singular.          */
static int svg_ps_mat_apply_inverse(const double *m, double xd, double yd,
  double *x, double *y)
{
  double det = m[0]*m[3] - m[1]*m[2];
  double dx, dy;
  if( fabs(det) < 1e-12 )
    return 0;
  dx = xd - m[4];
  dy = yd - m[5];
  *x = ( dx * m[3] - dy * m[2]) / det;
  *y = (-dx * m[1] + dy * m[0]) / det;
  return 1;
}

/* Build a fresh 6-element matrix array (arena-allocated) initialised from   */
/* the supplied 6 doubles (or identity if vals == NULL).                    */
static svg_value svg_ps_new_matrix_array(const double *vals)
{
  svg_value out;
  svg_value *items;
  int i;
  static const double ident[6] = { 1.0, 0.0, 0.0, 1.0, 0.0, 0.0 };
  const double *src = (vals != NULL) ? vals : ident;
  items = (svg_value *) svg_arena_alloc(sizeof(svg_value) * 6, 1);
  if( items != NULL )
  {
    for( i = 0; i < 6; i++ )
    {
      items[i].kind = SVG_VK_NUM;
      items[i].num = src[i];
      items[i].name = NULL;
      items[i].items = NULL;
      items[i].nitems = 0;
      items[i].dict_id = 0;
    }
  }
  out.kind = SVG_VK_ARRAY;
  out.num = 0.0;
  out.name = NULL;
  out.items = items;
  out.nitems = items ? 6 : 0;
  out.dict_id = 0;
  return out;
}


/*****************************************************************************/
/*                                                                           */
/*  Helpers that mirror C-side gsave/grestore/translate/rotate/scale onto    */
/*  the module-persistent g_psstate, so PS-internal CTM state and the       */
/*  baseline_m anchor stay in sync with the SVG <g transform> chain that     */
/*  surrounds each emitted path.                                             */
/*                                                                           */
/*****************************************************************************/

static void svg_ps_module_gsave(void)
{
  if( g_psstate.gs_top + 1 < SVG_PS_GS_DEPTH )
  {
    g_psstate.gs[g_psstate.gs_top + 1] = g_psstate.gs[g_psstate.gs_top];
    g_psstate.gs_top++;
  }
}

static void svg_ps_module_grestore(void)
{
  if( g_psstate.gs_top > 0 )
    g_psstate.gs_top--;
}

/* Pre-concatenate `t` into both CTM and baseline_m of the current gs       */
/* level: m' = t * m, baseline' = t * baseline.  This keeps the path-delta */
/* (inv(baseline) * m) identity for points emitted by paths drawn AFTER    */
/* the SVG <g transform> chain absorbs `t`.                                */
static void svg_ps_module_concat_baseline(const double *t)
{
  svg_ps_mat_mul(t, g_psstate.gs[g_psstate.gs_top].m,
    g_psstate.gs[g_psstate.gs_top].m);
  svg_ps_mat_mul(t, g_psstate.gs[g_psstate.gs_top].baseline_m,
    g_psstate.gs[g_psstate.gs_top].baseline_m);
}

static void svg_ps_module_translate(double tx, double ty)
{
  double t[6];
  t[0] = 1.0; t[1] = 0.0; t[2] = 0.0; t[3] = 1.0; t[4] = tx; t[5] = ty;
  svg_ps_module_concat_baseline(t);
}

static void svg_ps_module_rotate(double angle_deg)
{
  double t[6], c, sn, rad;
  rad = angle_deg * SVG_PI / 180.0;
  c = cos(rad); sn = sin(rad);
  t[0] = c;   t[1] = sn;
  t[2] = -sn; t[3] = c;
  t[4] = 0.0; t[5] = 0.0;
  svg_ps_module_concat_baseline(t);
}

static void svg_ps_module_scale(double sx, double sy)
{
  double t[6];
  t[0] = sx;  t[1] = 0.0; t[2] = 0.0; t[3] = sy; t[4] = 0.0; t[5] = 0.0;
  svg_ps_module_concat_baseline(t);
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_resolve_value - some named values defined by LoutGraphic.         */
/*                                                                           */
/*  Returns 1 and sets *v if the name is recognised, else returns 0.         */
/*                                                                           */
/*****************************************************************************/

static int svg_ps_resolve_value(svg_ps_state *s, const char *tok, double *v)
{
  if( strcmp(tok, "xsize") == 0 )  { *v = svg_var_xsize; return 1; }
  if( strcmp(tok, "ysize") == 0 )  { *v = svg_var_ysize; return 1; }
  if( strcmp(tok, "xmark") == 0 )  { *v = svg_var_xmark; return 1; }
  if( strcmp(tok, "ymark") == 0 )  { *v = svg_var_ymark; return 1; }
  if( strcmp(tok, "loutf") == 0 )  { *v = svg_var_loutf; return 1; }
  if( strcmp(tok, "loutv") == 0 )  { *v = svg_var_loutv; return 1; }
  if( strcmp(tok, "louts") == 0 )  { *v = svg_var_louts; return 1; }
  return 0;
}


/*****************************************************************************/
/*                                                                           */
/*  Set the stroke/fill rgb on the current graphics state.                   */
/*                                                                           */
/*****************************************************************************/

static void svg_ps_set_rgb(svg_ps_state *s, double r, double g, double b)
{
  int rr, gg, bb;
  rr = (int)(r * 255.0 + 0.5);
  gg = (int)(g * 255.0 + 0.5);
  bb = (int)(b * 255.0 + 0.5);
  if( rr < 0 ) rr = 0;
  if( rr > 255 ) rr = 255;
  if( gg < 0 ) gg = 0;
  if( gg > 255 ) gg = 255;
  if( bb < 0 ) bb = 0;
  if( bb > 255 ) bb = 255;
  sprintf(s->gs[s->gs_top].stroke_rgb, "rgb(%d,%d,%d)", rr, gg, bb);
  sprintf(s->gs[s->gs_top].fill_rgb,   "rgb(%d,%d,%d)", rr, gg, bb);
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_is_number - parse a (possibly signed/decimal) PostScript number.  */
/*                                                                           */
/*****************************************************************************/

static int svg_ps_is_number(const char *tok, double *out)
{
  char *end;
  double v;
  if( tok == NULL || tok[0] == '\0' )
    return 0;
  v = strtod(tok, &end);
  if( end == tok || *end != '\0' )
    return 0;
  *out = v;
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  Tokenise a flat PS buffer into a vector of (start, length) descriptors.  */
/*                                                                           */
/*****************************************************************************/

typedef struct svg_ps_tok {
  int start;
  int len;
} svg_ps_tok;


static int svg_ps_tokenise(const char *buf, int n,
  svg_ps_tok *toks, int max_toks)
{
  int i, tcount, start, paren;
  char c;
  i = 0;
  tcount = 0;
  while( i < n && tcount < max_toks )
  {
    c = buf[i];
    if( c == ' ' || c == '\t' || c == '\n' || c == '\r' )
    {
      i++;
      continue;
    }
    if( c == '%' )
    {
      while( i < n && buf[i] != '\n' )
        i++;
      continue;
    }
    if( c == '(' )
    {
      start = i;
      paren = 1;
      i++;
      while( i < n && paren > 0 )
      {
        if( buf[i] == '\\' && i + 1 < n ) { i += 2; continue; }
        if( buf[i] == '(' ) paren++;
        else if( buf[i] == ')' ) paren--;
        i++;
      }
      toks[tcount].start = start;
      toks[tcount].len = i - start;
      tcount++;
      continue;
    }
    if( c == '[' || c == ']' || c == '{' || c == '}' )
    {
      toks[tcount].start = i;
      toks[tcount].len = 1;
      tcount++;
      i++;
      continue;
    }
    start = i;
    while( i < n )
    {
      c = buf[i];
      if( c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
          c == '[' || c == ']' || c == '{' || c == '}' ||
          c == '(' || c == '%' )
        break;
      i++;
    }
    toks[tcount].start = start;
    toks[tcount].len = i - start;
    tcount++;
  }
  return tcount;
}


/*****************************************************************************/
/*                                                                           */
/*  Build a typed-value vector from a sub-range of raw tokens.  Procedure    */
/*  bodies '{ ... }' become nested PROC values whose items[] is recursively  */
/*  parsed.  Arrays are NOT pre-grouped here -- '[' and ']' are emitted as   */
/*  MARK and a separator name that the runtime collects on the operand     */
/*  stack (PostScript semantics).                                            */
/*                                                                           */
/*  *cur is advanced past consumed tokens.  Stop_at_brace controls whether  */
/*  a '}' terminates parsing of the current value list.                     */
/*                                                                           */
/*****************************************************************************/

#define SVG_PS_MAX_TOKENS 65536

static svg_value *svg_parse_tokens(const char *buf, svg_ps_tok *toks,
  int ntoks, int *cur, int stop_at_brace, int *out_n)
{
  svg_value *out;
  int cap, n, i;
  double dv;
  char tbuf[256];

  cap = 16;
  n   = 0;
  out = (svg_value *) svg_arena_alloc(sizeof(svg_value) * (size_t) cap, 1);
  if( out == NULL ) { *out_n = 0; return NULL; }

  while( *cur < ntoks )
  {
    int tstart, tlen;
    char first;
    svg_value v;

    /* ensure capacity */
    if( n >= cap )
    {
      svg_value *grow;
      int newcap = cap * 2;
      grow = (svg_value *) svg_arena_alloc(sizeof(svg_value) * (size_t) newcap, 1);
      if( grow == NULL ) { *out_n = n; return out; }
      memcpy(grow, out, sizeof(svg_value) * (size_t) n);
      out = grow;
      cap = newcap;
    }

    tstart = toks[*cur].start;
    tlen   = toks[*cur].len;
    if( tlen <= 0 ) { (*cur)++; continue; }
    first = buf[tstart];

    if( stop_at_brace && tlen == 1 && first == '}' )
    {
      (*cur)++;
      *out_n = n;
      return out;
    }

    if( tlen == 1 && first == '{' )
    {
      int sub_n = 0;
      svg_value *sub;
      (*cur)++;
      sub = svg_parse_tokens(buf, toks, ntoks, cur, 1, &sub_n);
      v.kind = SVG_VK_PROC;
      v.num = 0.0;
      v.name = NULL;
      v.items = sub;
      v.nitems = sub_n;
      v.dict_id = 0;
      out[n++] = v;
      continue;
    }

    if( tlen == 1 && first == '[' )
    {
      v.kind = SVG_VK_MARK;
      v.num = 0.0;
      v.name = NULL;
      v.items = NULL;
      v.nitems = 0;
      v.dict_id = 0;
      out[n++] = v;
      (*cur)++;
      continue;
    }
    if( tlen == 1 && first == ']' )
    {
      /* emit as a named operator "]" so executor collects values back to    */
      /* the most recent MARK on the operand stack into an ARRAY value.      */
      v.kind = SVG_VK_NAME;
      v.num = 0.0;
      v.name = svg_arena_strdup("]", 1);
      v.items = NULL;
      v.nitems = 0;
      v.dict_id = 0;
      out[n++] = v;
      (*cur)++;
      continue;
    }

    if( first == '(' )
    {
      /* literal string */
      int slen;
      char *body;
      slen = tlen >= 2 ? tlen - 2 : 0;
      body = svg_arena_strdup(buf + tstart + 1, slen);
      v.kind = SVG_VK_STRING;
      v.num = 0.0;
      v.name = body;
      v.items = NULL;
      v.nitems = 0;
      v.dict_id = 0;
      out[n++] = v;
      (*cur)++;
      continue;
    }

    /* generic word */
    if( tlen >= (int)(sizeof tbuf) ) tlen = (int)(sizeof tbuf) - 1;
    for( i = 0; i < tlen; i++ ) tbuf[i] = buf[tstart + i];
    tbuf[tlen] = '\0';

    if( tbuf[0] == '/' )
    {
      v.kind = SVG_VK_LITNAME;
      v.num = 0.0;
      v.name = svg_arena_strdup(tbuf + 1, tlen - 1);
      v.items = NULL;
      v.nitems = 0;
      v.dict_id = 0;
      out[n++] = v;
      (*cur)++;
      continue;
    }

    if( svg_ps_is_number(tbuf, &dv) )
    {
      v.kind = SVG_VK_NUM;
      v.num = dv;
      v.name = NULL;
      v.items = NULL;
      v.nitems = 0;
      v.dict_id = 0;
      out[n++] = v;
      (*cur)++;
      continue;
    }

    /* an executable name */
    v.kind = SVG_VK_NAME;
    v.num = 0.0;
    v.name = svg_arena_strdup(tbuf, tlen);
    v.items = NULL;
    v.nitems = 0;
    v.dict_id = 0;
    out[n++] = v;
    (*cur)++;
  }

  *out_n = n;
  return out;
}


/*****************************************************************************/
/*                                                                           */
/*  Stack manipulation helpers used by operators.                           */
/*                                                                           */
/*****************************************************************************/

static void svg_collect_to_mark(svg_ps_state *s, svg_value *out)
{
  int i, mark, n;
  svg_value *items;
  mark = -1;
  for( i = s->top - 1; i >= 0; i-- )
  {
    if( s->stack[i].kind == SVG_VK_MARK ) { mark = i; break; }
  }
  if( mark < 0 )
  {
    /* no mark - synthesise empty array */
    out->kind = SVG_VK_ARRAY;
    out->num = 0.0;
    out->name = NULL;
    out->items = NULL;
    out->nitems = 0;
    out->dict_id = 0;
    return;
  }
  n = s->top - mark - 1;
  items = NULL;
  if( n > 0 )
  {
    items = (svg_value *) svg_arena_alloc(sizeof(svg_value) * (size_t) n, 1);
    if( items != NULL )
    {
      for( i = 0; i < n; i++ )
        items[i] = s->stack[mark + 1 + i];
    }
  }
  s->top = mark;
  out->kind = SVG_VK_ARRAY;
  out->num = 0.0;
  out->name = NULL;
  out->items = items;
  out->nitems = items != NULL ? n : 0;
  out->dict_id = 0;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_show - emit a <text> element rendering the byte-string `str` at   */
/*  the current point in the active font.  The string is byte-iterated;     */
/*  bytes >= 0x80 are passed through as Latin-1 -> UTF-8 (sufficient for     */
/*  the ASCII numeric tick labels produced by @Graph and the literal strings */
/*  found in @Diag/Fig labels).  Advances cur_x by an approximate width      */
/*  (0.5 em per char) so subsequent rmoveto sequences land in the right     */
/*  ballpark.                                                                */
/*                                                                           */
/*****************************************************************************/

static void svg_ps_show(svg_ps_state *s, const char *str)
{
  svg_gstate *g;
  const char *p;
  unsigned int c;
  double xp, yp, size_pt;
  double x0p, y0p, x1p, y1p, dx, dy, angle_deg;
  int is_bold, is_italic;
  const char *fname;
  const char *col;

  if( out_fp == NULL || str == NULL || str[0] == '\0' )
    return;
  g = &s->gs[s->gs_top];
  if( g->clip_empty )
    return;

  /* Convert the current point through the path-delta in the same way        */
  /* svg_ps_moveto does, so the text aligns with the surrounding path        */
  /* coordinate system (i.e. inside the <g> chain mirroring the outer CTM). */
  svg_ps_xform_pt(s, s->cur_x, s->cur_y, &xp, &yp);

  /* Recover any path-delta rotation by probing the local x-axis direction. */
  /* PS code like `translate N rotate moveto show` (e.g. ldiagshowtags in   */
  /* diagf.lpg, and any @Diag link-label that uses linklabelangle) rotates */
  /* the coordinate frame before show; without applying that rotation here */
  /* the text renders upright at the rotated origin rather than along the  */
  /* rotated x-axis.  The angle is atan2(dy, dx) over the image of (PT,0). */
  /* Inside the page-level Y-flip group, SVG rotate() agrees with PS       */
  /* rotate() in sign, so no negation is needed.                            */
  svg_ps_xform_pt(s, 0.0, 0.0, &x0p, &y0p);
  svg_ps_xform_pt(s, (double) PT, 0.0, &x1p, &y1p);
  dx = x1p - x0p;
  dy = y1p - y0p;
  angle_deg = (dx*dx + dy*dy > 1e-12) ? atan2(dy, dx) * 180.0 / SVG_PI : 0.0;

  /* font_size is in internal units (e.g. 8 pt -> 160).  The path-coord      */
  /* frame is points (PT == 20 internal units per point).                    */
  size_pt = g->font_size / (double) PT;
  if( size_pt <= 0.0 ) size_pt = 10.0;

  fname = g->font_name[0] != '\0' ? g->font_name : "Times-Roman";
  /* Heuristic Bold/Italic detection from the font name.                     */
  is_bold = (strstr(fname, "Bold") != NULL);
  is_italic = (strstr(fname, "Italic") != NULL ||
               strstr(fname, "Oblique") != NULL ||
               strstr(fname, "Slope")  != NULL);

  col = g->fill_rgb[0] != '\0' ? g->fill_rgb :
        (g->stroke_rgb[0] != '\0' ? g->stroke_rgb : NULL);

  /* Counter-flip wrapper so glyphs are upright inside the page-level Y-     */
  /* flip group, with an optional rotate() if the path-delta carries one.   */
  /* Rotate sits before scale(1,-1) so it's interpreted in the bottom-left  */
  /* frame (CCW positive, matching PS).                                      */
  if( fabs(angle_deg) > 0.01 )
    fprintf(out_fp,
      "<g transform=\"translate(%.3f,%.3f) rotate(%.3f) scale(1,-1)\">",
      xp, yp, angle_deg);
  else
    fprintf(out_fp,
      "<g transform=\"translate(%.3f,%.3f) scale(1,-1)\">", xp, yp);
  fprintf(out_fp,
    "<text x=\"0\" y=\"0\" font-family=\"%s\" font-size=\"%.3f\"",
    fname, size_pt);
  if( is_bold )
    fputs(" font-weight=\"bold\"", out_fp);
  if( is_italic )
    fputs(" font-style=\"italic\"", out_fp);
  if( col != NULL )
    fprintf(out_fp, " fill=\"%s\"", col);
  fputc('>', out_fp);
  for( p = str; *p != '\0'; p++ )
  {
    c = (unsigned int) (unsigned char) *p;
    svg_emit_utf8(c);
  }
  fputs("</text></g>\n", out_fp);

  /* Advance the current point by an approximate string width so subsequent  */
  /* `rmoveto` / show sequences in the prologue land in the right ballpark.  */
  /* 0.5 em per char is a coarse but adequate fixed-pitch approximation.     */
  s->cur_x += (double) strlen(str) * g->font_size * 0.5;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_exec_op - look up `name` as a built-in operator and execute it.   */
/*  Returns 1 if handled, 0 if not.                                          */
/*                                                                           */
/*  Dispatch is done via an open-address FNV-1a hash table built once on    */
/*  first call, mapping each operator name to a small integer op_id.  A      */
/*  single `switch (op_id)` then runs the case body.  Aliases that share    */
/*  the case body (e.g. fill/eofill, setrgbcolor/LoutSetRGBColor) map to    */
/*  the same op_id; aliases whose body discriminates on the original name   */
/*  (e.g. arc/arcn) get distinct ids.                                        */
/*                                                                           */
/*****************************************************************************/

typedef enum {
  SVG_OP_NONE = 0,
  /* drawing ops */
  SVG_OP_NEWPATH, SVG_OP_MOVETO, SVG_OP_LINETO, SVG_OP_RLINETO, SVG_OP_RMOVETO,
  SVG_OP_CURVETO, SVG_OP_RCURVETO, SVG_OP_CLOSEPATH,
  SVG_OP_ARC, SVG_OP_ARCN,
  SVG_OP_STROKE, SVG_OP_FILL,
  SVG_OP_SETRGBCOLOR, SVG_OP_SETGRAY, SVG_OP_SETHSBCOLOR, SVG_OP_SETCMYKCOLOR,
  SVG_OP_SETLINEWIDTH, SVG_OP_SETLINESTYLE, SVG_OP_SETDASH,
  SVG_OP_SETLINECAP, SVG_OP_SETLINEJOIN, SVG_OP_SETMITERLIMIT,
  SVG_OP_CURRENTLINEWIDTH, SVG_OP_LINEWIDTH,
  SVG_OP_GSAVE, SVG_OP_GRESTORE, SVG_OP_SAVE, SVG_OP_RESTORE,
  SVG_OP_TRANSLATE, SVG_OP_SCALE, SVG_OP_ROTATE, SVG_OP_CONCAT,
  SVG_OP_TRANSFORM, SVG_OP_DTRANSFORM,
  SVG_OP_ITRANSFORM, SVG_OP_IDTRANSFORM,
  SVG_OP_MATRIX, SVG_OP_IDENTMATRIX,
  SVG_OP_CURRENTMATRIX, SVG_OP_SETMATRIX,
  SVG_OP_CURRENTPOINT,
  SVG_OP_CLIP, SVG_OP_SHOWPAGE,
  SVG_OP_SHOW, SVG_OP_STRINGWIDTH, SVG_OP_CHARPATH,
  SVG_OP_FINDFONT, SVG_OP_SCALEFONT, SVG_OP_SETFONT, SVG_OP_CURRENTFONT,
  /* @Graph plot symbols (all share a single dispatch helper) */
  SVG_OP_SYM_FILLEDSQUARE,   SVG_OP_SYM_DOFILLEDSQUARE,
  SVG_OP_SYM_SQUARE,         SVG_OP_SYM_DOSQUARE,
  SVG_OP_SYM_FILLEDCIRCLE,   SVG_OP_SYM_DOFILLEDCIRCLE,
  SVG_OP_SYM_CIRCLE,         SVG_OP_SYM_DOCIRCLE,
  SVG_OP_SYM_FILLEDDIAMOND,  SVG_OP_SYM_DOFILLEDDIAMOND,
  SVG_OP_SYM_DIAMOND,        SVG_OP_SYM_DODIAMOND,
  SVG_OP_SYM_FILLEDTRIANGLE, SVG_OP_SYM_DOFILLEDTRIANGLE,
  SVG_OP_SYM_TRIANGLE,       SVG_OP_SYM_DOTRIANGLE,
  SVG_OP_SYM_CROSS,          SVG_OP_SYM_DOCROSS,
  SVG_OP_SYM_PLUS,           SVG_OP_SYM_DOPLUS,
  /* Lout named procedures */
  SVG_OP_LOUTGRAPHIC, SVG_OP_LOUTBOX, SVG_OP_LOUTRULE,
  SVG_OP_LOUTCURVEBOX, SVG_OP_LOUTSHADOWBOX, SVG_OP_LOUTGR2,
  SVG_OP_SAVE_CP,
  SVG_OP_LOUTTEXTURESOLID, SVG_OP_LOUTSETTEXTURE, SVG_OP_LOUTMAKETEXTURE,
  SVG_OP_LOUTPAGEDICT, SVG_OP_LOUTPAGESET,
  SVG_OP_LOUTMARGSET, SVG_OP_LOUTMARGSHIFT,
  /* dictionary ops */
  SVG_OP_DICT, SVG_OP_BEGIN, SVG_OP_END, SVG_OP_CURRENTDICT,
  SVG_OP_SYSDICT,
  SVG_OP_DEF, SVG_OP_LOAD, SVG_OP_WHERE, SVG_OP_KNOWN,
  SVG_OP_EXEC, SVG_OP_BIND, SVG_OP_CVX, SVG_OP_CVLIT,
  SVG_OP_TYPE, SVG_OP_XCHECK,
  /* boolean predicates */
  SVG_OP_TRUE, SVG_OP_FALSE, SVG_OP_NULL,
  SVG_OP_EQ, SVG_OP_NE,
  SVG_OP_LT, SVG_OP_GT, SVG_OP_LE, SVG_OP_GE,
  SVG_OP_AND, SVG_OP_OR, SVG_OP_XOR, SVG_OP_NOT,
  /* arithmetic */
  SVG_OP_ADD, SVG_OP_SUB, SVG_OP_MUL, SVG_OP_DIV, SVG_OP_IDIV, SVG_OP_MOD,
  SVG_OP_NEG, SVG_OP_ABS, SVG_OP_SQRT,
  SVG_OP_SIN, SVG_OP_COS, SVG_OP_ATAN, SVG_OP_EXP, SVG_OP_LN, SVG_OP_LOG,
  SVG_OP_TRUNCATE, SVG_OP_FLOOR, SVG_OP_CEILING, SVG_OP_ROUND,
  SVG_OP_CVI, SVG_OP_CVR, SVG_OP_CVS, SVG_OP_CVN, SVG_OP_STRING,
  /* stack manipulation */
  SVG_OP_POP, SVG_OP_DUP, SVG_OP_EXCH, SVG_OP_INDEX, SVG_OP_COPY, SVG_OP_ROLL,
  SVG_OP_CLEAR, SVG_OP_COUNT, SVG_OP_MARK, SVG_OP_CLEARTOMARK, SVG_OP_COUNTTOMARK,
  SVG_OP_RBRACKET,
  /* array/string ops */
  SVG_OP_ALOAD, SVG_OP_ASTORE, SVG_OP_LENGTH, SVG_OP_GET, SVG_OP_PUT,
  SVG_OP_PUTINTERVAL, SVG_OP_SEARCH,
  /* control flow */
  SVG_OP_IF, SVG_OP_IFELSE, SVG_OP_FOR, SVG_OP_REPEAT, SVG_OP_LOOP, SVG_OP_FORALL,
  SVG_OP_EXIT, SVG_OP_STOP, SVG_OP_STOPPED,
  /* Lout numeric prologue helpers */
  SVG_OP_IN, SVG_OP_CM, SVG_OP_PT, SVG_OP_EM, SVG_OP_SP, SVG_OP_VS, SVG_OP_FT,
  SVG_OP_DG,
  SVG_OP__COUNT
} svg_op_id;

typedef struct {
  const char  *name;     /* arena-owned (interned via static seed) */
  unsigned int hash;
  svg_op_id    op_id;
} svg_op_hash_entry;

#define SVG_OP_HASH_SIZE 256
#define SVG_OP_HASH_MASK (SVG_OP_HASH_SIZE - 1)

static svg_op_hash_entry svg_op_hash_table[SVG_OP_HASH_SIZE];
static int               svg_op_hash_built = 0;

/* (name, op_id) seed table.  Order does not matter for correctness; the      */
/* hash distributes entries.  Keep aliases that share a case body grouped    */
/* together for readability.                                                  */
static const struct { const char *name; svg_op_id id; } svg_op_seed[] = {
  /* drawing ops */
  {"newpath", SVG_OP_NEWPATH}, {"moveto", SVG_OP_MOVETO},
  {"lineto", SVG_OP_LINETO}, {"rlineto", SVG_OP_RLINETO},
  {"rmoveto", SVG_OP_RMOVETO}, {"curveto", SVG_OP_CURVETO},
  {"rcurveto", SVG_OP_RCURVETO}, {"closepath", SVG_OP_CLOSEPATH},
  {"arc", SVG_OP_ARC}, {"arcn", SVG_OP_ARCN},
  {"stroke", SVG_OP_STROKE}, {"fill", SVG_OP_FILL}, {"eofill", SVG_OP_FILL},
  {"setrgbcolor", SVG_OP_SETRGBCOLOR}, {"LoutSetRGBColor", SVG_OP_SETRGBCOLOR},
  {"setgray", SVG_OP_SETGRAY}, {"LoutSetGray", SVG_OP_SETGRAY},
  {"sethsbcolor", SVG_OP_SETHSBCOLOR}, {"LoutSetHSBColor", SVG_OP_SETHSBCOLOR},
  {"setcmykcolor", SVG_OP_SETCMYKCOLOR}, {"LoutSetCMYKColor", SVG_OP_SETCMYKCOLOR},
  {"setlinewidth", SVG_OP_SETLINEWIDTH},
  {"setlinecap", SVG_OP_SETLINECAP}, {"setlinejoin", SVG_OP_SETLINEJOIN},
  {"setmiterlimit", SVG_OP_SETMITERLIMIT},
  {"currentlinewidth", SVG_OP_CURRENTLINEWIDTH},
  /* `linewidth` is a Lout @Graph prologue option (lout/include/graph) that  */
  /* in PostScript mode defaults to `{ currentlinewidth }` -- the user      */
  /* expects the active stroke width.  In SVG mode the option-binding       */
  /* machinery never runs, so the name arrives at the interpreter as an     */
  /* undefined symbol.  Treat it as a synonym for currentlinewidth so the    */
  /* @Graph mark-drawing procs (e.g. graphf.lpg's `linewidth setlinewidth   */
  /* stroke` cadence) keep their stroke width across the round-trip.        */
  {"linewidth", SVG_OP_LINEWIDTH},
  {"setdash", SVG_OP_SETDASH},
  /* save / restore are PostScript VM snapshots whose graphics-state effect */
  /* coincides with gsave/grestore for the small subset of state z53.c     */
  /* tracks.  Lout's @Fig / @Diag prologues use them to bracket independent */
  /* graphic-object emissions; alias for that purpose here.                 */
  {"gsave", SVG_OP_GSAVE}, {"grestore", SVG_OP_GRESTORE},
  {"save", SVG_OP_SAVE}, {"restore", SVG_OP_RESTORE},
  {"translate", SVG_OP_TRANSLATE}, {"scale", SVG_OP_SCALE},
  {"rotate", SVG_OP_ROTATE}, {"concat", SVG_OP_CONCAT},
  {"transform", SVG_OP_TRANSFORM}, {"dtransform", SVG_OP_DTRANSFORM},
  {"itransform", SVG_OP_ITRANSFORM}, {"idtransform", SVG_OP_IDTRANSFORM},
  {"matrix", SVG_OP_MATRIX}, {"identmatrix", SVG_OP_IDENTMATRIX},
  {"currentmatrix", SVG_OP_CURRENTMATRIX}, {"defaultmatrix", SVG_OP_CURRENTMATRIX},
  {"setmatrix", SVG_OP_SETMATRIX},
  {"currentpoint", SVG_OP_CURRENTPOINT},
  {"clip", SVG_OP_CLIP}, {"showpage", SVG_OP_SHOWPAGE},
  {"show", SVG_OP_SHOW}, {"stringwidth", SVG_OP_STRINGWIDTH},
  {"charpath", SVG_OP_CHARPATH},
  {"findfont", SVG_OP_FINDFONT}, {"scalefont", SVG_OP_SCALEFONT},
  {"setfont", SVG_OP_SETFONT}, {"currentfont", SVG_OP_CURRENTFONT},
  /* @Graph plot symbols */
  {"filledsquare",     SVG_OP_SYM_FILLEDSQUARE},
  {"dofilledsquare",   SVG_OP_SYM_DOFILLEDSQUARE},
  {"square",           SVG_OP_SYM_SQUARE},
  {"dosquare",         SVG_OP_SYM_DOSQUARE},
  {"filledcircle",     SVG_OP_SYM_FILLEDCIRCLE},
  {"dofilledcircle",   SVG_OP_SYM_DOFILLEDCIRCLE},
  {"circle",           SVG_OP_SYM_CIRCLE},
  {"docircle",         SVG_OP_SYM_DOCIRCLE},
  {"filleddiamond",    SVG_OP_SYM_FILLEDDIAMOND},
  {"dofilleddiamond",  SVG_OP_SYM_DOFILLEDDIAMOND},
  {"diamond",          SVG_OP_SYM_DIAMOND},
  {"dodiamond",        SVG_OP_SYM_DODIAMOND},
  {"filledtriangle",   SVG_OP_SYM_FILLEDTRIANGLE},
  {"dofilledtriangle", SVG_OP_SYM_DOFILLEDTRIANGLE},
  {"triangle",         SVG_OP_SYM_TRIANGLE},
  {"dotriangle",       SVG_OP_SYM_DOTRIANGLE},
  {"cross",            SVG_OP_SYM_CROSS},
  {"docross",          SVG_OP_SYM_DOCROSS},
  {"plus",             SVG_OP_SYM_PLUS},
  {"doplus",           SVG_OP_SYM_DOPLUS},
  /* Lout named procedures */
  {"LoutGraphic", SVG_OP_LOUTGRAPHIC}, {"LoutBox", SVG_OP_LOUTBOX},
  {"LoutRule", SVG_OP_LOUTRULE}, {"LoutCurveBox", SVG_OP_LOUTCURVEBOX},
  {"LoutShadowBox", SVG_OP_LOUTSHADOWBOX}, {"LoutGr2", SVG_OP_LOUTGR2},
  {"save_cp", SVG_OP_SAVE_CP}, {"restore_cp", SVG_OP_SAVE_CP},
  {"LoutTextureSolid", SVG_OP_LOUTTEXTURESOLID},
  {"LoutSetTexture", SVG_OP_LOUTSETTEXTURE},
  {"LoutMakeTexture", SVG_OP_LOUTMAKETEXTURE},
  /* Page / margin chrome (bsf.lpg, dsf).  bsf.lpg's `LoutPageSet` /            */
  /* `LoutMargSet` / `LoutMargShift` definitions are run when the prepend      */
  /* file is ingested, but the resulting userdict entries live in a            */
  /* throw-away interpreter state (see svg_ingest_prepend_files, ~line 5611)  */
  /* and so are absent from the persistent state that handles per-page         */
  /* @Place / @MargPut graphic bodies.  Without explicit ops here the          */
  /* names fall into the "unknown PostScript operator" path and the trailing  */
  /* `begin` / `setmatrix` / pop sequence corrupts the stack.  We define       */
  /* C-side stand-ins that keep the stack balanced and (best-effort)          */
  /* preserve the surrounding gsave/translate/grestore so @Place'd boxes      */
  /* render at roughly the right position.  See SVG_INCLUDES_AUDIT.md.        */
  {"LoutPageDict",  SVG_OP_LOUTPAGEDICT},
  {"LoutPageSet",   SVG_OP_LOUTPAGESET},
  {"LoutMargSet",   SVG_OP_LOUTMARGSET},
  {"LoutMargShift", SVG_OP_LOUTMARGSHIFT},
  /* `matr` is the matrix that LoutMargSet / LoutPageSet capture via          */
  /* `matrix currentmatrix def`.  It is referenced inside @Place / @MargPut   */
  /* bodies as `matr setmatrix`.  Since our LoutPageDict / LoutMargSet         */
  /* stand-ins do not populate that slot, alias `matr` to `matrix`: this      */
  /* pushes a fresh identity 6-element array, so the subsequent setmatrix    */
  /* consumes a well-formed operand and the CTM ends up as identity          */
  /* (effectively a no-op for the @Place coordinate-frame reset).            */
  {"matr",          SVG_OP_MATRIX},
  /* dictionary ops */
  {"dict", SVG_OP_DICT}, {"begin", SVG_OP_BEGIN}, {"end", SVG_OP_END},
  {"currentdict", SVG_OP_CURRENTDICT},
  {"userdict", SVG_OP_SYSDICT}, {"systemdict", SVG_OP_SYSDICT},
  {"globaldict", SVG_OP_SYSDICT}, {"errordict", SVG_OP_SYSDICT},
  {"statusdict", SVG_OP_SYSDICT}, {"$error", SVG_OP_SYSDICT},
  {"def", SVG_OP_DEF}, {"load", SVG_OP_LOAD}, {"where", SVG_OP_WHERE},
  {"known", SVG_OP_KNOWN}, {"exec", SVG_OP_EXEC}, {"bind", SVG_OP_BIND},
  {"cvx", SVG_OP_CVX}, {"cvlit", SVG_OP_CVLIT},
  {"type", SVG_OP_TYPE}, {"xcheck", SVG_OP_XCHECK},
  /* boolean predicates */
  {"true", SVG_OP_TRUE}, {"false", SVG_OP_FALSE}, {"null", SVG_OP_NULL},
  {"eq", SVG_OP_EQ}, {"ne", SVG_OP_NE},
  {"lt", SVG_OP_LT}, {"gt", SVG_OP_GT}, {"le", SVG_OP_LE}, {"ge", SVG_OP_GE},
  {"and", SVG_OP_AND}, {"or", SVG_OP_OR}, {"xor", SVG_OP_XOR},
  {"not", SVG_OP_NOT},
  /* arithmetic */
  {"add", SVG_OP_ADD}, {"sub", SVG_OP_SUB}, {"mul", SVG_OP_MUL},
  {"div", SVG_OP_DIV}, {"idiv", SVG_OP_IDIV}, {"mod", SVG_OP_MOD},
  {"neg", SVG_OP_NEG}, {"abs", SVG_OP_ABS}, {"sqrt", SVG_OP_SQRT},
  {"sin", SVG_OP_SIN}, {"cos", SVG_OP_COS}, {"atan", SVG_OP_ATAN},
  {"exp", SVG_OP_EXP}, {"ln", SVG_OP_LN}, {"log", SVG_OP_LOG},
  {"truncate", SVG_OP_TRUNCATE}, {"floor", SVG_OP_FLOOR},
  {"ceiling", SVG_OP_CEILING}, {"round", SVG_OP_ROUND},
  {"cvi", SVG_OP_CVI}, {"cvr", SVG_OP_CVR},
  {"cvs", SVG_OP_CVS}, {"cvn", SVG_OP_CVN}, {"string", SVG_OP_STRING},
  /* stack manipulation */
  {"pop", SVG_OP_POP}, {"dup", SVG_OP_DUP}, {"exch", SVG_OP_EXCH},
  {"index", SVG_OP_INDEX}, {"copy", SVG_OP_COPY}, {"roll", SVG_OP_ROLL},
  {"clear", SVG_OP_CLEAR}, {"count", SVG_OP_COUNT},
  {"mark", SVG_OP_MARK}, {"cleartomark", SVG_OP_CLEARTOMARK},
  {"counttomark", SVG_OP_COUNTTOMARK},
  {"]", SVG_OP_RBRACKET},
  /* array/string ops */
  {"aload", SVG_OP_ALOAD}, {"astore", SVG_OP_ASTORE},
  {"length", SVG_OP_LENGTH}, {"get", SVG_OP_GET}, {"put", SVG_OP_PUT},
  {"putinterval", SVG_OP_PUTINTERVAL}, {"search", SVG_OP_SEARCH},
  /* control flow */
  {"if", SVG_OP_IF}, {"ifelse", SVG_OP_IFELSE},
  {"for", SVG_OP_FOR}, {"repeat", SVG_OP_REPEAT}, {"loop", SVG_OP_LOOP},
  {"forall", SVG_OP_FORALL},
  {"exit", SVG_OP_EXIT}, {"stop", SVG_OP_STOP}, {"stopped", SVG_OP_STOPPED},
  /* Lout numeric helpers */
  {"in", SVG_OP_IN}, {"cm", SVG_OP_CM}, {"pt", SVG_OP_PT}, {"em", SVG_OP_EM},
  {"sp", SVG_OP_SP}, {"vs", SVG_OP_VS}, {"ft", SVG_OP_FT}, {"dg", SVG_OP_DG},
  {NULL, SVG_OP_NONE}
};

/* Populate svg_op_hash_table from svg_op_seed.  Uses the same FNV-1a +     */
/* open-addressed linear probing as svg_dict_lookup.  Called lazily on the */
/* first svg_ps_exec_op invocation.                                        */
static void svg_op_hash_build(void)
{
  int i;
  unsigned int h, slot;
  for( i = 0; i < SVG_OP_HASH_SIZE; i++ )
  {
    svg_op_hash_table[i].name  = NULL;
    svg_op_hash_table[i].hash  = 0;
    svg_op_hash_table[i].op_id = SVG_OP_NONE;
  }
  for( i = 0; svg_op_seed[i].name != NULL; i++ )
  {
    h = svg_name_hash(svg_op_seed[i].name);
    slot = h & (unsigned int) SVG_OP_HASH_MASK;
    while( svg_op_hash_table[slot].name != NULL )
      slot = (slot + 1) & (unsigned int) SVG_OP_HASH_MASK;
    svg_op_hash_table[slot].name  = svg_op_seed[i].name;
    svg_op_hash_table[slot].hash  = h;
    svg_op_hash_table[slot].op_id = svg_op_seed[i].id;
  }
  svg_op_hash_built = 1;
}

/* Look up `name` in the op-hash; return SVG_OP_NONE if not present.  Hot   */
/* path: one FNV-1a pass + (usually 1) probe + one strcmp.                  */
static svg_op_id svg_op_lookup(const char *name)
{
  unsigned int h, slot;
  svg_op_hash_entry *e;
  if( !svg_op_hash_built ) svg_op_hash_build();
  h = svg_name_hash(name);
  slot = h & (unsigned int) SVG_OP_HASH_MASK;
  while( (e = &svg_op_hash_table[slot])->name != NULL )
  {
    if( e->hash == h && strcmp(e->name, name) == 0 )
      return e->op_id;
    slot = (slot + 1) & (unsigned int) SVG_OP_HASH_MASK;
  }
  return SVG_OP_NONE;
}

/* Forward declaration of the @Graph plot-symbol helper, defined below.     */
static int svg_ps_exec_symbol(svg_ps_state *s, const char *name,
  svg_op_id op_id);

static int svg_ps_exec_op(svg_ps_state *s, const char *name)
{
  double a, b, c, d, e, f;
  svg_value va, vb;
  svg_op_id op_id = svg_op_lookup(name);
  if( op_id == SVG_OP_NONE ) return 0;
  switch( op_id )
  {

  /* drawing ops */
  case SVG_OP_NEWPATH:
    s->path[0] = '\0';
    s->plen = 0;
    s->had_geom = FALSE;
    s->have_cp = FALSE;
    s->last_pt_valid = FALSE;
    return 1;
  case SVG_OP_MOVETO:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_moveto(s, a, b);
    return 1;
  case SVG_OP_LINETO:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_lineto(s, a, b);
    return 1;
  case SVG_OP_RLINETO:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_lineto(s, s->cur_x + a, s->cur_y + b);
    return 1;
  case SVG_OP_RMOVETO:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_moveto(s, s->cur_x + a, s->cur_y + b);
    return 1;
  case SVG_OP_CURVETO:
    f = svg_ps_pop(s); e = svg_ps_pop(s);
    d = svg_ps_pop(s); c = svg_ps_pop(s);
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_curveto(s, a, b, c, d, e, f);
    return 1;
  case SVG_OP_RCURVETO:
  {
    double cx, cy;
    f = svg_ps_pop(s); e = svg_ps_pop(s);
    d = svg_ps_pop(s); c = svg_ps_pop(s);
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    cx = s->cur_x; cy = s->cur_y;
    svg_ps_curveto(s, cx + a, cy + b, cx + c, cy + d, cx + e, cy + f);
    return 1;
  }
  case SVG_OP_CLOSEPATH:
    svg_ps_closepath(s);
    return 1;
  case SVG_OP_ARC:
  case SVG_OP_ARCN:
  {
    double cx, cy, r, a1, a2;
    a2 = svg_ps_pop(s); a1 = svg_ps_pop(s);
    r  = svg_ps_pop(s);
    cy = svg_ps_pop(s); cx = svg_ps_pop(s);
    svg_ps_arc(s, cx, cy, r, a1, a2, op_id == SVG_OP_ARC ? 1 : 0);
    return 1;
  }
  case SVG_OP_STROKE:
    svg_ps_emit_path(s, 1, 0);
    return 1;
  case SVG_OP_FILL:
    svg_ps_emit_path(s, 0, 1);
    return 1;
  case SVG_OP_SETRGBCOLOR:
    c = svg_ps_pop(s); b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_set_rgb(s, a, b, c);
    return 1;
  case SVG_OP_SETGRAY:
    a = svg_ps_pop(s);
    svg_ps_set_rgb(s, a, a, a);
    return 1;
  case SVG_OP_SETHSBCOLOR:
    (void) svg_ps_pop(s); (void) svg_ps_pop(s); (void) svg_ps_pop(s);
    return 1;
  case SVG_OP_SETCMYKCOLOR:
  {
    double cc, mm, yy, kk;
    kk = svg_ps_pop(s); yy = svg_ps_pop(s);
    mm = svg_ps_pop(s); cc = svg_ps_pop(s);
    svg_ps_set_rgb(s,
      (1.0 - cc) * (1.0 - kk),
      (1.0 - mm) * (1.0 - kk),
      (1.0 - yy) * (1.0 - kk));
    return 1;
  }
  case SVG_OP_SETLINEWIDTH:
    a = svg_ps_pop(s);
    s->gs[s->gs_top].line_width = a / (double) PT;
    return 1;
  case SVG_OP_SETLINECAP:
  {
    /* PS encoding: 0=butt 1=round 2=square.  Stored verbatim; emitted as  */
    /* SVG stroke-linecap on the next stroke (svg_ps_emit_path).            */
    int ic;
    a = svg_ps_pop(s);
    ic = (int) a;
    if( ic < 0 ) ic = 0;
    if( ic > 2 ) ic = 2;
    s->gs[s->gs_top].line_cap = ic;
    return 1;
  }
  case SVG_OP_SETLINEJOIN:
  {
    /* PS encoding: 0=miter 1=round 2=bevel.  Stored verbatim; emitted as  */
    /* SVG stroke-linejoin on the next stroke.                              */
    int ij;
    a = svg_ps_pop(s);
    ij = (int) a;
    if( ij < 0 ) ij = 0;
    if( ij > 2 ) ij = 2;
    s->gs[s->gs_top].line_join = ij;
    return 1;
  }
  case SVG_OP_SETMITERLIMIT:
    a = svg_ps_pop(s);
    if( a <= 0.0 ) a = -1.0;   /* clamp to "unset" so we omit the attr */
    s->gs[s->gs_top].miter_limit = a;
    return 1;
  /* `setlinestyle` no longer occurs in the seed table; the case remains   */
  /* for backwards-compat if any external @Graphic body still ships it as a */
  /* single-arg combined setter -- pop and discard.                         */
  case SVG_OP_SETLINESTYLE:
    (void) svg_ps_pop(s);
    return 1;
  case SVG_OP_CURRENTLINEWIDTH:
  case SVG_OP_LINEWIDTH:
  {
    /* Push the current stroke width (in internal Lout units) back on the  */
    /* operand stack.  line_width is stored in PT (see SETLINEWIDTH); the   */
    /* PS convention is to express widths in user-space units, which for    */
    /* Lout's default CTM equals internal units (1 PT = 20).  Multiply      */
    /* back so subsequent `setlinewidth` round-trips preserve the value.    */
    double w = s->gs[s->gs_top].line_width * (double) PT;
    if( w <= 0.0 ) w = (double) PT;       /* harmless default: 1 PT       */
    svg_ps_push_num(s, w);
    return 1;
  }
  case SVG_OP_SETDASH:
  {
    /* arg layout: <array> <offset> setdash */
    int i;
    int olen;
    char arrbuf[SVG_DASH_BUF_SIZE];
    (void) svg_ps_pop(s);   /* offset */
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY )
    {
      olen = 0;
      arrbuf[0] = '\0';
      for( i = 0; i < va.nitems && (size_t) olen + 16 < sizeof arrbuf; i++ )
      {
        char num[32];
        double v = (va.items[i].kind == SVG_VK_NUM) ? va.items[i].num : 0.0;
        if( i > 0 ) { arrbuf[olen++] = ','; arrbuf[olen] = '\0'; }
        sprintf(num, "%.3f", v / (double) PT);
        {
          int nlen = (int) strlen(num);
          memcpy(arrbuf + olen, num, (size_t) nlen);
          olen += nlen;
          arrbuf[olen] = '\0';
        }
      }
      if( va.nitems == 0 )
        s->gs[s->gs_top].dasharray[0] = '\0';
      else
      {
        size_t cap = sizeof s->gs[s->gs_top].dasharray - 1;
        size_t alen = strlen(arrbuf);
        if( alen > cap ) alen = cap;
        memcpy(s->gs[s->gs_top].dasharray, arrbuf, alen);
        s->gs[s->gs_top].dasharray[alen] = '\0';
      }
    }
    return 1;
  }
  case SVG_OP_GSAVE:
  case SVG_OP_SAVE:
    /* PS `save` snapshots the entire VM (graphics, dict, allocation modes)  */
    /* and pushes a save-object on the operand stack.  z53.c's interpreter   */
    /* tracks only the graphics state here, so the snapshot reduces to a    */
    /* gsave -- copy the current gstate, advance gs_top.  We also push a   */
    /* sentinel (NULL value) for `save` so a trailing `restore` finds      */
    /* something to pop: the @Fig prologue uses `save ... restore` and     */
    /* would otherwise underflow the operand stack.                        */
    if( s->gs_top + 1 < SVG_PS_GS_DEPTH )
    {
      s->gs[s->gs_top + 1] = s->gs[s->gs_top];
      s->gs_top++;
    }
    if( op_id == SVG_OP_SAVE )
    {
      svg_value sv;
      sv.kind = SVG_VK_NULL;
      sv.num = 0.0;
      sv.name = NULL;
      sv.items = NULL;
      sv.nitems = 0;
      sv.dict_id = 0;
      svg_ps_push(s, &sv);
    }
    return 1;
  case SVG_OP_GRESTORE:
    if( s->gs_top > 0 ) s->gs_top--;
    return 1;
  case SVG_OP_RESTORE:
    /* PS `restore` consumes the save-object on top of the stack and       */
    /* unwinds VM state to the matching `save`.  Mirror the gstate pop;    */
    /* eat the save-object sentinel (any value will do -- we don't check). */
    (void) svg_ps_pop_value(s);
    if( s->gs_top > 0 ) s->gs_top--;
    return 1;
  case SVG_OP_TRANSLATE:
  {
    /* Optional matrix variant: tx ty matrix translate -> matrix             */
    /* Plain variant:           tx ty translate         -> updates CTM       */
    double tx, ty;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_ARRAY )
    {
      /* fill the supplied matrix with a translate matrix and push back     */
      svg_value vm = svg_ps_pop_value(s);
      ty = svg_ps_pop(s); tx = svg_ps_pop(s);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        vm.items[0].num = 1.0; vm.items[1].num = 0.0;
        vm.items[2].num = 0.0; vm.items[3].num = 1.0;
        vm.items[4].num = tx;  vm.items[5].num = ty;
      }
      svg_ps_push(s, &vm);
    }
    else
    {
      ty = svg_ps_pop(s); tx = svg_ps_pop(s);
      svg_ps_ctm_translate(s, tx, ty);
    }
    return 1;
  }
  case SVG_OP_SCALE:
  {
    double sx, sy;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_ARRAY )
    {
      svg_value vm = svg_ps_pop_value(s);
      sy = svg_ps_pop(s); sx = svg_ps_pop(s);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        vm.items[0].num = sx;  vm.items[1].num = 0.0;
        vm.items[2].num = 0.0; vm.items[3].num = sy;
        vm.items[4].num = 0.0; vm.items[5].num = 0.0;
      }
      svg_ps_push(s, &vm);
    }
    else
    {
      sy = svg_ps_pop(s); sx = svg_ps_pop(s);
      svg_ps_ctm_scale(s, sx, sy);
    }
    return 1;
  }
  case SVG_OP_ROTATE:
  {
    double ang;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_ARRAY )
    {
      svg_value vm = svg_ps_pop_value(s);
      double c, sn, rad;
      ang = svg_ps_pop(s);
      rad = ang * SVG_PI / 180.0;
      c = cos(rad); sn = sin(rad);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        vm.items[0].num = c;   vm.items[1].num = sn;
        vm.items[2].num = -sn; vm.items[3].num = c;
        vm.items[4].num = 0.0; vm.items[5].num = 0.0;
      }
      svg_ps_push(s, &vm);
    }
    else
    {
      ang = svg_ps_pop(s);
      svg_ps_ctm_rotate(s, ang);
    }
    return 1;
  }
  case SVG_OP_CONCAT:
  {
    /* matrix concat -> pre-concat into CTM                                  */
    svg_value vm = svg_ps_pop_value(s);
    double tmp[6];
    int i;
    if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
    {
      for( i = 0; i < 6; i++ )
        tmp[i] = (vm.items[i].kind == SVG_VK_NUM) ? vm.items[i].num : 0.0;
      svg_ps_ctm_concat(s, tmp);
    }
    return 1;
  }
  case SVG_OP_TRANSFORM:
  case SVG_OP_DTRANSFORM:
  {
    /* "x y transform"        -> xd yd using CTM                              */
    /* "x y matrix transform" -> xd yd using supplied matrix                  */
    /* dtransform: same but ignores tx/ty (delta transform). Reasonable      */
    /* approximation here: apply with tx=ty=0.                                */
    double x, y, xd, yd;
    double mtmp[6];
    const double *m_use;
    int dtrans = (op_id == SVG_OP_DTRANSFORM);
    int i;
    svg_value vm;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_ARRAY )
    {
      vm = svg_ps_pop_value(s);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        for( i = 0; i < 6; i++ )
          mtmp[i] = (vm.items[i].kind == SVG_VK_NUM) ? vm.items[i].num : 0.0;
      }
      else
      {
        mtmp[0]=1; mtmp[1]=0; mtmp[2]=0; mtmp[3]=1; mtmp[4]=0; mtmp[5]=0;
      }
      m_use = mtmp;
    }
    else
      m_use = svg_ps_ctm(s);
    y = svg_ps_pop(s); x = svg_ps_pop(s);
    if( dtrans )
    {
      xd = m_use[0]*x + m_use[2]*y;
      yd = m_use[1]*x + m_use[3]*y;
    }
    else
      svg_ps_mat_apply(m_use, x, y, &xd, &yd);
    svg_ps_push_num(s, xd);
    svg_ps_push_num(s, yd);
    return 1;
  }
  case SVG_OP_ITRANSFORM:
  case SVG_OP_IDTRANSFORM:
  {
    double xd, yd, x, y;
    double mtmp[6];
    const double *m_use;
    int dtrans = (op_id == SVG_OP_IDTRANSFORM);
    int ok, i;
    svg_value vm;
    double det;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_ARRAY )
    {
      vm = svg_ps_pop_value(s);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        for( i = 0; i < 6; i++ )
          mtmp[i] = (vm.items[i].kind == SVG_VK_NUM) ? vm.items[i].num : 0.0;
      }
      else
      {
        mtmp[0]=1; mtmp[1]=0; mtmp[2]=0; mtmp[3]=1; mtmp[4]=0; mtmp[5]=0;
      }
      m_use = mtmp;
    }
    else
      m_use = svg_ps_ctm(s);
    yd = svg_ps_pop(s); xd = svg_ps_pop(s);
    if( dtrans )
    {
      det = m_use[0]*m_use[3] - m_use[1]*m_use[2];
      if( fabs(det) < 1e-12 ) { x = xd; y = yd; }
      else
      {
        x = ( xd * m_use[3] - yd * m_use[2]) / det;
        y = (-xd * m_use[1] + yd * m_use[0]) / det;
      }
    }
    else
    {
      ok = svg_ps_mat_apply_inverse(m_use, xd, yd, &x, &y);
      if( !ok ) { x = xd; y = yd; }
    }
    svg_ps_push_num(s, x);
    svg_ps_push_num(s, y);
    return 1;
  }
  case SVG_OP_MATRIX:
  case SVG_OP_IDENTMATRIX:
  {
    /* matrix     : push identity 6-element matrix array                     */
    /* identmatrix: pop array, fill with identity, push back                 */
    if( op_id == SVG_OP_IDENTMATRIX )
    {
      svg_value vm = svg_ps_pop_value(s);
      if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
      {
        vm.items[0].num = 1.0; vm.items[1].num = 0.0;
        vm.items[2].num = 0.0; vm.items[3].num = 1.0;
        vm.items[4].num = 0.0; vm.items[5].num = 0.0;
      }
      svg_ps_push(s, &vm);
    }
    else
    {
      svg_value out = svg_ps_new_matrix_array(NULL);
      svg_ps_push(s, &out);
    }
    return 1;
  }
  case SVG_OP_CURRENTMATRIX:
  {
    /* expects an array on the stack; fills it with the CTM and returns it.  */
    svg_value vm = svg_ps_pop_value(s);
    const double *m = svg_ps_ctm(s);
    if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
    {
      int i;
      for( i = 0; i < 6; i++ ) vm.items[i].num = m[i];
      svg_ps_push(s, &vm);
    }
    else
    {
      /* if no array arg supplied, fall back to fresh identity-shaped array */
      svg_value out = svg_ps_new_matrix_array(m);
      svg_ps_push(s, &out);
    }
    return 1;
  }
  case SVG_OP_SETMATRIX:
  {
    /* matrix setmatrix -> overwrite CTM                                     */
    svg_value vm = svg_ps_pop_value(s);
    if( vm.kind == SVG_VK_ARRAY && vm.nitems == 6 && vm.items != NULL )
    {
      double *m = svg_ps_ctm(s);
      int i;
      for( i = 0; i < 6; i++ )
        m[i] = (vm.items[i].kind == SVG_VK_NUM) ? vm.items[i].num : 0.0;
    }
    return 1;
  }
  case SVG_OP_CURRENTPOINT:
    svg_ps_push_num(s, s->cur_x);
    svg_ps_push_num(s, s->cur_y);
    return 1;
  case SVG_OP_CLIP:
  case SVG_OP_SHOWPAGE:
    /* Clipping with an empty current path masks all subsequent drawing in   */
    /* this gstate (and its gsave-descendants) until the matching grestore. */
    /* The @Diag prologue uses `newpath clip gsave` to suppress unwanted    */
    /* arrowhead nodes (e.g. the back-arrowhead when only forward is        */
    /* requested).  Honour this by flagging the current gstate; the path-  */
    /* emit primitive then drops paths drawn while the flag is set.        */
    if( op_id == SVG_OP_CLIP && !s->had_geom )
      s->gs[s->gs_top].clip_empty = 1;
    return 1;
  case SVG_OP_SHOW:
  {
    /* <string> show -- render at the current point in the active font.    */
    svg_value v = svg_ps_pop_value(s);
    if( v.kind == SVG_VK_STRING && v.name != NULL )
      svg_ps_show(s, v.name);
    return 1;
  }
  case SVG_OP_STRINGWIDTH:
  {
    /* Fixed-pitch approximation: width = strlen * (font_size * 0.5).      */
    /* Adequate for the few prologue paths (e.g. expstringshow's centring) */
    /* that consult the width before invoking show.                        */
    svg_value v = svg_ps_pop_value(s);
    double w = 0.0;
    if( v.kind == SVG_VK_STRING && v.name != NULL )
      w = (double) strlen(v.name) * s->gs[s->gs_top].font_size * 0.5;
    svg_ps_push_num(s, w);
    svg_ps_push_num(s, 0.0);
    return 1;
  }
  case SVG_OP_CHARPATH:
  {
    /* <string> <bool> charpath -- append the string's outline to the      */
    /* current path so a subsequent fill/stroke renders text as paths.     */
    /*                                                                     */
    /* Real outline path: for each byte, map StandardEncoding -> glyph     */
    /* name and ask svg_glyph_emit_outline (z53_glyph.c) to walk the Type  */
    /* 1 charstring through the path accumulator.  The outline service    */
    /* honours an optional LOUT_T1_FONT_DIR env var and falls back to a   */
    /* hardcoded list of system .pfb search dirs; if it can't find the    */
    /* font, or the glyph name isn't in the font's CharStrings dict, the  */
    /* per-character fallback is the original bbox rectangle (0.5 em wide */
    /* x 1.0 em tall).  coltex's `charpath flattenpath pathbbox` consumer */
    /* sees a bbox of the same plausible shape either way -- only the    */
    /* inside-the-rectangle path data changes.                            */
    svg_value vb_local = svg_ps_pop_value(s);  /* bool (true=stroke flag) */
    svg_value vs_local = svg_ps_pop_value(s);  /* string                  */
    double fs = s->gs[s->gs_top].font_size;
    double adv_default = fs * 0.5;   /* fallback advance per char         */
    double asc = fs * 0.8;           /* ascent above baseline (fallback)  */
    double desc = fs * 0.2;          /* descent below baseline (fallback) */
    double cx = s->cur_x;
    double y0 = s->cur_y;
    int i, n;
    const char *fname;
    (void) vb_local;
    fname = s->gs[s->gs_top].font_name[0] != '\0'
              ? s->gs[s->gs_top].font_name : "Times-Roman";
    if( vs_local.kind == SVG_VK_STRING && vs_local.name != NULL )
    {
      n = (int) strlen(vs_local.name);
      for( i = 0; i < n; i++ )
      {
        unsigned int c = (unsigned int) (unsigned char) vs_local.name[i];
        const char *gname = svg_ascii_glyph_name(c);
        double adv = adv_default;
        double adv_real = 0.0;
        int got = 0;
        if( gname != NULL )
          got = svg_glyph_emit_outline(fname, gname, fs, cx, y0, &adv_real,
            (void *) s,
            svg_charpath_cb_move, svg_charpath_cb_line,
            svg_charpath_cb_curve, svg_charpath_cb_close);
        if( got )
        {
          if( adv_real > 0.0 ) adv = adv_real;
        }
        else
        {
          /* fallback: bbox rectangle */
          svg_ps_moveto(s, cx,         y0 - desc);
          svg_ps_lineto(s, cx + adv,   y0 - desc);
          svg_ps_lineto(s, cx + adv,   y0 + asc);
          svg_ps_lineto(s, cx,         y0 + asc);
          svg_ps_closepath(s);
        }
        cx += adv;
      }
      /* Advance the current point past the string (PS charpath leaves   */
      /* the CP at the end of the last glyph, mirroring show).            */
      svg_ps_moveto(s, cx, y0);
    }
    return 1;
  }
  case SVG_OP_FINDFONT:
  {
    /* <name> findfont -- record name as the active font and push a dict.  */
    svg_value v = svg_ps_pop_value(s);
    svg_value out;
    if( (v.kind == SVG_VK_LITNAME || v.kind == SVG_VK_NAME ||
         v.kind == SVG_VK_STRING) && v.name != NULL )
    {
      size_t n = strlen(v.name);
      if( n >= sizeof s->gs[s->gs_top].font_name )
        n = sizeof s->gs[s->gs_top].font_name - 1;
      memcpy(s->gs[s->gs_top].font_name, v.name, n);
      s->gs[s->gs_top].font_name[n] = '\0';
    }
    /* Push a stand-in dict value (the setfont/scalefont path is opaque so  */
    /* the dict identity does not matter -- the gstate carries the real    */
    /* font state).                                                         */
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_SCALEFONT:
  {
    /* <fontdict> <scalar> scalefont <fontdict'> -- record scalar as size. */
    double sz = svg_ps_pop(s);
    /* leave the font dict on top of the stack untouched */
    s->gs[s->gs_top].font_size = sz;
    return 1;
  }
  case SVG_OP_SETFONT:
    /* <fontdict> setfont -- consumes the dict.  The gstate already        */
    /* carries the active font name/size set by findfont/scalefont.        */
    (void) svg_ps_pop_value(s);
    return 1;
  case SVG_OP_CURRENTFONT:
  {
    /* Push a stub font-dict value.  Used by `gsave currentfont 0.7        */
    /* scalefont setfont ... grestore` in graphf's exponent rendering --   */
    /* the dict identity is opaque, scalefont rebinds the gstate size.     */
    svg_value out;
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }

  /* @Graph plot-symbol prologue procs - factored into svg_ps_exec_symbol.  */
  case SVG_OP_SYM_FILLEDSQUARE:   case SVG_OP_SYM_DOFILLEDSQUARE:
  case SVG_OP_SYM_SQUARE:         case SVG_OP_SYM_DOSQUARE:
  case SVG_OP_SYM_FILLEDCIRCLE:   case SVG_OP_SYM_DOFILLEDCIRCLE:
  case SVG_OP_SYM_CIRCLE:         case SVG_OP_SYM_DOCIRCLE:
  case SVG_OP_SYM_FILLEDDIAMOND:  case SVG_OP_SYM_DOFILLEDDIAMOND:
  case SVG_OP_SYM_DIAMOND:        case SVG_OP_SYM_DODIAMOND:
  case SVG_OP_SYM_FILLEDTRIANGLE: case SVG_OP_SYM_DOFILLEDTRIANGLE:
  case SVG_OP_SYM_TRIANGLE:       case SVG_OP_SYM_DOTRIANGLE:
  case SVG_OP_SYM_CROSS:          case SVG_OP_SYM_DOCROSS:
  case SVG_OP_SYM_PLUS:           case SVG_OP_SYM_DOPLUS:
    return svg_ps_exec_symbol(s, name, op_id);

  /* Lout prologue named procedures - direct C implementations */
  case SVG_OP_LOUTGRAPHIC:
    svg_var_louts = svg_ps_pop(s);
    svg_var_loutv = svg_ps_pop(s);
    svg_var_loutf = svg_ps_pop(s);
    svg_var_ymark = svg_ps_pop(s);
    svg_var_xmark = svg_ps_pop(s);
    svg_var_ysize = svg_ps_pop(s);
    svg_var_xsize = svg_ps_pop(s);
    return 1;
  case SVG_OP_LOUTBOX:
    svg_ps_moveto(s, 0.0, 0.0);
    svg_ps_lineto(s, svg_var_xsize, 0.0);
    svg_ps_lineto(s, svg_var_xsize, svg_var_ysize);
    svg_ps_lineto(s, 0.0, svg_var_ysize);
    svg_ps_closepath(s);
    return 1;
  case SVG_OP_LOUTRULE:
    svg_ps_moveto(s, 0.0, 0.0);
    svg_ps_lineto(s, svg_var_xsize, 0.0);
    return 1;
  case SVG_OP_LOUTCURVEBOX:
  {
    double xm, xs, ys;
    xm = svg_var_xmark; xs = svg_var_xsize; ys = svg_var_ysize;
    svg_ps_moveto(s, xm, 0.0);
    svg_ps_arc(s, xs - xm, xm, xm, 270.0, 360.0, 1);
    svg_ps_arc(s, xs - xm, ys - xm, xm, 0.0, 90.0, 1);
    svg_ps_arc(s, xm, ys - xm, xm, 90.0, 180.0, 1);
    svg_ps_arc(s, xm, xm, xm, 180.0, 270.0, 1);
    svg_ps_closepath(s);
    return 1;
  }
  case SVG_OP_LOUTSHADOWBOX:
  {
    double xm = svg_var_xmark, xs = svg_var_xsize, ys = svg_var_ysize;
    svg_ps_moveto(s, xm * 2.0, 0.0);
    svg_ps_lineto(s, xs, 0.0);
    svg_ps_lineto(s, xs, ys - xm * 2.0);
    svg_ps_lineto(s, xs - xm, ys - xm * 2.0);
    svg_ps_lineto(s, xs - xm, xm);
    svg_ps_lineto(s, xm * 2.0, xm);
    svg_ps_closepath(s);
    return 1;
  }
  case SVG_OP_LOUTGR2:
  {
    /* gsave translate LoutGraphic gsave -- on stack:                       */
    /* xsize ysize xmark ymark loutf loutv louts tx ty LoutGr2              */
    /* In SVG mode the visible translate is emitted as a surrounding        */
    /* <g transform="..."> by the page layout (SVG_CoordTranslate), which   */
    /* also updates the module-level svg_outer_ctm that seeds the           */
    /* interpreter's CTM at the start of every @Graphic body.  So here we  */
    /* only consume the arguments and bind the LoutGraphic-style names; no  */
    /* CTM or gstate mutation is needed.                                    */
    (void) svg_ps_pop(s);  /* ty */
    (void) svg_ps_pop(s);  /* tx */
    svg_var_louts = svg_ps_pop(s);
    svg_var_loutv = svg_ps_pop(s);
    svg_var_loutf = svg_ps_pop(s);
    svg_var_ymark = svg_ps_pop(s);
    svg_var_xmark = svg_ps_pop(s);
    svg_var_ysize = svg_ps_pop(s);
    svg_var_xsize = svg_ps_pop(s);
    return 1;
  }
  case SVG_OP_SAVE_CP:
    return 1;
  case SVG_OP_LOUTTEXTURESOLID:
    /* PS: { null LoutSetTexture } bind def.  Clear any active texture so   */
    /* subsequent fills go back to a plain colour.                          */
    s->gs[s->gs_top].texture_kind = SVG_TEX_SOLID;
    return 1;
  case SVG_OP_LOUTSETTEXTURE:
  {
    /* PS: { pop } (no-texture build) or texture-stack manipulation.  Both    */
    /* variants consume exactly one operand.  An earlier no-op handler left   */
    /* the argument on the stack -- across many @Graphic invocations the     */
    /* leftover operand accumulated and eventually masked the connector     */
    /* outline/dashlength arguments of ldiagnodeend/ldiaglinkend, dropping  */
    /* the thin (0.48) connector strokes in late pages of the user guide.   */
    /* The argument here is either the value pushed by LoutMakeTexture       */
    /* (kind == SVG_VK_NUM, num == texture-kind index) or null (for the     */
    /* `null LoutSetTexture` sequence in LoutTextureSolid).                  */
    svg_value v = svg_ps_pop_value(s);
    if( v.kind == SVG_VK_NUM )
    {
      int k = (int) v.num;
      if( k > SVG_TEX_SOLID && k < SVG_TEX_COUNT )
        s->gs[s->gs_top].texture_kind = k;
      else
        s->gs[s->gs_top].texture_kind = SVG_TEX_SOLID;
    }
    else
      s->gs[s->gs_top].texture_kind = SVG_TEX_SOLID;
    return 1;
  }
  case SVG_OP_LOUTMAKETEXTURE:
  {
    /* PS: consumes 11 operands -- scale scalex scaley rotate hshift vshift   */
    /* painttype bbox xstep ystep paintproc -- and pushes a "pattern" value.  */
    /* In SVG we map the named textures (recognised by scanning the paint-   */
    /* proc body for distinctive operators) onto pre-defined <pattern> defs  */
    /* emitted at page open; the value pushed here carries the texture-kind  */
    /* index that the subsequent LoutSetTexture stores on the gstate.        */
    int i, kind;
    svg_value out;
    svg_value paintproc;
    paintproc = svg_ps_pop_value(s);    /* paintproc (top)              */
    kind = svg_tex_identify(&paintproc);
    for( i = 0; i < 10; i++ )           /* remaining 10 operands         */
      (void) svg_ps_pop_value(s);
    out.kind = SVG_VK_NUM;
    out.num = (double) kind;
    out.name = NULL; out.items = NULL; out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_LOUTPAGEDICT:
  {
    /* bsf.lpg: `/LoutPageDict 5 dict def` inside LoutPageSet.  In @Place      */
    /* bodies the name is referenced *before* any LoutPageSet has been         */
    /* persistently re-executed in our state, so a real lookup would fail and  */
    /* the trailing `begin matr setmatrix x y translate end gsave ...          */
    /* grestore` would corrupt the operand stack.  Push the userdict slot     */
    /* as a stand-in -- identical strategy to the SYSDICT alias group --      */
    /* so `begin`/`end` are balanced and the `matr setmatrix` step lands on  */
    /* an identity matrix (`matr` is aliased to the `matrix` op, which       */
    /* pushes a fresh 6-element identity array).                              */
    svg_value out;
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = svg_dict_stack[0];
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_LOUTPAGESET:
    /* bsf.lpg: takes no operands.  Side effect is to define LoutPageDict /   */
    /* matr / left / right / foot / top inside userdict.  We accept the call  */
    /* as a no-op -- the LoutPageDict / matr stand-ins above absorb the        */
    /* subsequent references.  Emit a one-line XML comment for traceability;  */
    /* this fires at most once per page (from dsf's @PageSet branch).         */
    if( out_fp != NULL )
      fputs("<!-- z53.c: LoutPageSet (page-dict init, no-op in SVG mode) -->\n",
        out_fp);
    return 1;
  case SVG_OP_LOUTMARGSET:
    /* bsf.lpg: `parity LoutMargSet -` (consumes 1 operand: the parity).  In  */
    /* PostScript this builds LoutMargDict for the subsequent @MargPut /      */
    /* LoutMargShift sequence.  In SVG mode margin notes are unsupported     */
    /* (see SVG_INCLUDES_AUDIT.md); pop the parity to keep the stack clean.  */
    (void) svg_ps_pop_value(s);
    if( out_fp != NULL )
      fputs("<!-- z53.c: LoutMargSet (margin notes unsupported in SVG mode) -->\n",
        out_fp);
    return 1;
  case SVG_OP_LOUTMARGSHIFT:
    /* bsf.lpg: `type LoutMargShift -` (consumes 1 operand: the margin       */
    /* type -- 0=left, 1=right, 2=outer, 3=inner).  In PostScript it         */
    /* translates the CTM so subsequent drawing lands in the margin.  In    */
    /* SVG mode the surrounding gsave/grestore still emits an empty <g>     */
    /* group; the body simply renders at the page origin instead of the     */
    /* margin.  Pop the operand to keep the stack consistent.                */
    (void) svg_ps_pop_value(s);
    if( out_fp != NULL )
      fputs("<!-- z53.c: LoutMargShift (margin notes unsupported in SVG mode) -->\n",
        out_fp);
    return 1;

  /* dictionary ops */
  case SVG_OP_DICT:
  {
    int id;
    svg_value out;
    (void) svg_ps_pop_value(s);  /* capacity */
    id = svg_dict_alloc();
    if( id < 0 ) id = 0;
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = id;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_BEGIN:
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_DICT && svg_dict_top + 1 < SVG_PS_DICT_STACK_DEPTH )
    {
      svg_dict_top++;
      svg_dict_stack[svg_dict_top] = va.dict_id;
    }
    return 1;
  case SVG_OP_END:
    if( svg_dict_top > 0 )
    {
      int did = svg_dict_stack[svg_dict_top];
      svg_dict_top--;
      /* Reclaim anonymous `N dict begin ... end` dicts so the pool doesn't  */
      /* exhaust during long documents (each @Diag node creates one).        */
      svg_dict_try_free_anonymous(did, s);
    }
    return 1;
  case SVG_OP_CURRENTDICT:
  {
    svg_value out;
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = (svg_dict_top >= 0) ? svg_dict_stack[svg_dict_top] : 0;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_SYSDICT:
  {
    /* Push the userdict slot as a stand-in for any system-defined dict.   */
    /* This keeps `<dictname> begin ... end` (used by prologue stanzas     */
    /* like `errordict begin /handleerror {...} def end` in diagf.lpg) in  */
    /* sync with the dict stack so that subsequent operations don't see a */
    /* stale operand left behind by an unbalanced `begin`.                */
    svg_value out;
    out.kind = SVG_VK_DICT;
    out.num = 0.0;
    out.name = NULL;
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = svg_dict_stack[0];
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_DEF:
    vb = svg_ps_pop_value(s);  /* value */
    va = svg_ps_pop_value(s);  /* name */
    if( va.kind == SVG_VK_LITNAME && va.name != NULL )
      svg_dict_stack_def(va.name, &vb);
    return 1;
  case SVG_OP_LOAD:
  {
    svg_value out;
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_LITNAME && va.name != NULL &&
        svg_dict_stack_lookup(va.name, &out) )
      svg_ps_push(s, &out);
    else
    {
      out.kind = SVG_VK_NULL;
      out.num = 0.0; out.name = NULL; out.items = NULL; out.nitems = 0;
      out.dict_id = 0;
      svg_ps_push(s, &out);
    }
    return 1;
  }
  case SVG_OP_WHERE:
  {
    svg_value out;
    const char *key;
    int j, found_did;
    va = svg_ps_pop_value(s);
    key = NULL;
    if( (va.kind == SVG_VK_LITNAME || va.kind == SVG_VK_NAME ||
         va.kind == SVG_VK_STRING) && va.name != NULL )
      key = va.name;
    found_did = -1;
    if( key != NULL )
    {
      /* Walk the dict stack and return the FIRST dict that contains the    */
      /* key, NOT the top dict.  (The earlier implementation always pointed */
      /* at the top dict, which made `forall` after `where` iterate the    */
      /* wrong dictionary -- that broke @Diag link/label promotion.)        */
      for( j = svg_dict_top; j >= 0; j-- )
      {
        if( svg_dict_lookup(svg_dict_stack[j], key, &out) )
        { found_did = svg_dict_stack[j]; break; }
      }
    }
    if( found_did >= 0 )
    {
      svg_value dv;
      dv.kind = SVG_VK_DICT;
      dv.num = 0.0; dv.name = NULL; dv.items = NULL; dv.nitems = 0;
      dv.dict_id = found_did;
      svg_ps_push(s, &dv);
      svg_ps_push_bool(s, 1);
    }
    else
      svg_ps_push_bool(s, 0);
    return 1;
  }
  case SVG_OP_KNOWN:
  {
    svg_value out;
    int found;
    vb = svg_ps_pop_value(s);  /* key */
    va = svg_ps_pop_value(s);  /* dict */
    found = 0;
    if( va.kind == SVG_VK_DICT && vb.kind == SVG_VK_LITNAME && vb.name != NULL )
      found = svg_dict_lookup(va.dict_id, vb.name, &out);
    svg_ps_push_bool(s, found);
    return 1;
  }
  case SVG_OP_EXEC:
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_PROC )
      svg_ps_exec_proc(s, &va);
    else
      svg_ps_exec_value(s, &va);
    return 1;
  case SVG_OP_BIND:
    return 1;   /* no-op */
  case SVG_OP_CVX:
    /* Make the top stack item executable.  Arrays become procedures and    */
    /* literal names become executable names.  Critical for prologue code   */
    /* that builds executable arrays inline via [ ... /op cvx ... ] cvx --  */
    /* without LITNAME->NAME the embedded /op stays literal and silently    */
    /* fails to run when the proc is later executed.                        */
    if( s->top > 0 )
    {
      if( s->stack[s->top - 1].kind == SVG_VK_ARRAY )
        s->stack[s->top - 1].kind = SVG_VK_PROC;
      else if( s->stack[s->top - 1].kind == SVG_VK_LITNAME )
        s->stack[s->top - 1].kind = SVG_VK_NAME;
    }
    return 1;
  case SVG_OP_CVLIT:
    if( s->top > 0 )
    {
      if( s->stack[s->top - 1].kind == SVG_VK_PROC )
        s->stack[s->top - 1].kind = SVG_VK_ARRAY;
      else if( s->stack[s->top - 1].kind == SVG_VK_NAME )
        s->stack[s->top - 1].kind = SVG_VK_LITNAME;
    }
    return 1;
  case SVG_OP_TYPE:
  {
    svg_value out;
    const char *tn;
    va = svg_ps_pop_value(s);
    switch( va.kind )
    {
      case SVG_VK_NUM:     tn = "realtype";    break;
      case SVG_VK_BOOL:    tn = "booleantype"; break;
      case SVG_VK_NAME:    tn = "nametype";    break;
      case SVG_VK_LITNAME: tn = "nametype";    break;
      case SVG_VK_STRING:  tn = "stringtype";  break;
      case SVG_VK_PROC:    tn = "arraytype";   break;
      case SVG_VK_ARRAY:   tn = "arraytype";   break;
      case SVG_VK_DICT:    tn = "dicttype";    break;
      case SVG_VK_NULL:    tn = "nulltype";    break;
      default:             tn = "nulltype";    break;
    }
    out.kind = SVG_VK_LITNAME;
    out.num = 0.0;
    out.name = svg_arena_strdup(tn, (int) strlen(tn));
    out.items = NULL;
    out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_XCHECK:
    va = svg_ps_pop_value(s);
    svg_ps_push_bool(s, va.kind == SVG_VK_PROC || va.kind == SVG_VK_NAME);
    return 1;

  /* boolean predicates */
  case SVG_OP_TRUE:  svg_ps_push_bool(s, 1); return 1;
  case SVG_OP_FALSE: svg_ps_push_bool(s, 0); return 1;
  case SVG_OP_NULL:
  {
    svg_value out;
    out.kind = SVG_VK_NULL;
    out.num = 0.0; out.name = NULL; out.items = NULL; out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }

  case SVG_OP_EQ:
  case SVG_OP_NE:
  {
    int eq = 0;
    vb = svg_ps_pop_value(s); va = svg_ps_pop_value(s);
    /* PostScript eq: numbers compare by value, booleans by value, but a   */
    /* number and a boolean are never equal -- they're distinct types.    */
    /* The graphf.lpg axesstyle dispatch `xaxis false eq yaxis false eq   */
    /* or { framestyle } { ... } ifelse` relies on this: when `xaxis`     */
    /* and `yaxis` are 0 (the common `xorigin { 0 }` case on user-guide   */
    /* pp. 248, 262) they must not test equal to the boolean `false`, or  */
    /* the dispatch wrongly selects framestyle and the axis lines + tick  */
    /* labels are never drawn.                                            */
    if( va.kind == SVG_VK_NUM && vb.kind == SVG_VK_NUM )
      eq = (va.num == vb.num);
    else if( va.kind == SVG_VK_BOOL && vb.kind == SVG_VK_BOOL )
      eq = (va.num == vb.num);
    else if( (va.kind == SVG_VK_NAME || va.kind == SVG_VK_LITNAME ||
              va.kind == SVG_VK_STRING) &&
             (vb.kind == SVG_VK_NAME || vb.kind == SVG_VK_LITNAME ||
              vb.kind == SVG_VK_STRING) )
      eq = (va.name != NULL && vb.name != NULL &&
            strcmp(va.name, vb.name) == 0);
    else
      eq = (va.kind == vb.kind);
    if( op_id == SVG_OP_NE ) eq = !eq;
    svg_ps_push_bool(s, eq);
    return 1;
  }
  case SVG_OP_LT:
  case SVG_OP_GT:
  case SVG_OP_LE:
  case SVG_OP_GE:
  {
    int r;
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    if( op_id == SVG_OP_LT ) r = (a <  b);
    else if( op_id == SVG_OP_GT ) r = (a >  b);
    else if( op_id == SVG_OP_LE ) r = (a <= b);
    else r = (a >= b);
    svg_ps_push_bool(s, r);
    return 1;
  }
  case SVG_OP_AND:
  case SVG_OP_OR:
  case SVG_OP_XOR:
  {
    int ai, bi, r;
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    ai = a != 0.0; bi = b != 0.0;
    if( op_id == SVG_OP_AND ) r = ai & bi;
    else if( op_id == SVG_OP_OR ) r = ai | bi;
    else r = ai ^ bi;
    svg_ps_push_bool(s, r);
    return 1;
  }
  case SVG_OP_NOT:
    a = svg_ps_pop(s);
    svg_ps_push_bool(s, a == 0.0);
    return 1;

  /* arithmetic */
  case SVG_OP_ADD: b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a + b); return 1;
  case SVG_OP_SUB: b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a - b); return 1;
  case SVG_OP_MUL: b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a * b); return 1;
  case SVG_OP_DIV:
    b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, b == 0.0 ? 0.0 : a / b); return 1;
  case SVG_OP_IDIV:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_push_num(s, b == 0.0 ? 0.0 : (double)((long)a / (long)b));
    return 1;
  case SVG_OP_MOD:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_push_num(s, b == 0.0 ? 0.0 : (double)((long)a % (long)b));
    return 1;
  case SVG_OP_NEG: a = svg_ps_pop(s); svg_ps_push_num(s, -a); return 1;
  case SVG_OP_ABS: a = svg_ps_pop(s); svg_ps_push_num(s, a < 0 ? -a : a); return 1;
  case SVG_OP_SQRT: a = svg_ps_pop(s); svg_ps_push_num(s, a < 0 ? 0.0 : sqrt(a)); return 1;
  case SVG_OP_SIN:
    a = svg_ps_pop(s); svg_ps_push_num(s, sin(a * SVG_PI / 180.0)); return 1;
  case SVG_OP_COS:
    a = svg_ps_pop(s); svg_ps_push_num(s, cos(a * SVG_PI / 180.0)); return 1;
  case SVG_OP_ATAN:
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    {
      double r;
      if( a == 0.0 && b == 0.0 ) r = 0.0;
      else r = atan2(a, b) * 180.0 / SVG_PI;
      if( r < 0.0 ) r += 360.0;
      svg_ps_push_num(s, r);
    }
    return 1;
  case SVG_OP_EXP:
    b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, pow(a, b)); return 1;
  case SVG_OP_LN:
    a = svg_ps_pop(s); svg_ps_push_num(s, a <= 0 ? 0.0 : log(a)); return 1;
  case SVG_OP_LOG:
    a = svg_ps_pop(s); svg_ps_push_num(s, a <= 0 ? 0.0 : log10(a)); return 1;
  case SVG_OP_TRUNCATE:
    a = svg_ps_pop(s); svg_ps_push_num(s, a >= 0 ? floor(a) : ceil(a)); return 1;
  case SVG_OP_FLOOR:
    a = svg_ps_pop(s); svg_ps_push_num(s, floor(a)); return 1;
  case SVG_OP_CEILING:
    a = svg_ps_pop(s); svg_ps_push_num(s, ceil(a)); return 1;
  case SVG_OP_ROUND:
    a = svg_ps_pop(s); svg_ps_push_num(s, floor(a + 0.5)); return 1;
  case SVG_OP_CVI:
    a = svg_ps_pop(s); svg_ps_push_num(s, (double)(long)a); return 1;
  case SVG_OP_CVR: return 1;  /* numeric already */
  case SVG_OP_CVS:
  {
    char numbuf[64];
    svg_value out;
    svg_value src;
    const char *str = NULL;
    (void) svg_ps_pop_value(s);   /* discard target string */
    src = svg_ps_pop_value(s);
    numbuf[0] = '\0';
    if( src.kind == SVG_VK_NAME || src.kind == SVG_VK_LITNAME ||
        src.kind == SVG_VK_STRING )
      str = src.name;
    else if( src.kind == SVG_VK_BOOL )
      str = (src.num != 0.0) ? "true" : "false";
    else if( src.kind == SVG_VK_NUM )
    {
      sprintf(numbuf, "%g", src.num);
      str = numbuf;
    }
    if( str == NULL ) str = "";
    out.kind = SVG_VK_STRING;
    out.num = 0.0;
    out.name = svg_arena_strdup(str, (int) strlen(str));
    out.items = NULL;
    out.nitems = (int) strlen(str);
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }
  case SVG_OP_CVN:
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_STRING )
    {
      va.kind = SVG_VK_LITNAME;
      svg_ps_push(s, &va);
    }
    else
      svg_ps_push(s, &va);
    return 1;
  case SVG_OP_STRING:
  {
    svg_value out;
    int slen;
    char *buf;
    slen = (int) svg_ps_pop(s);
    if( slen < 0 ) slen = 0;
    buf = (char *) svg_arena_alloc((size_t) slen + 1, 0);
    if( buf != NULL )
    {
      int k;
      for( k = 0; k < slen; k++ ) buf[k] = ' ';
      buf[slen] = '\0';
    }
    out.kind = SVG_VK_STRING;
    out.num = 0.0;
    out.name = buf;
    out.items = NULL;
    out.nitems = slen;   /* track explicit length */
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }

  /* stack manipulation */
  case SVG_OP_POP: (void) svg_ps_pop_value(s); return 1;
  case SVG_OP_DUP:
    if( s->top > 0 )
    {
      svg_value top = s->stack[s->top - 1];
      svg_ps_push(s, &top);
    }
    return 1;
  case SVG_OP_EXCH:
    if( s->top >= 2 )
    {
      svg_value t = s->stack[s->top - 1];
      s->stack[s->top - 1] = s->stack[s->top - 2];
      s->stack[s->top - 2] = t;
    }
    return 1;
  case SVG_OP_INDEX:
  {
    int n;
    a = svg_ps_pop(s);
    n = (int) a;
    if( n >= 0 && s->top - 1 - n >= 0 )
    {
      svg_value v = s->stack[s->top - 1 - n];
      svg_ps_push(s, &v);
    }
    return 1;
  }
  case SVG_OP_COPY:
  {
    int n, i;
    if( s->top > 0 && s->stack[s->top - 1].kind == SVG_VK_NUM )
    {
      n = (int) s->stack[s->top - 1].num;
      s->top--;
      if( n > 0 && s->top >= n && s->top + n <= SVG_PS_STACK_DEPTH )
      {
        for( i = 0; i < n; i++ )
          s->stack[s->top + i] = s->stack[s->top - n + i];
        s->top += n;
      }
    }
    else
    {
      /* copy with composite operand - silently ignore */
      (void) svg_ps_pop_value(s);
    }
    return 1;
  }
  case SVG_OP_ROLL:
  {
    int n, j, k, i;
    svg_value tmp[SVG_PS_STACK_DEPTH];
    j = (int) svg_ps_pop(s);  /* shift */
    n = (int) svg_ps_pop(s);  /* count */
    if( n <= 0 || n > s->top ) return 1;
    /* normalise j into [0, n) */
    j = j % n;
    if( j < 0 ) j += n;
    for( i = 0; i < n; i++ )
      tmp[i] = s->stack[s->top - n + i];
    for( i = 0; i < n; i++ )
    {
      k = (i - j + n) % n;
      s->stack[s->top - n + i] = tmp[k];
    }
    return 1;
  }
  case SVG_OP_CLEAR: s->top = 0; return 1;
  case SVG_OP_COUNT: svg_ps_push_num(s, (double) s->top); return 1;
  case SVG_OP_MARK:
  {
    svg_value m;
    m.kind = SVG_VK_MARK; m.num = 0.0; m.name = NULL;
    m.items = NULL; m.nitems = 0; m.dict_id = 0;
    svg_ps_push(s, &m);
    return 1;
  }
  case SVG_OP_CLEARTOMARK:
  {
    int i;
    for( i = s->top - 1; i >= 0; i-- )
      if( s->stack[i].kind == SVG_VK_MARK ) { s->top = i; return 1; }
    s->top = 0;
    return 1;
  }
  case SVG_OP_COUNTTOMARK:
  {
    int i, c = 0;
    for( i = s->top - 1; i >= 0; i-- )
    {
      if( s->stack[i].kind == SVG_VK_MARK ) { svg_ps_push_num(s, (double) c); return 1; }
      c++;
    }
    svg_ps_push_num(s, (double) c);
    return 1;
  }

  /* array constructor terminator ']' */
  case SVG_OP_RBRACKET:
  {
    svg_value arr;
    svg_collect_to_mark(s, &arr);
    svg_ps_push(s, &arr);
    return 1;
  }

  /* array ops */
  case SVG_OP_ALOAD:
  {
    int i;
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY )
    {
      for( i = 0; i < va.nitems; i++ )
        svg_ps_push(s, &va.items[i]);
      svg_ps_push(s, &va);
    }
    return 1;
  }
  case SVG_OP_ASTORE:
  {
    int i;
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY )
    {
      for( i = va.nitems - 1; i >= 0; i-- )
      {
        if( s->top > 0 )
          va.items[i] = s->stack[--s->top];
      }
      svg_ps_push(s, &va);
    }
    return 1;
  }
  case SVG_OP_LENGTH:
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY || va.kind == SVG_VK_PROC )
      svg_ps_push_num(s, (double) va.nitems);
    else if( va.kind == SVG_VK_STRING && va.name != NULL )
      svg_ps_push_num(s, (double) strlen(va.name));
    else
      svg_ps_push_num(s, 0.0);
    return 1;
  case SVG_OP_GET:
  {
    svg_value out;
    int idx;
    vb = svg_ps_pop_value(s);  /* key */
    va = svg_ps_pop_value(s);  /* composite */
    if( va.kind == SVG_VK_DICT &&
        (vb.kind == SVG_VK_LITNAME || vb.kind == SVG_VK_NAME ||
         vb.kind == SVG_VK_STRING) && vb.name != NULL )
    {
      if( svg_dict_lookup(va.dict_id, vb.name, &out) )
        svg_ps_push(s, &out);
    }
    else if( (va.kind == SVG_VK_ARRAY || va.kind == SVG_VK_PROC) &&
             vb.kind == SVG_VK_NUM )
    {
      idx = (int) vb.num;
      if( idx >= 0 && idx < va.nitems )
        svg_ps_push(s, &va.items[idx]);
    }
    return 1;
  }
  case SVG_OP_PUT:
  {
    svg_value vv = svg_ps_pop_value(s);  /* value */
    vb = svg_ps_pop_value(s);            /* key */
    va = svg_ps_pop_value(s);            /* composite */
    if( va.kind == SVG_VK_DICT && vb.kind == SVG_VK_LITNAME && vb.name != NULL )
      svg_dict_def(va.dict_id, vb.name, &vv);
    else if( (va.kind == SVG_VK_ARRAY) && vb.kind == SVG_VK_NUM )
    {
      int idx = (int) vb.num;
      if( idx >= 0 && idx < va.nitems && va.items != NULL )
        va.items[idx] = vv;
    }
    return 1;
  }
  case SVG_OP_PUTINTERVAL:
  {
    /* dest index src  putinterval  --   (string variant; copies src into  */
    /* dest starting at index, mutating dest in place).                    */
    svg_value vd, vs;
    int idx, srclen, dstlen;
    vs = svg_ps_pop_value(s);            /* source */
    vb = svg_ps_pop_value(s);            /* index */
    vd = svg_ps_pop_value(s);            /* dest   */
    if( vd.kind == SVG_VK_STRING && vs.kind == SVG_VK_STRING &&
        vb.kind == SVG_VK_NUM && vd.name != NULL && vs.name != NULL )
    {
      idx = (int) vb.num;
      srclen = (int) strlen(vs.name);
      dstlen = (int) strlen(vd.name);
      if( idx >= 0 && idx + srclen <= dstlen )
        memcpy(vd.name + idx, vs.name, (size_t) srclen);
    }
    return 1;
  }
  case SVG_OP_SEARCH:
  {
    /* string seek  search  -> post match pre true                         */
    /*                      -> string false   (if not found)                */
    svg_value vstr, vseek, vpre, vmatch, vpost;
    const char *hit;
    int seeklen, prelen, postlen;
    vseek = svg_ps_pop_value(s);
    vstr  = svg_ps_pop_value(s);
    if( vstr.kind != SVG_VK_STRING || vseek.kind != SVG_VK_STRING ||
        vstr.name == NULL || vseek.name == NULL )
    {
      svg_ps_push(s, &vstr);
      svg_ps_push_bool(s, 0);
      return 1;
    }
    hit = strstr(vstr.name, vseek.name);
    if( hit == NULL )
    {
      svg_ps_push(s, &vstr);
      svg_ps_push_bool(s, 0);
      return 1;
    }
    seeklen = (int) strlen(vseek.name);
    prelen  = (int) (hit - vstr.name);
    postlen = (int) strlen(vstr.name) - prelen - seeklen;
    vpost.kind = SVG_VK_STRING;
    vpost.num = 0.0;
    vpost.name = svg_arena_strdup(hit + seeklen, postlen);
    vpost.items = NULL;
    vpost.nitems = postlen;
    vpost.dict_id = 0;
    vmatch.kind = SVG_VK_STRING;
    vmatch.num = 0.0;
    vmatch.name = svg_arena_strdup(hit, seeklen);
    vmatch.items = NULL;
    vmatch.nitems = seeklen;
    vmatch.dict_id = 0;
    vpre.kind = SVG_VK_STRING;
    vpre.num = 0.0;
    vpre.name = svg_arena_strdup(vstr.name, prelen);
    vpre.items = NULL;
    vpre.nitems = prelen;
    vpre.dict_id = 0;
    svg_ps_push(s, &vpost);
    svg_ps_push(s, &vmatch);
    svg_ps_push(s, &vpre);
    svg_ps_push_bool(s, 1);
    return 1;
  }

  /* control flow */
  case SVG_OP_IF:
  {
    svg_value proc = svg_ps_pop_value(s);
    a = svg_ps_pop(s);
    if( a != 0.0 ) svg_ps_call(s, &proc);
    return 1;
  }
  case SVG_OP_IFELSE:
  {
    svg_value pf = svg_ps_pop_value(s);  /* false branch */
    svg_value pt = svg_ps_pop_value(s);  /* true branch */
    a = svg_ps_pop(s);
    if( a != 0.0 ) svg_ps_call(s, &pt);
    else            svg_ps_call(s, &pf);
    return 1;
  }
  case SVG_OP_FOR:
  {
    svg_value proc = svg_ps_pop_value(s);
    double init, incr, limit, val;
    limit = svg_ps_pop(s);
    incr  = svg_ps_pop(s);
    init  = svg_ps_pop(s);
    val = init;
    while( (incr > 0 && val <= limit) || (incr < 0 && val >= limit) ||
           (incr == 0 && val == limit) )
    {
      svg_ps_push_num(s, val);
      svg_ps_call(s, &proc);
      if( svg_exit_flag || svg_stop_flag ) break;
      if( incr == 0 ) break;
      val += incr;
    }
    svg_exit_flag = 0;
    return 1;
  }
  case SVG_OP_REPEAT:
  {
    svg_value proc = svg_ps_pop_value(s);
    int n, i;
    n = (int) svg_ps_pop(s);
    for( i = 0; i < n; i++ )
    {
      svg_ps_call(s, &proc);
      if( svg_exit_flag || svg_stop_flag ) break;
    }
    svg_exit_flag = 0;
    return 1;
  }
  case SVG_OP_LOOP:
  {
    svg_value proc = svg_ps_pop_value(s);
    int safety = 100000;
    while( safety-- > 0 )
    {
      svg_ps_call(s, &proc);
      if( svg_exit_flag || svg_stop_flag ) break;
    }
    svg_exit_flag = 0;
    return 1;
  }
  case SVG_OP_FORALL:
  {
    svg_value proc = svg_ps_pop_value(s);
    int i;
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY || va.kind == SVG_VK_PROC )
    {
      for( i = 0; i < va.nitems; i++ )
      {
        svg_ps_push(s, &va.items[i]);
        svg_ps_call(s, &proc);
        if( svg_exit_flag || svg_stop_flag ) break;
      }
    }
    else if( va.kind == SVG_VK_DICT &&
             va.dict_id >= 0 && va.dict_id < SVG_PS_DICT_POOL )
    {
      svg_dict *d = &svg_dict_pool[va.dict_id];
      for( i = 0; i < SVG_PS_DICT_ENTRIES; i++ )
      {
        svg_value kv;
        if( !d->entries[i].used || d->entries[i].name == NULL )
          continue;
        kv.kind = SVG_VK_LITNAME;
        kv.num = 0.0;
        kv.name = d->entries[i].name;
        kv.items = NULL;
        kv.nitems = 0;
        kv.dict_id = 0;
        svg_ps_push(s, &kv);
        svg_ps_push(s, &d->entries[i].value);
        svg_ps_call(s, &proc);
        if( svg_exit_flag || svg_stop_flag ) break;
      }
    }
    svg_exit_flag = 0;
    return 1;
  }
  case SVG_OP_EXIT: svg_exit_flag = 1; return 1;
  case SVG_OP_STOP: svg_stop_flag = 1; return 1;
  case SVG_OP_STOPPED:
  {
    svg_value proc = svg_ps_pop_value(s);
    int was_stop = svg_stop_flag;
    svg_stop_flag = 0;
    svg_ps_call(s, &proc);
    {
      int r = svg_stop_flag;
      svg_stop_flag = was_stop;
      svg_ps_push_bool(s, r);
    }
    return 1;
  }

  /* Lout numeric prologue helpers (defined in z49.c prologue):              */
  /*   in cm pt em sp vs ft dg - these multiply by scale constants.          */
  case SVG_OP_IN: a = svg_ps_pop(s); svg_ps_push_num(s, a * 1440.0); return 1;
  case SVG_OP_CM: a = svg_ps_pop(s); svg_ps_push_num(s, a * 566.929); return 1;
  case SVG_OP_PT: a = svg_ps_pop(s); svg_ps_push_num(s, a * 20.0); return 1;
  case SVG_OP_EM: a = svg_ps_pop(s); svg_ps_push_num(s, a * 120.0); return 1;
  case SVG_OP_SP: a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_louts); return 1;
  case SVG_OP_VS: a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_loutv); return 1;
  case SVG_OP_FT: a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_loutf); return 1;
  case SVG_OP_DG: return 1;  /* identity */

  default:
    return 0;
  }  /* switch */
}

/* @Graph plot-symbol prologue procs - factored out of svg_ps_exec_op.  See */
/* notes in the original implementation: bypass the dict procs entirely    */
/* and draw a fixed-size symbol path centred at (xcurr, ycurr) directly.   */
static int svg_ps_exec_symbol(svg_ps_state *s, const char *name,
  svg_op_id op_id)
{
  double slw, ss, yc, xc;
  int is_do = (strncmp(name, "do", 2) == 0);
  int outline;
  int is_filled;
  const char *shape;
  (void) op_id;  /* the resolved op_id is informational; name carries shape */
  /* All graphf.lpg symbols use an "open" form (square, circle, ...)    */
  /* drawn as an outline stroke, and a "filled" form (filledsquare,    */
  /* ...) drawn as a solid fill.  cross/plus are stroked single-line  */
  /* glyphs in both forms (no fill).                                  */
  /* Strip the "do" prefix first, then the optional "filled" prefix,  */
  /* so all four spellings (square / dosquare / filledsquare /         */
  /* dofilledsquare) reduce to a bare shape name plus an is_filled    */
  /* flag.  Without the second strip "filledsquare" never matched any */
  /* shape branch below and the function silently emitted nothing --  */
  /* the root cause of the user-guide page 248 / 262 regression where */
  /* @Graph plot symbols vanished from the SVG output.                */
  shape = is_do ? name + 2 : name;
  is_filled = (strncmp(shape, "filled", 6) == 0);
  if( is_filled )
    shape = shape + 6;
  outline = !is_filled;
  /* cross and plus are stroked single-line glyphs in either spelling;  */
  /* never fill.  Their dispatch below already passes 1,0 to emit_path. */
  if( !is_do )
  {
    /* No-arg wrapper: pull xcurr/ycurr/symbolsize/symbollinewidth     */
    /* from the dict stack, transform (xcurr, ycurr) via the same     */
    /* axis-mapping that trpoint does.  Fall back to (0, 0) and a    */
    /* sensible default size if any lookup fails, so we degrade        */
    /* gracefully rather than blowing up the page.                    */
    svg_value vv;
    double xcur = 0.0, ycur = 0.0;
    if( svg_dict_stack_lookup("xcurr", &vv) && vv.kind == SVG_VK_NUM )
      xcur = vv.num;
    if( svg_dict_stack_lookup("ycurr", &vv) && vv.kind == SVG_VK_NUM )
      ycur = vv.num;
    ss = 0.15 * svg_var_loutf;
    if( svg_dict_stack_lookup("symbolsize", &vv) && vv.kind == SVG_VK_NUM )
      ss = vv.num;
    slw = 0.5;
    if( svg_dict_stack_lookup("symbollinewidth", &vv) && vv.kind == SVG_VK_NUM )
      slw = vv.num;
    /* trpoint: map data-space (xcur, ycur) -> graphic-space (xc, yc). */
    /* Implemented by inlining the relevant graphf.lpg arithmetic       */
    /* using dict-bound axis variables.  All of these are simple        */
    /* numerics defined by xset / yset; if they're missing we leave    */
    /* the point un-transformed, which still beats a 5x-size blob.     */
    {
      double trxmin = 0, trxmax = 1, trymin = 0, trymax = 1;
      double xwidth = 0, ywidth = 0, xextra = 0, yextra = 0;
      double xdecr = 0, ydecr = 0;
      double xlog = 0, ylog = 0;
      if( svg_dict_stack_lookup("trxmin", &vv) && vv.kind == SVG_VK_NUM )
        trxmin = vv.num;
      if( svg_dict_stack_lookup("trxmax", &vv) && vv.kind == SVG_VK_NUM )
        trxmax = vv.num;
      if( svg_dict_stack_lookup("trymin", &vv) && vv.kind == SVG_VK_NUM )
        trymin = vv.num;
      if( svg_dict_stack_lookup("trymax", &vv) && vv.kind == SVG_VK_NUM )
        trymax = vv.num;
      if( svg_dict_stack_lookup("xwidth", &vv) && vv.kind == SVG_VK_NUM )
        xwidth = vv.num;
      if( svg_dict_stack_lookup("ywidth", &vv) && vv.kind == SVG_VK_NUM )
        ywidth = vv.num;
      if( svg_dict_stack_lookup("xextra", &vv) && vv.kind == SVG_VK_NUM )
        xextra = vv.num;
      if( svg_dict_stack_lookup("yextra", &vv) && vv.kind == SVG_VK_NUM )
        yextra = vv.num;
      /* xdecr/ydecr: graphf.lpg's xset/yset bind these from a boolean    */
      /* argument via `/xdecr exch def`, so the dict entry has kind BOOL  */
      /* (vv.num == 1.0 for true, 0.0 for false), not NUM.  Accept both   */
      /* so the symbol position tracks descending-axis graphs the same    */
      /* way the curve does.                                              */
      if( svg_dict_stack_lookup("xdecr", &vv) &&
          (vv.kind == SVG_VK_NUM || vv.kind == SVG_VK_BOOL) )
        xdecr = vv.num;
      if( svg_dict_stack_lookup("ydecr", &vv) &&
          (vv.kind == SVG_VK_NUM || vv.kind == SVG_VK_BOOL) )
        ydecr = vv.num;
      if( svg_dict_stack_lookup("xlog", &vv) && vv.kind == SVG_VK_NUM )
        xlog = vv.num;
      if( svg_dict_stack_lookup("ylog", &vv) && vv.kind == SVG_VK_NUM )
        ylog = vv.num;
      if( xlog > 1 && xcur > 0 ) xcur = log(xcur) / log(xlog);
      if( ylog > 1 && ycur > 0 ) ycur = log(ycur) / log(ylog);
      if( trxmax - trxmin != 0.0 )
        xc = (xdecr != 0.0 ? (trxmax - xcur) : (xcur - trxmin))
             / (trxmax - trxmin) * xwidth + xextra;
      else
        xc = xextra;
      if( trymax - trymin != 0.0 )
        yc = (ydecr != 0.0 ? (trymax - ycur) : (ycur - trymin))
             / (trymax - trymin) * ywidth + yextra;
      else
        yc = yextra;
    }
  }
  else
  {
    /* do<shape>: 4-arg form, stack has x y symbolsize symbollinewidth. */
    slw = svg_ps_pop(s);
    ss  = svg_ps_pop(s);
    yc  = svg_ps_pop(s);
    xc  = svg_ps_pop(s);
  }
  if( strcmp(shape, "square") == 0 )
  {
    double half = outline ? (ss - slw * 0.5) : ss;
    if( half < 0.0 ) half = 0.0;
    svg_ps_moveto(s, xc - half, yc - half);
    svg_ps_lineto(s, xc + half, yc - half);
    svg_ps_lineto(s, xc + half, yc + half);
    svg_ps_lineto(s, xc - half, yc + half);
    svg_ps_closepath(s);
    svg_ps_emit_path(s, outline, !outline);
  }
  else if( strcmp(shape, "circle") == 0 )
  {
    double r = outline ? (ss - slw * 0.5) : ss;
    if( r < 0.0 ) r = 0.0;
    svg_ps_moveto(s, xc + r, yc);
    svg_ps_arc(s, xc, yc, r, 0.0, 180.0, 1);
    svg_ps_arc(s, xc, yc, r, 180.0, 360.0, 1);
    svg_ps_closepath(s);
    svg_ps_emit_path(s, outline, !outline);
  }
  else if( strcmp(shape, "diamond") == 0 )
  {
    double half = outline ? (ss - slw * 0.5) : ss;
    if( half < 0.0 ) half = 0.0;
    svg_ps_moveto(s, xc - half, yc);
    svg_ps_lineto(s, xc, yc - half);
    svg_ps_lineto(s, xc + half, yc);
    svg_ps_lineto(s, xc, yc + half);
    svg_ps_closepath(s);
    svg_ps_emit_path(s, outline, !outline);
  }
  else if( strcmp(shape, "triangle") == 0 )
  {
    double h = outline ? (ss - slw * 0.5) : ss;
    if( h < 0.0 ) h = 0.0;
    svg_ps_moveto(s, xc, yc + h * 1.5);
    svg_ps_lineto(s, xc - h, yc - h);
    svg_ps_lineto(s, xc + h, yc - h);
    svg_ps_closepath(s);
    svg_ps_emit_path(s, outline, !outline);
  }
  else if( strcmp(shape, "cross") == 0 )
  {
    svg_ps_moveto(s, xc - ss, yc - ss);
    svg_ps_lineto(s, xc + ss, yc + ss);
    svg_ps_emit_path(s, 1, 0);
    svg_ps_moveto(s, xc - ss, yc + ss);
    svg_ps_lineto(s, xc + ss, yc - ss);
    svg_ps_emit_path(s, 1, 0);
  }
  else if( strcmp(shape, "plus") == 0 )
  {
    svg_ps_moveto(s, xc - ss, yc);
    svg_ps_lineto(s, xc + ss, yc);
    svg_ps_emit_path(s, 1, 0);
    svg_ps_moveto(s, xc, yc - ss);
    svg_ps_lineto(s, xc, yc + ss);
    svg_ps_emit_path(s, 1, 0);
  }
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_is_graph_symbol_proc - is `name` one of the 20 @Graph plot-symbol    */
/*  shortcut ops (filledsquare, dosquare, circle, plus, ...)?  Used to      */
/*  intercept them BEFORE the dictionary lookup so graphf.lpg's potentially */
/*  buggy dict-procs don't shadow our C implementations.  First-char        */
/*  switch keeps the non-matching common case (moveto, xcurr, ...) cheap.   */
/*                                                                           */
/*****************************************************************************/

static int svg_is_graph_symbol_proc(const char *name)
{
  /* Names: filledsquare, dofilledsquare, square, dosquare,                   */
  /*        filledcircle, dofilledcircle, circle, docircle,                  */
  /*        filleddiamond, dofilleddiamond, diamond, dodiamond,              */
  /*        filledtriangle, dofilledtriangle, triangle, dotriangle,          */
  /*        cross, docross, plus, doplus.                                    */
  switch( name[0] )
  {
    case 'f':
      /* filledsquare / filledcircle / filleddiamond / filledtriangle */
      if( name[1] != 'i' || name[2] != 'l' || name[3] != 'l' ||
          name[4] != 'e' || name[5] != 'd' )
        return 0;
      return strcmp(name + 6, "square")   == 0 ||
             strcmp(name + 6, "circle")   == 0 ||
             strcmp(name + 6, "diamond")  == 0 ||
             strcmp(name + 6, "triangle") == 0;
    case 'd':
      /* diamond, or do<symbol> prefix */
      if( name[1] == 'i' )
        return strcmp(name, "diamond") == 0;
      if( name[1] != 'o' ) return 0;
      return strcmp(name + 2, "filledsquare")   == 0 ||
             strcmp(name + 2, "filledcircle")   == 0 ||
             strcmp(name + 2, "filleddiamond")  == 0 ||
             strcmp(name + 2, "filledtriangle") == 0 ||
             strcmp(name + 2, "square")   == 0 ||
             strcmp(name + 2, "circle")   == 0 ||
             strcmp(name + 2, "diamond")  == 0 ||
             strcmp(name + 2, "triangle") == 0 ||
             strcmp(name + 2, "cross")    == 0 ||
             strcmp(name + 2, "plus")     == 0;
    case 's':
      return strcmp(name, "square") == 0;
    case 'c':
      return strcmp(name, "circle") == 0 ||
             strcmp(name, "cross")  == 0;
    case 't':
      return strcmp(name, "triangle") == 0;
    case 'p':
      return strcmp(name, "plus") == 0;
    default:
      return 0;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_exec_value - push a value, or execute it if executable.          */
/*                                                                           */
/*****************************************************************************/

static void svg_ps_exec_value(svg_ps_state *s, const svg_value *v)
{
  svg_value resolved;
  double dv;

  if( svg_stop_flag || svg_exit_flag )
    return;
  if( v == NULL )
    return;
  svg_recursion_depth++;
  if( svg_recursion_depth > SVG_PS_MAX_RECURSION )
  {
    svg_recursion_depth--;
    svg_stop_flag = 1;
    return;
  }

  switch( v->kind )
  {
    case SVG_VK_NUM:
    case SVG_VK_BOOL:
    case SVG_VK_LITNAME:
    case SVG_VK_STRING:
    case SVG_VK_ARRAY:
    case SVG_VK_DICT:
    case SVG_VK_MARK:
    case SVG_VK_NULL:
    case SVG_VK_PROC:
      /* Literal procedures are PUSHED rather than executed.  Execution    */
      /* happens only via `exec`, control-flow operators, or name lookup. */
      svg_ps_push(s, v);
      break;

    case SVG_VK_NAME:
      if( v->name == NULL ) break;
      /* Lout-bound numerics resolve directly */
      if( svg_ps_resolve_value(s, v->name, &dv) )
      {
        svg_ps_push_num(s, dv);
        break;
      }
      /* A short list of @Graph prologue procs that we override with C        */
      /* implementations in svg_ps_exec_op to sidestep the dict-proc bug      */
      /* responsible for the inflated-symbol regression on user's-guide      */
      /* pages 248/262.  These names must be intercepted BEFORE the          */
      /* dictionary lookup or graphf.lpg's procs would shadow them.          */
      /* First-char dispatch keeps the common path (moveto, xcurr, ...)      */
      /* to one cheap byte comparison instead of 20 strcmps.                 */
      if( svg_is_graph_symbol_proc(v->name) )
      {
        if( svg_ps_exec_op(s, v->name) )
          break;
      }
      /* dictionary lookup */
      if( svg_dict_stack_lookup(v->name, &resolved) )
      {
        if( resolved.kind == SVG_VK_PROC )
          svg_ps_exec_proc(s, &resolved);
        else
          svg_ps_push(s, &resolved);
        break;
      }
      /* built-in operator? */
      if( svg_ps_exec_op(s, v->name) )
        break;
      /* unknown: warn (rate-limited) and also emit a one-line XML comment   */
      /* in the SVG output so the fall-through is visible to readers of the */
      /* file, not only to whoever was watching stderr.  Per-op only, not  */
      /* per-buffer: don't drop the whole @Graphic on the floor as the   */
      /* legacy fallback did -- the rest of the operators in the buffer  */
      /* may still translate cleanly.                                      */
      /*                                                                   */
      /* Suppress entirely for names that look like Lout-level tag         */
      /* identifiers rather than PostScript operators.  The @Diag prologue */
      /* and @Fig macros expand tag references (`A1`, `B1`, `FROM`, `LMID`,*/
      /* `LFROM`, `LTO`, `XINDENT`, ...) into the @Graphic body via Lout's */
      /* macro layer; those names never reach the PostScript dict because */
      /* the binding lives in Lout's symbol table, not in any @Graphic    */
      /* prologue.  No real PS operator looks like this -- they're all    */
      /* lowercase or mixed-case (moveto, LoutSetRGBColor) -- so a name   */
      /* that's all uppercase ASCII letters + digits (and starts with a   */
      /* letter) is almost certainly a tag-name leak.  Skip both stderr  */
      /* and the SVG XML comment for those: they're not bugs in the      */
      /* interpreter, just side-effects of macro expansion.              */
      {
        int is_tag_name = 0;
        if( v->name != NULL && v->name[0] >= 'A' && v->name[0] <= 'Z' )
        {
          const char *p;
          is_tag_name = 1;
          for( p = v->name; *p != '\0'; p++ )
          {
            int ch = (unsigned char) *p;
            if( !((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) )
            {
              is_tag_name = 0;
              break;
            }
          }
        }
        if( is_tag_name )
          break;
      }
      if( svg_warn_unknown_count < SVG_WARN_MAX )
      {
        svg_warn_unknown_count++;
        if( svg_warn_unknown_count == SVG_WARN_MAX )
        {
          fprintf(stderr,
            "lout (SVG): further unknown PostScript operators suppressed\n");
          if( out_fp != NULL )
            fputs("<!-- z53.c: further unknown PostScript ops suppressed -->\n",
              out_fp);
        }
        else
        {
          fprintf(stderr,
            "lout (SVG): unknown PostScript operator '%s'\n", v->name);
          if( out_fp != NULL && v->name != NULL )
          {
            /* sanitise: only emit alphanumerics + a small set of safe punct   */
            /* characters to avoid accidentally closing the comment.          */
            const char *p;
            fputs("<!-- z53.c: unimplemented PostScript op '", out_fp);
            for( p = v->name; *p != '\0'; p++ )
            {
              int ch = (unsigned char) *p;
              if( (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                  ch == '.' || ch == '@' )
                fputc(ch, out_fp);
              else
                fputc('_', out_fp);
            }
            fputs("' -->\n", out_fp);
          }
        }
      }
      break;

    default:
      break;
  }
  svg_recursion_depth--;
}

static void svg_ps_exec_proc(svg_ps_state *s, const svg_value *proc)
{
  int i;
  if( proc == NULL || proc->items == NULL ) return;
  for( i = 0; i < proc->nitems; i++ )
  {
    if( svg_exit_flag || svg_stop_flag ) return;
    svg_ps_exec_value(s, &proc->items[i]);
  }
}

static void svg_ps_call(svg_ps_state *s, const svg_value *v)
{
  /* Execute a value: if it's a procedure, run its body; otherwise treat   */
  /* like exec_value (which pushes literals).                              */
  if( v == NULL ) return;
  if( v->kind == SVG_VK_PROC )
    svg_ps_exec_proc(s, v);
  else
    svg_ps_exec_value(s, v);
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ps_run - entry point: tokenise, parse, execute one @Graphic body.   */
/*                                                                           */
/*  A small content-keyed cache of already-parsed value arrays lives next   */
/*  to svg_ps_run.  Many @Graphic invocations re-feed the exact same       */
/*  prologue procedure body (graphf.lpg, diagf.lpg, ...) verbatim.  Caching */
/*  the parsed `vals` array skips both svg_ps_tokenise and svg_parse_tokens */
/*  on hit.  The cached vals/nvals are arena-owned and live for the entire */
/*  lout invocation (svg_arena_free_all is only called from               */
/*  svg_psinterp_init/shutdown), so re-execution is safe so long as the    */
/*  executor never mutates its input value array -- which it does not.     */
/*                                                                           */
/*****************************************************************************/

#define SVG_RUN_CACHE_SIZE 256                  /* power of 2; ring-buffer */
#define SVG_RUN_CACHE_MASK (SVG_RUN_CACHE_SIZE - 1)

typedef struct svg_run_cache_entry {
  unsigned int hash;        /* FNV-1a 32-bit hash of (buf, n)              */
  int          n;           /* original buffer length                      */
  const char  *buf_copy;    /* arena-owned content snapshot                */
  svg_value   *vals;        /* parsed value array (arena-owned)            */
  int          nvals;
} svg_run_cache_entry;

static svg_run_cache_entry svg_run_cache[SVG_RUN_CACHE_SIZE];
static int svg_run_cache_next = 0;              /* FIFO insertion index    */

/* FNV-1a 32-bit over a byte range (not NUL-terminated). */
static unsigned int svg_run_cache_hash(const char *buf, int n)
{
  unsigned int h = 2166136261u;
  int i;
  for( i = 0; i < n; i++ )
  {
    h ^= (unsigned char) buf[i];
    h *= 16777619u;
  }
  return h;
}

/* Returns the cached entry index for (buf, n), or -1 on miss. */
static int svg_run_cache_find(const char *buf, int n, unsigned int h)
{
  int i;
  svg_run_cache_entry *e;
  for( i = 0; i < SVG_RUN_CACHE_SIZE; i++ )
  {
    e = &svg_run_cache[i];
    if( e->buf_copy == NULL ) continue;
    if( e->hash != h || e->n != n ) continue;
    if( memcmp(e->buf_copy, buf, (size_t) n) == 0 )
      return i;
  }
  return -1;
}

/* Drop all cached entries.  Called from svg_psinterp_init / shutdown        */
/* immediately after svg_arena_free_all() has invalidated the arena-owned   */
/* buf_copy / vals pointers held by the cache.                              */
static void svg_run_cache_reset(void)
{
  int i;
  for( i = 0; i < SVG_RUN_CACHE_SIZE; i++ )
  {
    svg_run_cache[i].hash     = 0;
    svg_run_cache[i].n        = 0;
    svg_run_cache[i].buf_copy = NULL;
    svg_run_cache[i].vals     = NULL;
    svg_run_cache[i].nvals    = 0;
  }
  svg_run_cache_next = 0;
}

/* Insert (or overwrite the FIFO victim slot with) a fresh cache entry. */
static void svg_run_cache_insert(const char *buf, int n, unsigned int h,
  svg_value *vals, int nvals)
{
  svg_run_cache_entry *e;
  char *copy;
  int slot;
  /* Refuse to cache pathologically huge bodies -- the win is in the small,  */
  /* repeated prologues; large one-off bodies just bloat the arena.          */
  if( n <= 0 || n > 65536 ) return;
  copy = (char *) svg_arena_alloc((size_t) n, 0);
  if( copy == NULL ) return;
  memcpy(copy, buf, (size_t) n);
  slot = svg_run_cache_next;
  svg_run_cache_next = (svg_run_cache_next + 1) & SVG_RUN_CACHE_MASK;
  e = &svg_run_cache[slot];
  /* Old entry (if any) leaks its arena slots until svg_arena_free_all at    */
  /* shutdown; acceptable -- the FIFO ring caps total turnover.              */
  e->hash     = h;
  e->n        = n;
  e->buf_copy = copy;
  e->vals     = vals;
  e->nvals    = nvals;
}

static void svg_ps_run(const char *buf, int n, svg_ps_state *s)
{
  static svg_ps_tok toks[SVG_PS_MAX_TOKENS];
  svg_value *vals;
  int ntoks, cur, nvals, i, hit;
  unsigned int h;

  /* Cache probe -- skip tokenise+parse on hit. */
  h = svg_run_cache_hash(buf, n);
  hit = svg_run_cache_find(buf, n, h);
  if( hit >= 0 )
  {
    vals  = svg_run_cache[hit].vals;
    nvals = svg_run_cache[hit].nvals;
  }
  else
  {
    ntoks = svg_ps_tokenise(buf, n, toks, SVG_PS_MAX_TOKENS);
    cur = 0;
    nvals = 0;
    vals = svg_parse_tokens(buf, toks, ntoks, &cur, 0, &nvals);
    if( vals != NULL && nvals > 0 )
      svg_run_cache_insert(buf, n, h, vals, nvals);
  }

  svg_exit_flag = 0;
  svg_stop_flag = 0;
  svg_recursion_depth = 0;

  if( vals != NULL )
  {
    for( i = 0; i < nvals; i++ )
    {
      if( svg_stop_flag ) break;
      svg_exit_flag = 0;
      svg_ps_exec_value(s, &vals[i]);
    }
  }

  if( s->had_geom )
    svg_ps_emit_path(s, 1, 0);

  /* Sweep the dict pool to reclaim anonymous dicts that became unreachable  */
  /* during this run (e.g., tag-dicts that ldiagpoptagdict popped while a    */
  /* transient operand-stack reference prevented svg_dict_try_free_anonymous */
  /* from reclaiming them at the moment of `end`).                            */
  svg_dict_gc_sweep(s);
}


/*****************************************************************************/
/*                                                                           */
/*  svg_psinterp_init / svg_psinterp_shutdown - persistent dictionary state */
/*  across the whole document.                                              */
/*                                                                           */
/*****************************************************************************/

/* Forward decl for cache reset, defined alongside svg_ps_run. */
static void svg_run_cache_reset(void);

static void svg_psinterp_init(void)
{
  int i;
  svg_arena_free_all();
  svg_run_cache_reset();
  for( i = 0; i < SVG_PS_DICT_POOL; i++ )
    svg_dict_pool[i].in_use = 0;
  svg_dict_pool_used = 0;
  /* allocate one bottom dict (userdict) */
  svg_dict_pool[0].in_use = 1;
  svg_dict_clear(&svg_dict_pool[0]);
  svg_dict_pool[0].in_use = 1;
  svg_dict_stack[0] = 0;
  svg_dict_top = 0;
  svg_warn_unknown_count = 0;
  svg_recursion_depth = 0;
  svg_exit_flag = 0;
  svg_stop_flag = 0;
  svg_var_xsize = 0.0;
  svg_var_ysize = 0.0;
  svg_var_xmark = 0.0;
  svg_var_ymark = 0.0;
  svg_var_loutf = 12.0 * PT;
  svg_var_loutv = 12.0 * PT;
  svg_var_louts = 4.0 * PT;
  /* Initialise the module-persistent PS interpreter state.                */
  svg_ps_init(&g_psstate);
}

static void svg_psinterp_shutdown(void)
{
  int i;
  svg_arena_free_all();
  svg_run_cache_reset();
  for( i = 0; i < SVG_PS_DICT_POOL; i++ )
    svg_dict_pool[i].in_use = 0;
  svg_dict_top = -1;
}


/*****************************************************************************/
/*                                                                           */
/*  svg_ingest_prepend_files                                                 */
/*                                                                           */
/*  Read every @SysPrependGraphic / @PrependGraphic file and feed its       */
/*  contents through the PS interpreter so any procedure defined there      */
/*  (e.g. all of diagf.lpg's ldiag* helpers) becomes available to           */
/*  subsequent @Graphic clauses via the persistent dictionary stack.        */
/*                                                                           */
/*****************************************************************************/

static void svg_ingest_prepend_files(void)
{
  FILE_NUM fnum;
  FILE *fp;
  static char prepend_buf[262144];
  int buflen;
  size_t got;
  svg_ps_state psstate;
  for( fnum = FirstFile(PREPEND_FILE);  fnum != NO_FILE;
       fnum = NextFile(fnum) )
  {
    fp = OpenFile(fnum, FALSE, FALSE);
    if( fp == NULL )
      continue;
    buflen = 0;
    while( !feof(fp) && buflen + 4096 < (int) sizeof prepend_buf )
    {
      got = fread(prepend_buf + buflen, 1, 4096, fp);
      if( got == 0 ) break;
      buflen += (int) got;
    }
    fclose(fp);
    if( buflen <= 0 ) continue;
    if( buflen >= (int) sizeof prepend_buf )
      buflen = (int) sizeof prepend_buf - 1;
    prepend_buf[buflen] = '\0';
    svg_ps_init(&psstate);
    svg_ps_run(prepend_buf, buflen, &psstate);
  }
}


/*****************************************************************************/
/*                                                                           */
/*  SVG_PrintGraphicObject                                                   */
/*                                                                           */
/*  Flatten the @Graphic ACAT into one buffer and interpret it as PS.  If   */
/*  the buffer's first non-space char is '<', pass through verbatim (raw    */
/*  SVG markup).                                                             */
/*                                                                           */
/*****************************************************************************/

static void SVG_PrintGraphicObject(OBJECT x)
{
  static char buf[SVG_GRAPHIC_BUF_SIZE];
  int len, i;
  char first;
  if( out_fp == NULL )
    return;
  buf[0] = '\0';
  len = svg_graphic_concat(x, buf, 0, SVG_GRAPHIC_BUF_SIZE);
  if( len == 0 )
    return;
  first = '\0';
  for( i = 0; i < len; i++ )
  {
    if( buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r' )
    {
      first = buf[i];
      break;
    }
  }
  if( first == '<' )
  {
    fputs(buf, out_fp);
    fputc('\n', out_fp);
    return;
  }

  /* Use the module-persistent PS interpreter state.  The gs stack, CTM,   */
  /* and dictionary state survive across SVG_PrintGraphicObject calls so   */
  /* that PS-internal translates/rotates emitted in a parent @Graphic body */
  /* (e.g. the arrow-position translate inside the @Diag prologue) remain */
  /* in effect when the child @Graphic (e.g. an arrowhead node) is later   */
  /* invoked.  Only the per-call path accumulator and current-point are    */
  /* reset here.                                                            */
  g_psstate.path[0] = '\0';
  g_psstate.plen    = 0;
  g_psstate.cur_x   = 0.0;
  g_psstate.cur_y   = 0.0;
  g_psstate.last_xp = 0.0;
  g_psstate.last_yp = 0.0;
  g_psstate.last_pt_valid = FALSE;
  g_psstate.have_cp = FALSE;
  g_psstate.had_geom = FALSE;
  if( cur_gr_set )
  {
    svg_var_xsize = (double) cur_gr_xsize;
    svg_var_ysize = (double) cur_gr_ysize;
    svg_var_xmark = (double) cur_gr_xmark;
    svg_var_ymark = (double) cur_gr_ymark;
    svg_var_loutf = (double) cur_gr_loutf;
    svg_var_loutv = (double) cur_gr_loutv;
    svg_var_louts = (double) cur_gr_louts;
  }
  svg_ps_run(buf, len, &g_psstate);
}

static void SVG_DefineGraphicNames(OBJECT x)
{
  /* Capture the same numbers that PS_DefineGraphicNames pushes onto the    */
  /* PS stack before "LoutGraphic", so the interpreter inside              */
  /* SVG_PrintGraphicObject can resolve `xsize`, `ysize`, etc.             */
  COLOUR_NUM col;
  char colbuf[24];
  const char *colstr;
  cur_gr_xsize = size(x, COLM);
  cur_gr_ysize = size(x, ROWM);
  cur_gr_xmark = back(x, COLM);
  cur_gr_ymark = fwd(x, ROWM);
  /* Mirror PS_DefineGraphicNames in z49.c: use the actual font size for     */
  /* this graphic (so `loutf` and `ft` reflect any enclosing @Font scaling   */
  /* such as `font { -2p }` inside @Graph), falling back to 12pt only when   */
  /* no font has been set up yet.                                            */
  {
    FONT_NUM gf = font(save_style(x));
    cur_gr_loutf = (gf <= 0) ? 12 * PT : FontSize(gf, x);
  }
  cur_gr_loutv = width(line_gap(save_style(x)));
  cur_gr_louts = width(space_gap(save_style(x)));
  cur_gr_set   = TRUE;

  /* Propagate the current Lout colour (set by an enclosing @Colour or     */
  /* @SetColour) into the PS interpreter's per-gstate fill/stroke colour.  */
  /* PS mode achieves this naturally via SetColourAndTexture writing a    */
  /* "...setrgbcolor" command into the PS output before LoutGraphic; SVG  */
  /* has no implicit current point so we mirror it explicitly.  This is   */
  /* what makes @Box paint{darkred}, @FilledBox under @Colour, and other  */
  /* colour-bearing @Graphic blocks render in the right colour rather     */
  /* than the SVG default "currentColor" (which inherits to black).       */
  col = colour(save_style(x));
  if( col > 0 )
  {
    colstr = svg_colour_rgb(col, colbuf);
    if( colstr != NULL )
    {
      strncpy(g_psstate.gs[g_psstate.gs_top].fill_rgb, colstr,
              sizeof(g_psstate.gs[g_psstate.gs_top].fill_rgb) - 1);
      g_psstate.gs[g_psstate.gs_top].fill_rgb
        [sizeof(g_psstate.gs[g_psstate.gs_top].fill_rgb) - 1] = '\0';
      strncpy(g_psstate.gs[g_psstate.gs_top].stroke_rgb, colstr,
              sizeof(g_psstate.gs[g_psstate.gs_top].stroke_rgb) - 1);
      g_psstate.gs[g_psstate.gs_top].stroke_rgb
        [sizeof(g_psstate.gs[g_psstate.gs_top].stroke_rgb) - 1] = '\0';
    }
  }
}

static void SVG_SaveTranslateDefineSave(OBJECT x, FULL_LENGTH xdist,
  FULL_LENGTH ydist)
{
  /* Mirror PS_SaveTranslateDefineSave's effect: save, translate, define-   */
  /* graphic-names, save -- so the @Graphic body runs in a frame anchored   */
  /* at the bottom-left of the object x.                                    */
  SVG_SaveGraphicState(x);
  SVG_CoordTranslate(xdist, ydist);
  SVG_DefineGraphicNames(x);
  SVG_SaveGraphicState(x);
}


/*****************************************************************************/
/*                                                                           */
/*  SVG_PrintGraphicInclude                                                  */
/*                                                                           */
/*  Coordinates are computed in the page-level y-flipped frame, with each   */
/*  raster image wrapped in its own counter-flip so the image is displayed   */
/*  the right way up.                                                        */
/*                                                                           */
/*****************************************************************************/

static void SVG_PrintGraphicInclude(OBJECT x, FULL_LENGTH colmark,
  FULL_LENGTH rowmark)
{
  OBJECT y;
  FULL_CHAR *fname;
  const char *ext;
  size_t flen, elen, j;
  double cx, cy, cw, ch;
  char lext[8];
  if( out_fp == NULL || !page_open )
    return;
  Child(y, Down(x))
    ;
  fname = string(y);
  if( fname == NULL )
    return;
  flen = strlen((const char *) fname);
  ext = "";
  elen = (flen >= 5) ? 5 : flen;
  for( j = 0; j < elen; j++ )
  {
    int c = (unsigned char) fname[flen - elen + j];
    if( c >= 'A' && c <= 'Z' ) c += 32;
    lext[j] = (char) c;
  }
  lext[elen] = '\0';
  if( flen >= 4 )
  {
    const char *tail = lext + elen - 4;
    if(      strcmp(tail, ".svg") == 0 ) ext = ".svg";
    else if( strcmp(tail, ".png") == 0 ) ext = ".png";
    else if( strcmp(tail, ".jpg") == 0 ) ext = ".jpg";
    else if( strcmp(tail, ".gif") == 0 ) ext = ".gif";
    else if( strcmp(tail, ".eps") == 0 ) ext = ".eps";
  }
  if( flen >= 5 && strcmp(lext, ".jpeg") == 0 )
    ext = ".jpg";
  /* In the page-level flipped frame, Lout (x,y) maps directly. */
  cx = (double) (colmark - back(x, COLM))           / PT;
  cy = (double) (rowmark - fwd(x, ROWM))            / PT;
  cw = (double) (back(x, COLM) + fwd(x, COLM))      / PT;
  ch = (double) (back(x, ROWM) + fwd(x, ROWM))      / PT;
  if( strcmp(ext, ".svg") == 0 ||
      strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
      strcmp(ext, ".gif") == 0 )
  {
    /* counter-flip so the image is upright in the page-level flipped frame */
    fprintf(out_fp,
      "<g transform=\"translate(%.3f,%.3f) scale(1,-1)\">"
      "<image href=\"%s\" x=\"0\" y=\"0\" width=\"%.3f\" height=\"%.3f\"/>"
      "</g>\n",
      cx, cy + ch, (const char *) fname, cw, ch);
  }
  else if( strcmp(ext, ".eps") == 0 )
  {
    fprintf(out_fp,
      "<!-- @IncludeGraphic %s: EPS not supported in SVG mode -->\n",
      (const char *) fname);
  }
  else
  {
    fprintf(out_fp,
      "<!-- @IncludeGraphic %s: unknown file type -->\n",
      (const char *) fname);
  }
}


/*****************************************************************************/
/*                                                                           */
/*  Hyperlinks                                                               */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_link_id(OBJECT name, char *buf, size_t buflen)
{
  FULL_CHAR *p;
  size_t i;
  if( buflen < 5 )
  {
    if( buflen > 0 ) buf[0] = '\0';
    return;
  }
  strcpy(buf, "LOUT");
  i = 4;
  for( p = string(name); *p != '\0' && i + 1 < buflen; p++ )
  {
    int c = (unsigned char) *p;
    if( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') )
      buf[i++] = (char) c;
    else
      buf[i++] = '_';
  }
  buf[i] = '\0';
}


static void svg_emit_link_rect(FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{
  double x_pt, y_pt, w_pt, h_pt;
  x_pt = (double) llx                / PT;
  y_pt = (double) lly                / PT;
  w_pt = (double) (urx - llx)        / PT;
  h_pt = (double) (ury - lly)        / PT;
  fprintf(out_fp,
    "<rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" "
    "fill=\"none\" pointer-events=\"all\"/>",
    x_pt, y_pt, w_pt, h_pt);
}


static void SVG_LinkSource(OBJECT name, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{
  char idbuf[256];
  if( out_fp == NULL || !page_open || name == NULL )
    return;
  svg_emit_link_id(name, idbuf, sizeof idbuf);
  fprintf(out_fp, "<a xlink:href=\"#%s\">", idbuf);
  svg_emit_link_rect(llx, lly, urx, ury);
  fputs("</a>\n", out_fp);
}


static void SVG_LinkDest(OBJECT name, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{
  char idbuf[256];
  double x_pt, y_pt;
  if( out_fp == NULL || !page_open || name == NULL )
    return;
  svg_emit_link_id(name, idbuf, sizeof idbuf);
  x_pt = (double) llx / PT;
  y_pt = (double) ury / PT;
  fprintf(out_fp,
    "<a id=\"%s\"><rect x=\"%.3f\" y=\"%.3f\" width=\"0\" height=\"0\" "
    "fill=\"none\"/></a>\n",
    idbuf, x_pt, y_pt);
}


static void SVG_LinkURL(OBJECT url, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{
  if( out_fp == NULL || !page_open || url == NULL || !is_word(type(url)) )
    return;
  fputs("<a xlink:href=\"", out_fp);
  svg_emit_xml_escaped(string(url));
  fputs("\" target=\"_blank\">", out_fp);
  svg_emit_link_rect(llx, lly, urx, ury);
  fputs("</a>\n", out_fp);
}


static void SVG_LinkCheck(void)
{
}


/*****************************************************************************/
/*                                                                           */
/*  SVG_BackEnd                                                              */
/*                                                                           */
/*****************************************************************************/

static struct back_end_rec svg_back = {
  SVG,                                  /* the code number of the back end   */
  STR_SVG,                              /* string name of the back end       */
  TRUE,                                 /* TRUE if @Scale is available       */
  TRUE,                                 /* TRUE if @Rotate is available      */
  TRUE,                                 /* TRUE if @HMirror, @VMirror avail  */
  TRUE,                                 /* TRUE if @Graphic is available     */
  TRUE,                                 /* TRUE if @IncludeGraphic is avail. */
  FALSE,                                /* TRUE if @PlainGraphic is avail.   */
  TRUE,                                 /* TRUE if fractional spacing avail. */
  TRUE,                                 /* TRUE if actual font metrics used  */
  TRUE,                                 /* TRUE if colour is available       */
  SVG_PrintInitialize,
  SVG_PrintLength,
  SVG_PrintPageSetupForFont,
  SVG_PrintPageResourceForFont,
  SVG_PrintMapping,
  SVG_PrintBeforeFirstPage,
  SVG_PrintBetweenPages,
  SVG_PrintAfterLastPage,
  SVG_PrintWord,
  SVG_PrintPlainGraphic,
  SVG_PrintUnderline,
  SVG_CoordTranslate,
  SVG_CoordRotate,
  SVG_CoordScale,
  SVG_CoordHMirror,
  SVG_CoordVMirror,
  SVG_SaveGraphicState,
  SVG_RestoreGraphicState,
  SVG_PrintGraphicObject,
  SVG_DefineGraphicNames,
  SVG_SaveTranslateDefineSave,
  SVG_PrintGraphicInclude,
  SVG_LinkSource,
  SVG_LinkDest,
  SVG_LinkURL,
  SVG_LinkCheck,
};

BACK_END SVG_BackEnd = &svg_back;


/*****************************************************************************/
/*                                                                           */
/*  SVG_NullBackEnd                                                          */
/*                                                                           */
/*  A null (non-printing) version of the SVG back end, mirroring the layout */
/*  of PS_NullBackEnd in z49.c.  Used by z01.c for non-final cross-reference */
/*  resolution passes, where the engine traverses the formatted tree but no */
/*  output should be emitted.  Every callback here is a true no-op; we do   */
/*  NOT reuse the real SVG_* callbacks (some of which perform expensive     */
/*  per-page work like ingesting @SysPrependGraphic files into the PS       */
/*  interpreter, or maintain CTM/dictionary state that is irrelevant when   */
/*  no SVG is being emitted).                                                */
/*                                                                           */
/*****************************************************************************/

static void SVG_NullPrintInitialize(FILE *fp, BOOLEAN enc)
{}

static void SVG_NullPrintPageSetupForFont(OBJECT face, int font_curr_page,
  FULL_CHAR *font_name, FULL_CHAR *short_name)
{}

static void SVG_NullPrintPageResourceForFont(FULL_CHAR *font_name,
  BOOLEAN first)
{}

static void SVG_NullPrintMapping(MAPPING m)
{}

static void SVG_NullPrintBeforeFirstPage(FULL_LENGTH h, FULL_LENGTH v,
  FULL_CHAR *label)
{}

static void SVG_NullPrintBetweenPages(FULL_LENGTH h, FULL_LENGTH v,
  FULL_CHAR *label)
{}

static void SVG_NullPrintAfterLastPage(void)
{}

static void SVG_NullPrintWord(OBJECT x, int hpos, int vpos)
{}

static void SVG_NullPrintPlainGraphic(OBJECT x, FULL_LENGTH xmk,
  FULL_LENGTH ymk, OBJECT z)
{}

static void SVG_NullPrintUnderline(FONT_NUM fnum, COLOUR_NUM col,
  TEXTURE_NUM pat, FULL_LENGTH xstart, FULL_LENGTH xstop, FULL_LENGTH ymk)
{}

static void SVG_NullCoordTranslate(FULL_LENGTH xdist, FULL_LENGTH ydist)
{}

static void SVG_NullCoordRotate(FULL_LENGTH amount)
{}

static void SVG_NullCoordScale(float hfactor, float vfactor)
{}

static void SVG_NullCoordHMirror(void)
{}

static void SVG_NullCoordVMirror(void)
{}

static void SVG_NullSaveGraphicState(OBJECT x)
{}

static void SVG_NullRestoreGraphicState(void)
{}

static void SVG_NullPrintGraphicObject(OBJECT x)
{}

static void SVG_NullDefineGraphicNames(OBJECT x)
{}

static void SVG_NullSaveTranslateDefineSave(OBJECT x, FULL_LENGTH xdist,
  FULL_LENGTH ydist)
{}

static void SVG_NullPrintGraphicInclude(OBJECT x, FULL_LENGTH colmark,
  FULL_LENGTH rowmark)
{}

static void SVG_NullLinkSource(OBJECT name, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{}

static void SVG_NullLinkDest(OBJECT name, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{}

static void SVG_NullLinkURL(OBJECT url, FULL_LENGTH llx, FULL_LENGTH lly,
  FULL_LENGTH urx, FULL_LENGTH ury)
{}

static void SVG_NullLinkCheck(void)
{}

static struct back_end_rec svg_null_back = {
  SVG,                                  /* the code number of the back end   */
  STR_SVG,                              /* string name of the back end       */
  TRUE,                                 /* TRUE if @Scale is available       */
  TRUE,                                 /* TRUE if @Rotate is available      */
  TRUE,                                 /* TRUE if @HMirror, @VMirror avail  */
  TRUE,                                 /* TRUE if @Graphic is available     */
  TRUE,                                 /* TRUE if @IncludeGraphic is avail. */
  FALSE,                                /* TRUE if @PlainGraphic is avail.   */
  TRUE,                                 /* TRUE if fractional spacing avail. */
  TRUE,                                 /* TRUE if actual font metrics used  */
  TRUE,                                 /* TRUE if colour is available       */
  SVG_NullPrintInitialize,
  SVG_PrintLength,
  SVG_NullPrintPageSetupForFont,
  SVG_NullPrintPageResourceForFont,
  SVG_NullPrintMapping,
  SVG_NullPrintBeforeFirstPage,
  SVG_NullPrintBetweenPages,
  SVG_NullPrintAfterLastPage,
  SVG_NullPrintWord,
  SVG_NullPrintPlainGraphic,
  SVG_NullPrintUnderline,
  SVG_NullCoordTranslate,
  SVG_NullCoordRotate,
  SVG_NullCoordScale,
  SVG_NullCoordHMirror,
  SVG_NullCoordVMirror,
  SVG_NullSaveGraphicState,
  SVG_NullRestoreGraphicState,
  SVG_NullPrintGraphicObject,
  SVG_NullDefineGraphicNames,
  SVG_NullSaveTranslateDefineSave,
  SVG_NullPrintGraphicInclude,
  SVG_NullLinkSource,
  SVG_NullLinkDest,
  SVG_NullLinkURL,
  SVG_NullLinkCheck,
};

BACK_END SVG_NullBackEnd = &svg_null_back;
