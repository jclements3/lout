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
#define SVG_PS_DICT_ENTRIES   256
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
  if( out_fp != NULL )
  {
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
  { NULL,             0      }
};


static unsigned int svg_glyph_to_unicode(const char *name)
{
  int i;
  if( name == NULL || name[0] == '\0' )
    return 0;
  if( strcmp(name, "-none-") == 0 )
    return 0;
  for( i = 0; svg_glyph_table[i].name != NULL; i++ )
  {
    if( strcmp(svg_glyph_table[i].name, name) == 0 )
      return svg_glyph_table[i].cp;
  }
  return 0;
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
/*  static void svg_emit_word_text(FONT_NUM fnum, FULL_CHAR *s, OBJECT x)    */
/*                                                                           */
/*  Emit the text of a word by mapping each byte through the font's LCM     */
/*  vector to a glyph name, then to Unicode.  Falls back to direct          */
/*  Latin-1 -> UTF-8 if no mapping exists.                                  */
/*                                                                           */
/*****************************************************************************/

static void svg_emit_word_text(FONT_NUM fnum, FULL_CHAR *s, OBJECT x)
{
  MAPPING m;
  MAP_VEC mv;
  const FULL_CHAR *p;
  unsigned int c, cp;
  OBJECT name_obj;
  FULL_CHAR *gname;

  if( s == NULL )
    return;

  mv = NULL;
  m = FontMapping(fnum, &fpos(x));
  if( m != 0 && MapTable != NULL && MapTable[m] != NULL )
    mv = MapTable[m];

  for( p = s; *p != '\0'; p++ )
  {
    c = (unsigned int) *p;
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
    {
      /* fallback: Latin-1 -> UTF-8 */
      cp = c;
    }
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

static void SVG_PrintWord(OBJECT x, int hpos, int vpos)
{
  FONT_NUM fnum;
  FULL_LENGTH fsize, fhxh;
  FULL_CHAR *fname, *fface;
  double x_pt, y_pt, size_pt;
  char colourbuf[32];
  const char *colour_str;
  BOOLEAN is_bold, is_italic;

  if( out_fp == NULL || !page_open )
    return;

  fnum   = word_font(x);
  fsize  = FontSize(fnum, x);
  fhxh   = FontHalfXHeight(fnum);
  fname  = FontFamily(fnum);
  fface  = FontFace(fnum);

  /* Lout vpos marks the x-height midline; shift down by half-xheight to    */
  /* reach the baseline.  Coordinates are in the Lout (bottom-left) frame   */
  /* of the enclosing flipped group: text baseline at y = vpos - fhxh.      */
  x_pt    = (double) hpos          / PT;
  y_pt    = (double) (vpos - fhxh) / PT;
  size_pt = (double) fsize         / PT;

  colour_str = svg_colour_rgb(word_colour(x), colourbuf);

  is_bold = FALSE;
  is_italic = FALSE;
  if( fface != NULL )
  {
    if( strstr((const char *) fface, "Bold") != NULL )
      is_bold = TRUE;
    if( strstr((const char *) fface, "Italic") != NULL ||
        strstr((const char *) fface, "Slope") != NULL ||
        strstr((const char *) fface, "Oblique") != NULL )
      is_italic = TRUE;
  }

  /* Counter-flip wrapper so the glyph is upright inside the page-level Y-  */
  /* flip group.  The text origin lives at (x_pt, y_pt) in flipped coords; */
  /* after scale(1,-1) the local frame is back to top-left, so x="0" y="0" */
  /* anchors the baseline.                                                  */
  fprintf(out_fp,
    "<g transform=\"translate(%.3f,%.3f) scale(1,-1)\">",
    x_pt, y_pt);
  fprintf(out_fp,
    "<text x=\"0\" y=\"0\" font-family=\"%s\" font-size=\"%.3f\"",
    fname == NULL ? "serif" : (const char *) fname,
    size_pt);
  if( is_bold )
    fputs(" font-weight=\"bold\"", out_fp);
  if( is_italic )
    fputs(" font-style=\"italic\"", out_fp);
  if( colour_str != NULL )
    fprintf(out_fp, " fill=\"%s\"", colour_str);
  fputc('>', out_fp);
  svg_emit_word_text(fnum, string(x), x);
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
} svg_gstate;


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
  char     *name;               /* arena-owned */
  svg_value value;
  int       used;
} svg_dict_entry;

typedef struct svg_dict {
  svg_dict_entry entries[SVG_PS_DICT_ENTRIES];
  int            in_use;
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

static void svg_dict_clear(svg_dict *d)
{
  int i;
  d->in_use = 1;
  for( i = 0; i < SVG_PS_DICT_ENTRIES; i++ )
  {
    d->entries[i].used = 0;
    d->entries[i].name = NULL;
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

static void svg_dict_def(int did, const char *name, const svg_value *v)
{
  int i, slot, first_free;
  svg_dict *d;
  if( did < 0 || did >= SVG_PS_DICT_POOL )
    return;
  d = &svg_dict_pool[did];
  first_free = -1;
  for( i = 0; i < SVG_PS_DICT_ENTRIES; i++ )
  {
    if( !d->entries[i].used )
    {
      if( first_free < 0 ) first_free = i;
      continue;
    }
    if( d->entries[i].name != NULL && strcmp(d->entries[i].name, name) == 0 )
    {
      d->entries[i].value = *v;
      return;
    }
  }
  if( first_free < 0 )
    return;
  slot = first_free;
  d->entries[slot].used = 1;
  d->entries[slot].name = svg_arena_strdup(name, (int) strlen(name));
  d->entries[slot].value = *v;
}

static int svg_dict_lookup(int did, const char *name, svg_value *out)
{
  int i;
  svg_dict *d;
  if( did < 0 || did >= SVG_PS_DICT_POOL )
    return 0;
  d = &svg_dict_pool[did];
  for( i = 0; i < SVG_PS_DICT_ENTRIES; i++ )
  {
    if( d->entries[i].used && d->entries[i].name != NULL &&
        strcmp(d->entries[i].name, name) == 0 )
    {
      *out = d->entries[i].value;
      return 1;
    }
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
    fprintf(out_fp, " fill=\"%s\"", col);
  }
  else
    fputs(" fill=\"none\"", out_fp);
  if( do_stroke )
  {
    const char *col = g->stroke_rgb[0] != '\0' ? g->stroke_rgb : "currentColor";
    fprintf(out_fp, " stroke=\"%s\"", col);
    if( g->line_width > 0.0 )
      fprintf(out_fp, " stroke-width=\"%.3f\"", g->line_width);
    if( g->dasharray[0] != '\0' )
      fprintf(out_fp, " stroke-dasharray=\"%s\"", g->dasharray);
    fputs(" stroke-linecap=\"butt\" stroke-linejoin=\"miter\"", out_fp);
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
/*  svg_ps_exec_op - look up `name` as a built-in operator and execute it.   */
/*  Returns 1 if handled, 0 if not.                                          */
/*                                                                           */
/*****************************************************************************/

static int svg_ps_exec_op(svg_ps_state *s, const char *name)
{
  double a, b, c, d, e, f;
  svg_value va, vb;

  /* drawing ops */
  if( strcmp(name, "newpath") == 0 )
  {
    s->path[0] = '\0';
    s->plen = 0;
    s->had_geom = FALSE;
    s->have_cp = FALSE;
    s->last_pt_valid = FALSE;
    return 1;
  }
  if( strcmp(name, "moveto") == 0 )
  {
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_moveto(s, a, b);
    return 1;
  }
  if( strcmp(name, "lineto") == 0 )
  {
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_lineto(s, a, b);
    return 1;
  }
  if( strcmp(name, "rlineto") == 0 )
  {
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_lineto(s, s->cur_x + a, s->cur_y + b);
    return 1;
  }
  if( strcmp(name, "rmoveto") == 0 )
  {
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_moveto(s, s->cur_x + a, s->cur_y + b);
    return 1;
  }
  if( strcmp(name, "curveto") == 0 )
  {
    f = svg_ps_pop(s); e = svg_ps_pop(s);
    d = svg_ps_pop(s); c = svg_ps_pop(s);
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_curveto(s, a, b, c, d, e, f);
    return 1;
  }
  if( strcmp(name, "rcurveto") == 0 )
  {
    double cx, cy;
    f = svg_ps_pop(s); e = svg_ps_pop(s);
    d = svg_ps_pop(s); c = svg_ps_pop(s);
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    cx = s->cur_x; cy = s->cur_y;
    svg_ps_curveto(s, cx + a, cy + b, cx + c, cy + d, cx + e, cy + f);
    return 1;
  }
  if( strcmp(name, "closepath") == 0 )
  {
    svg_ps_closepath(s);
    return 1;
  }
  if( strcmp(name, "arc") == 0 || strcmp(name, "arcn") == 0 )
  {
    double cx, cy, r, a1, a2;
    a2 = svg_ps_pop(s); a1 = svg_ps_pop(s);
    r  = svg_ps_pop(s);
    cy = svg_ps_pop(s); cx = svg_ps_pop(s);
    svg_ps_arc(s, cx, cy, r, a1, a2, strcmp(name, "arc") == 0 ? 1 : 0);
    return 1;
  }
  if( strcmp(name, "stroke") == 0 )
  {
    svg_ps_emit_path(s, 1, 0);
    return 1;
  }
  if( strcmp(name, "fill") == 0 || strcmp(name, "eofill") == 0 )
  {
    svg_ps_emit_path(s, 0, 1);
    return 1;
  }
  if( strcmp(name, "setrgbcolor") == 0 || strcmp(name, "LoutSetRGBColor") == 0 )
  {
    c = svg_ps_pop(s); b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_set_rgb(s, a, b, c);
    return 1;
  }
  if( strcmp(name, "setgray") == 0 || strcmp(name, "LoutSetGray") == 0 )
  {
    a = svg_ps_pop(s);
    svg_ps_set_rgb(s, a, a, a);
    return 1;
  }
  if( strcmp(name, "sethsbcolor") == 0 || strcmp(name, "LoutSetHSBColor") == 0 )
  {
    (void) svg_ps_pop(s); (void) svg_ps_pop(s); (void) svg_ps_pop(s);
    return 1;
  }
  if( strcmp(name, "setcmykcolor") == 0 || strcmp(name, "LoutSetCMYKColor") == 0 )
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
  if( strcmp(name, "setlinewidth") == 0 )
  {
    a = svg_ps_pop(s);
    s->gs[s->gs_top].line_width = a / (double) PT;
    return 1;
  }
  if( strcmp(name, "setlinecap") == 0 ||
      strcmp(name, "setlinejoin") == 0 ||
      strcmp(name, "setmiterlimit") == 0 )
  {
    (void) svg_ps_pop(s);
    return 1;
  }
  if( strcmp(name, "setdash") == 0 )
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
  if( strcmp(name, "gsave") == 0 )
  {
    if( s->gs_top + 1 < SVG_PS_GS_DEPTH )
    {
      s->gs[s->gs_top + 1] = s->gs[s->gs_top];
      s->gs_top++;
    }
    return 1;
  }
  if( strcmp(name, "grestore") == 0 )
  {
    if( s->gs_top > 0 ) s->gs_top--;
    return 1;
  }
  if( strcmp(name, "translate") == 0 )
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
  if( strcmp(name, "scale") == 0 )
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
  if( strcmp(name, "rotate") == 0 )
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
  if( strcmp(name, "concat") == 0 )
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
  if( strcmp(name, "transform") == 0 || strcmp(name, "dtransform") == 0 )
  {
    /* "x y transform"        -> xd yd using CTM                              */
    /* "x y matrix transform" -> xd yd using supplied matrix                  */
    /* dtransform: same but ignores tx/ty (delta transform). Reasonable      */
    /* approximation here: apply with tx=ty=0.                                */
    double x, y, xd, yd;
    double mtmp[6];
    const double *m_use;
    int dtrans = (strcmp(name, "dtransform") == 0);
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
  if( strcmp(name, "itransform") == 0 || strcmp(name, "idtransform") == 0 )
  {
    double xd, yd, x, y;
    double mtmp[6];
    const double *m_use;
    int dtrans = (strcmp(name, "idtransform") == 0);
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
  if( strcmp(name, "matrix") == 0 || strcmp(name, "identmatrix") == 0 )
  {
    /* matrix     : push identity 6-element matrix array                     */
    /* identmatrix: pop array, fill with identity, push back                 */
    if( strcmp(name, "identmatrix") == 0 )
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
  if( strcmp(name, "currentmatrix") == 0 || strcmp(name, "defaultmatrix") == 0 )
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
  if( strcmp(name, "setmatrix") == 0 )
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
  if( strcmp(name, "currentpoint") == 0 )
  {
    svg_ps_push_num(s, s->cur_x);
    svg_ps_push_num(s, s->cur_y);
    return 1;
  }
  if( strcmp(name, "clip") == 0 || strcmp(name, "showpage") == 0 ||
      strcmp(name, "show") == 0 || strcmp(name, "stringwidth") == 0 ||
      strcmp(name, "charpath") == 0 )
  {
    if( strcmp(name, "show") == 0 || strcmp(name, "charpath") == 0 )
      (void) svg_ps_pop_value(s);
    if( strcmp(name, "stringwidth") == 0 )
    {
      (void) svg_ps_pop_value(s);
      svg_ps_push_num(s, 0.0);
      svg_ps_push_num(s, 0.0);
    }
    /* Clipping with an empty current path masks all subsequent drawing in   */
    /* this gstate (and its gsave-descendants) until the matching grestore. */
    /* The @Diag prologue uses `newpath clip gsave` to suppress unwanted    */
    /* arrowhead nodes (e.g. the back-arrowhead when only forward is        */
    /* requested).  Honour this by flagging the current gstate; the path-  */
    /* emit primitive then drops paths drawn while the flag is set.        */
    if( strcmp(name, "clip") == 0 && !s->had_geom )
      s->gs[s->gs_top].clip_empty = 1;
    return 1;
  }

  /* Lout prologue named procedures - direct C implementations */
  if( strcmp(name, "LoutGraphic") == 0 )
  {
    svg_var_louts = svg_ps_pop(s);
    svg_var_loutv = svg_ps_pop(s);
    svg_var_loutf = svg_ps_pop(s);
    svg_var_ymark = svg_ps_pop(s);
    svg_var_xmark = svg_ps_pop(s);
    svg_var_ysize = svg_ps_pop(s);
    svg_var_xsize = svg_ps_pop(s);
    return 1;
  }
  if( strcmp(name, "LoutBox") == 0 )
  {
    svg_ps_moveto(s, 0.0, 0.0);
    svg_ps_lineto(s, svg_var_xsize, 0.0);
    svg_ps_lineto(s, svg_var_xsize, svg_var_ysize);
    svg_ps_lineto(s, 0.0, svg_var_ysize);
    svg_ps_closepath(s);
    return 1;
  }
  if( strcmp(name, "LoutRule") == 0 )
  {
    svg_ps_moveto(s, 0.0, 0.0);
    svg_ps_lineto(s, svg_var_xsize, 0.0);
    return 1;
  }
  if( strcmp(name, "LoutCurveBox") == 0 )
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
  if( strcmp(name, "LoutShadowBox") == 0 )
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
  if( strcmp(name, "LoutGr2") == 0 )
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
  if( strcmp(name, "LoutTextureSolid") == 0 ||
      strcmp(name, "save_cp") == 0 ||
      strcmp(name, "restore_cp") == 0 )
    return 1;
  if( strcmp(name, "LoutSetTexture") == 0 )
  {
    /* PS: { pop } (no-texture build) or texture-stack manipulation.  Both    */
    /* variants consume exactly one operand.  An earlier no-op handler left   */
    /* the argument on the stack -- across many @Graphic invocations the     */
    /* leftover operand accumulated and eventually masked the connector     */
    /* outline/dashlength arguments of ldiagnodeend/ldiaglinkend, dropping  */
    /* the thin (0.48) connector strokes in late pages of the user guide.   */
    (void) svg_ps_pop_value(s);
    return 1;
  }
  if( strcmp(name, "LoutMakeTexture") == 0 )
  {
    /* PS: consumes 11 operands and pushes one (a pattern or null).          */
    int i;
    svg_value out;
    for( i = 0; i < 11; i++ )
      (void) svg_ps_pop_value(s);
    out.kind = SVG_VK_NULL;
    out.num = 0.0; out.name = NULL; out.items = NULL; out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }

  /* dictionary ops */
  if( strcmp(name, "dict") == 0 )
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
  if( strcmp(name, "begin") == 0 )
  {
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_DICT && svg_dict_top + 1 < SVG_PS_DICT_STACK_DEPTH )
    {
      svg_dict_top++;
      svg_dict_stack[svg_dict_top] = va.dict_id;
    }
    return 1;
  }
  if( strcmp(name, "end") == 0 )
  {
    if( svg_dict_top > 0 )
    {
      int did = svg_dict_stack[svg_dict_top];
      svg_dict_top--;
      /* Reclaim anonymous `N dict begin ... end` dicts so the pool doesn't  */
      /* exhaust during long documents (each @Diag node creates one).        */
      svg_dict_try_free_anonymous(did, s);
    }
    return 1;
  }
  if( strcmp(name, "currentdict") == 0 )
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
  if( strcmp(name, "userdict") == 0 ||
      strcmp(name, "systemdict") == 0 ||
      strcmp(name, "globaldict") == 0 ||
      strcmp(name, "errordict") == 0 ||
      strcmp(name, "statusdict") == 0 ||
      strcmp(name, "$error") == 0 )
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
  if( strcmp(name, "def") == 0 )
  {
    vb = svg_ps_pop_value(s);  /* value */
    va = svg_ps_pop_value(s);  /* name */
    if( va.kind == SVG_VK_LITNAME && va.name != NULL )
      svg_dict_stack_def(va.name, &vb);
    return 1;
  }
  if( strcmp(name, "load") == 0 )
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
  if( strcmp(name, "where") == 0 )
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
  if( strcmp(name, "known") == 0 )
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
  if( strcmp(name, "exec") == 0 )
  {
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_PROC )
      svg_ps_exec_proc(s, &va);
    else
      svg_ps_exec_value(s, &va);
    return 1;
  }
  if( strcmp(name, "bind") == 0 )
    return 1;   /* no-op */
  if( strcmp(name, "cvx") == 0 )
  {
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
  }
  if( strcmp(name, "cvlit") == 0 )
  {
    if( s->top > 0 )
    {
      if( s->stack[s->top - 1].kind == SVG_VK_PROC )
        s->stack[s->top - 1].kind = SVG_VK_ARRAY;
      else if( s->stack[s->top - 1].kind == SVG_VK_NAME )
        s->stack[s->top - 1].kind = SVG_VK_LITNAME;
    }
    return 1;
  }
  if( strcmp(name, "type") == 0 )
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
  if( strcmp(name, "xcheck") == 0 )
  {
    va = svg_ps_pop_value(s);
    svg_ps_push_bool(s, va.kind == SVG_VK_PROC || va.kind == SVG_VK_NAME);
    return 1;
  }

  /* boolean predicates */
  if( strcmp(name, "true") == 0 )  { svg_ps_push_bool(s, 1); return 1; }
  if( strcmp(name, "false") == 0 ) { svg_ps_push_bool(s, 0); return 1; }
  if( strcmp(name, "null") == 0 )
  {
    svg_value out;
    out.kind = SVG_VK_NULL;
    out.num = 0.0; out.name = NULL; out.items = NULL; out.nitems = 0;
    out.dict_id = 0;
    svg_ps_push(s, &out);
    return 1;
  }

  if( strcmp(name, "eq") == 0 || strcmp(name, "ne") == 0 )
  {
    int eq = 0;
    vb = svg_ps_pop_value(s); va = svg_ps_pop_value(s);
    if( (va.kind == SVG_VK_NUM || va.kind == SVG_VK_BOOL) &&
        (vb.kind == SVG_VK_NUM || vb.kind == SVG_VK_BOOL) )
      eq = (va.num == vb.num);
    else if( (va.kind == SVG_VK_NAME || va.kind == SVG_VK_LITNAME ||
              va.kind == SVG_VK_STRING) &&
             (vb.kind == SVG_VK_NAME || vb.kind == SVG_VK_LITNAME ||
              vb.kind == SVG_VK_STRING) )
      eq = (va.name != NULL && vb.name != NULL &&
            strcmp(va.name, vb.name) == 0);
    else
      eq = (va.kind == vb.kind);
    if( strcmp(name, "ne") == 0 ) eq = !eq;
    svg_ps_push_bool(s, eq);
    return 1;
  }
  if( strcmp(name, "lt") == 0 || strcmp(name, "gt") == 0 ||
      strcmp(name, "le") == 0 || strcmp(name, "ge") == 0 )
  {
    int r;
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    if( strcmp(name, "lt") == 0 ) r = (a <  b);
    else if( strcmp(name, "gt") == 0 ) r = (a >  b);
    else if( strcmp(name, "le") == 0 ) r = (a <= b);
    else r = (a >= b);
    svg_ps_push_bool(s, r);
    return 1;
  }
  if( strcmp(name, "and") == 0 || strcmp(name, "or") == 0 ||
      strcmp(name, "xor") == 0 )
  {
    int ai, bi, r;
    b = svg_ps_pop(s); a = svg_ps_pop(s);
    ai = a != 0.0; bi = b != 0.0;
    if( strcmp(name, "and") == 0 ) r = ai & bi;
    else if( strcmp(name, "or") == 0 ) r = ai | bi;
    else r = ai ^ bi;
    svg_ps_push_bool(s, r);
    return 1;
  }
  if( strcmp(name, "not") == 0 )
  {
    a = svg_ps_pop(s);
    svg_ps_push_bool(s, a == 0.0);
    return 1;
  }

  /* arithmetic */
  if( strcmp(name, "add") == 0 ) { b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a + b); return 1; }
  if( strcmp(name, "sub") == 0 ) { b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a - b); return 1; }
  if( strcmp(name, "mul") == 0 ) { b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, a * b); return 1; }
  if( strcmp(name, "div") == 0 )
  { b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, b == 0.0 ? 0.0 : a / b); return 1; }
  if( strcmp(name, "idiv") == 0 )
  { b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_push_num(s, b == 0.0 ? 0.0 : (double)((long)a / (long)b));
    return 1; }
  if( strcmp(name, "mod") == 0 )
  { b = svg_ps_pop(s); a = svg_ps_pop(s);
    svg_ps_push_num(s, b == 0.0 ? 0.0 : (double)((long)a % (long)b));
    return 1; }
  if( strcmp(name, "neg") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, -a); return 1; }
  if( strcmp(name, "abs") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a < 0 ? -a : a); return 1; }
  if( strcmp(name, "sqrt") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a < 0 ? 0.0 : sqrt(a)); return 1; }
  if( strcmp(name, "sin") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, sin(a * SVG_PI / 180.0)); return 1; }
  if( strcmp(name, "cos") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, cos(a * SVG_PI / 180.0)); return 1; }
  if( strcmp(name, "atan") == 0 )
  { b = svg_ps_pop(s); a = svg_ps_pop(s);
    {
      double r;
      if( a == 0.0 && b == 0.0 ) r = 0.0;
      else r = atan2(a, b) * 180.0 / SVG_PI;
      if( r < 0.0 ) r += 360.0;
      svg_ps_push_num(s, r);
    }
    return 1; }
  if( strcmp(name, "exp") == 0 )
  { b = svg_ps_pop(s); a = svg_ps_pop(s); svg_ps_push_num(s, pow(a, b)); return 1; }
  if( strcmp(name, "ln") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, a <= 0 ? 0.0 : log(a)); return 1; }
  if( strcmp(name, "log") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, a <= 0 ? 0.0 : log10(a)); return 1; }
  if( strcmp(name, "truncate") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, a >= 0 ? floor(a) : ceil(a)); return 1; }
  if( strcmp(name, "floor") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, floor(a)); return 1; }
  if( strcmp(name, "ceiling") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, ceil(a)); return 1; }
  if( strcmp(name, "round") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, floor(a + 0.5)); return 1; }
  if( strcmp(name, "cvi") == 0 )
  { a = svg_ps_pop(s); svg_ps_push_num(s, (double)(long)a); return 1; }
  if( strcmp(name, "cvr") == 0 ) return 1;  /* numeric already */
  if( strcmp(name, "cvs") == 0 )
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
  if( strcmp(name, "cvn") == 0 )
  {
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_STRING )
    {
      va.kind = SVG_VK_LITNAME;
      svg_ps_push(s, &va);
    }
    else
      svg_ps_push(s, &va);
    return 1;
  }
  if( strcmp(name, "string") == 0 )
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
  if( strcmp(name, "pop") == 0 ) { (void) svg_ps_pop_value(s); return 1; }
  if( strcmp(name, "dup") == 0 )
  {
    if( s->top > 0 )
    {
      svg_value top = s->stack[s->top - 1];
      svg_ps_push(s, &top);
    }
    return 1;
  }
  if( strcmp(name, "exch") == 0 )
  {
    if( s->top >= 2 )
    {
      svg_value t = s->stack[s->top - 1];
      s->stack[s->top - 1] = s->stack[s->top - 2];
      s->stack[s->top - 2] = t;
    }
    return 1;
  }
  if( strcmp(name, "index") == 0 )
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
  if( strcmp(name, "copy") == 0 )
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
  if( strcmp(name, "roll") == 0 )
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
  if( strcmp(name, "clear") == 0 ) { s->top = 0; return 1; }
  if( strcmp(name, "count") == 0 ) { svg_ps_push_num(s, (double) s->top); return 1; }
  if( strcmp(name, "mark") == 0 )
  {
    svg_value m;
    m.kind = SVG_VK_MARK; m.num = 0.0; m.name = NULL;
    m.items = NULL; m.nitems = 0; m.dict_id = 0;
    svg_ps_push(s, &m);
    return 1;
  }
  if( strcmp(name, "cleartomark") == 0 )
  {
    int i;
    for( i = s->top - 1; i >= 0; i-- )
      if( s->stack[i].kind == SVG_VK_MARK ) { s->top = i; return 1; }
    s->top = 0;
    return 1;
  }
  if( strcmp(name, "counttomark") == 0 )
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
  if( strcmp(name, "]") == 0 )
  {
    svg_value arr;
    svg_collect_to_mark(s, &arr);
    svg_ps_push(s, &arr);
    return 1;
  }

  /* array ops */
  if( strcmp(name, "aload") == 0 )
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
  if( strcmp(name, "astore") == 0 )
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
  if( strcmp(name, "length") == 0 )
  {
    va = svg_ps_pop_value(s);
    if( va.kind == SVG_VK_ARRAY || va.kind == SVG_VK_PROC )
      svg_ps_push_num(s, (double) va.nitems);
    else if( va.kind == SVG_VK_STRING && va.name != NULL )
      svg_ps_push_num(s, (double) strlen(va.name));
    else
      svg_ps_push_num(s, 0.0);
    return 1;
  }
  if( strcmp(name, "get") == 0 )
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
  if( strcmp(name, "put") == 0 )
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
  if( strcmp(name, "putinterval") == 0 )
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
  if( strcmp(name, "search") == 0 )
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
  if( strcmp(name, "if") == 0 )
  {
    svg_value proc = svg_ps_pop_value(s);
    a = svg_ps_pop(s);
    if( a != 0.0 ) svg_ps_call(s, &proc);
    return 1;
  }
  if( strcmp(name, "ifelse") == 0 )
  {
    svg_value pf = svg_ps_pop_value(s);  /* false branch */
    svg_value pt = svg_ps_pop_value(s);  /* true branch */
    a = svg_ps_pop(s);
    if( a != 0.0 ) svg_ps_call(s, &pt);
    else            svg_ps_call(s, &pf);
    return 1;
  }
  if( strcmp(name, "for") == 0 )
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
  if( strcmp(name, "repeat") == 0 )
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
  if( strcmp(name, "loop") == 0 )
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
  if( strcmp(name, "forall") == 0 )
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
  if( strcmp(name, "exit") == 0 ) { svg_exit_flag = 1; return 1; }
  if( strcmp(name, "stop") == 0 ) { svg_stop_flag = 1; return 1; }
  if( strcmp(name, "stopped") == 0 )
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
  if( strcmp(name, "in") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * 1440.0); return 1; }
  if( strcmp(name, "cm") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * 566.929); return 1; }
  if( strcmp(name, "pt") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * 20.0); return 1; }
  if( strcmp(name, "em") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * 120.0); return 1; }
  if( strcmp(name, "sp") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_louts); return 1; }
  if( strcmp(name, "vs") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_loutv); return 1; }
  if( strcmp(name, "ft") == 0 ) { a = svg_ps_pop(s); svg_ps_push_num(s, a * svg_var_loutf); return 1; }
  if( strcmp(name, "dg") == 0 ) return 1;  /* identity */

  return 0;
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
      /* unknown: warn and skip */
      if( svg_warn_unknown_count < SVG_WARN_MAX )
      {
        svg_warn_unknown_count++;
        if( svg_warn_unknown_count == SVG_WARN_MAX )
          fprintf(stderr,
            "lout (SVG): further unknown PostScript operators suppressed\n");
        else
          fprintf(stderr,
            "lout (SVG): unknown PostScript operator '%s'\n", v->name);
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
/*****************************************************************************/

static void svg_ps_run(const char *buf, int n, svg_ps_state *s)
{
  static svg_ps_tok toks[SVG_PS_MAX_TOKENS];
  svg_value *vals;
  int ntoks, cur, nvals, i;

  ntoks = svg_ps_tokenise(buf, n, toks, SVG_PS_MAX_TOKENS);
  cur = 0;
  nvals = 0;
  vals = svg_parse_tokens(buf, toks, ntoks, &cur, 0, &nvals);

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

static void svg_psinterp_init(void)
{
  int i;
  svg_arena_free_all();
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
  cur_gr_loutf = 12 * PT;
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
/*****************************************************************************/

static void SVG_NullPrintInitialize(FILE *fp, BOOLEAN enc)
{}

static struct back_end_rec svg_null_back = {
  SVG,
  STR_SVG,
  TRUE,
  TRUE,
  TRUE,
  TRUE,
  TRUE,
  FALSE,
  TRUE,
  TRUE,
  TRUE,
  SVG_NullPrintInitialize,
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

BACK_END SVG_NullBackEnd = &svg_null_back;
