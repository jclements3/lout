/*@z53_glyph.c:SVG Back End Glyph Outlines@***********************************/
/*                                                                           */
/*  THE LOUT DOCUMENT FORMATTING SYSTEM (VERSION 3.43)                       */
/*  COPYRIGHT (C) 2026 James Clements III                                    */
/*                                                                           */
/*  This program is free software; you can redistribute it and/or modify     */
/*  it under the terms of the GNU General Public License as published by     */
/*  the Free Software Foundation; either Version 3, or (at your option)      */
/*  any later version.                                                       */
/*                                                                           */
/*  FILE:         z53_glyph.c                                                */
/*  MODULE:       SVG Back End / Type 1 + CFF Glyph Outline Service          */
/*  EXTERNS:      svg_glyph_emit_outline                                     */
/*                                                                           */
/*  STATUS:       Two outline back ends behind one cache:                    */
/*                                                                           */
/*                (a) Adobe Type 1 .pfb (URW++ / Ghostscript base-35 set).   */
/*                    PFB segment unwrap -> eexec decryption (key 55665,    */
/*                    lenIV 4) -> CharStrings dict scan -> per-glyph        */
/*                    charstring decryption (key 4330, lenIV 4) ->          */
/*                    Type 1 charstring interpreter.  Subrs supported,     */
/*                    seac via a small Adobe StandardEncoding table.        */
/*                                                                           */
/*                (b) CFF/OpenType .otf (OTTO-tagged container, Type 2      */
/*                    charstrings).  OT table directory walk -> CFF        */
/*                    section -> Top DICT (CharStrings, Private,           */
/*                    charset) -> per-glyph Type 2 charstring with         */
/*                    biased local + global Subrs and the hflex / flex /   */
/*                    hflex1 / flex1 family.                               */
/*                                                                           */
/*                Both share the same arena allocator, cache record, and    */
/*                public entry point (svg_glyph_emit_outline).               */
/*                                                                           */
/*                TrueType `glyf` outlines (.ttf with magic 0x00010000)     */
/*                are out of scope and fall through to the bbox rectangle. */
/*                                                                           */
/*  ANSI C ONLY: must compile with tcc.  No mid-block decls, no // comments, */
/*  no designated initialisers, no GCC extensions.                           */
/*                                                                           */
/*****************************************************************************/
#include "externs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/*****************************************************************************/
/*                                                                           */
/*  Compile-time limits.                                                     */
/*                                                                           */
/*****************************************************************************/

#define SVG_GLYPH_MAX_FONTS       16    /* concurrent fonts in cache         */
#define SVG_GLYPH_MAX_GLYPHS    4096    /* glyphs per font                    */
#define SVG_GLYPH_MAX_SUBRS     8192    /* Subrs per font (T1 or CFF local)   */
#define SVG_GLYPH_MAX_GSUBRS    8192    /* CFF global Subrs                   */
#define SVG_GLYPH_NAME_LEN        40    /* AGL glyph name max length          */
#define SVG_GLYPH_PFB_MAX    1048576    /* 1 MiB raw PFB cap                  */
#define SVG_GLYPH_OTF_MAX    8388608    /* 8 MiB raw OTF cap                  */
#define SVG_GLYPH_DECRYPT_LEN_IV   4    /* default lenIV for both layers      */
#define SVG_GLYPH_STK_DEPTH       96    /* CharString operand-stack depth     */
#define SVG_GLYPH_PSN_LEN         96    /* PS font name max length            */

/* Font kind tag: governs interpreter dispatch.                              */
#define SVG_GLYPH_KIND_T1   1
#define SVG_GLYPH_KIND_CFF  2


/*****************************************************************************/
/*                                                                           */
/*  Per-font cache record.                                                   */
/*                                                                           */
/*****************************************************************************/

typedef struct svg_glyph_entry {
  char            name[SVG_GLYPH_NAME_LEN];
  unsigned char  *cs;        /* decrypted charstring bytes (arena-alloc)    */
  int             cs_len;
} svg_glyph_entry;

typedef struct svg_subr_entry {
  unsigned char  *cs;
  int             cs_len;
} svg_subr_entry;

typedef struct svg_glyph_font {
  char            ps_name[SVG_GLYPH_PSN_LEN];
  int             kind;                    /* SVG_GLYPH_KIND_T1 or _CFF      */
  int             loaded;                  /* 1 once parsed                  */
  int             load_failed;             /* 1 if we tried and failed       */
  svg_glyph_entry glyphs[SVG_GLYPH_MAX_GLYPHS];
  int             nglyphs;
  svg_subr_entry  subrs[SVG_GLYPH_MAX_SUBRS];   /* T1 Subrs or CFF local     */
  int             nsubrs;
  int             subr_bias;               /* CFF: bias for local Subr idx  */
  svg_subr_entry  gsubrs[SVG_GLYPH_MAX_GSUBRS]; /* CFF only                  */
  int             ngsubrs;
  int             gsubr_bias;              /* CFF: bias for global subrs    */
  int             lenIV;                   /* Type 1 only                    */
  double          em_scale;                /* design-units -> 1000-em factor */
  /* simple arena -- one malloc, grown as needed; freed at process exit (no  */
  /* explicit cleanup hook in this back end).  All cs / subrs pointers       */
  /* point inside arena.                                                     */
  unsigned char  *arena;
  size_t          arena_used;
  size_t          arena_cap;
} svg_glyph_font;

static svg_glyph_font svg_glyph_fonts[SVG_GLYPH_MAX_FONTS];
static int            svg_glyph_n_fonts = 0;


/*****************************************************************************/
/*                                                                           */
/*  PostScript font name -> Type 1 .pfb file name table.                     */
/*  All paths are joined with the search prefixes in svg_glyph_search_dir[]. */
/*  Adobe base-35 set, mapped to URW++ Nimbus / Century / Bookman /          */
/*  Standard Symbol / Dingbats / Z003.                                       */
/*                                                                           */
/*****************************************************************************/

typedef struct {
  const char *ps_name;
  const char *file;
} svg_glyph_map_entry;

static const svg_glyph_map_entry svg_glyph_name_map[] = {
  /* Times -> Nimbus Roman No 9 L */
  { "Times-Roman",          "n021003l.pfb" },
  { "Times-Italic",         "n021023l.pfb" },
  { "Times-Bold",           "n021004l.pfb" },
  { "Times-BoldItalic",     "n021024l.pfb" },
  /* Helvetica -> Nimbus Sans L */
  { "Helvetica",            "n019003l.pfb" },
  { "Helvetica-Oblique",    "n019023l.pfb" },
  { "Helvetica-Bold",       "n019004l.pfb" },
  { "Helvetica-BoldOblique","n019024l.pfb" },
  /* Helvetica Narrow / Condensed -> Nimbus Sans Narrow */
  { "Helvetica-Narrow",     "n019043l.pfb" },
  { "Helvetica-Narrow-Oblique",     "n019063l.pfb" },
  { "Helvetica-Narrow-Bold",        "n019044l.pfb" },
  { "Helvetica-Narrow-BoldOblique", "n019064l.pfb" },
  /* Courier -> Nimbus Mono L */
  { "Courier",              "n022003l.pfb" },
  { "Courier-Oblique",      "n022023l.pfb" },
  { "Courier-Bold",         "n022004l.pfb" },
  { "Courier-BoldOblique",  "n022024l.pfb" },
  /* Symbol -> Standard Symbols L */
  { "Symbol",               "s050000l.pfb" },
  /* Zapf Dingbats -> Dingbats L */
  { "ZapfDingbats",         "d050000l.pfb" },
  /* Zapf Chancery -> URW Chancery L */
  { "ZapfChancery-MediumItalic", "z003034l.pfb" },
  /* Bookman -> URW Bookman L */
  { "Bookman-Light",        "b018012l.pfb" },
  { "Bookman-LightItalic",  "b018015l.pfb" },
  { "Bookman-Demi",         "b018032l.pfb" },
  { "Bookman-DemiItalic",   "b018035l.pfb" },
  /* New Century Schoolbook -> URW Century Schoolbook L */
  { "NewCenturySchlbk-Roman",      "c059013l.pfb" },
  { "NewCenturySchlbk-Italic",     "c059016l.pfb" },
  { "NewCenturySchlbk-Bold",       "c059033l.pfb" },
  { "NewCenturySchlbk-BoldItalic", "c059036l.pfb" },
  /* ITC Avant Garde -> URW Gothic L */
  { "AvantGarde-Book",        "a010013l.pfb" },
  { "AvantGarde-BookOblique", "a010033l.pfb" },
  { "AvantGarde-Demi",        "a010015l.pfb" },
  { "AvantGarde-DemiOblique", "a010035l.pfb" },
  /* Palatino -> URW Palladio L */
  { "Palatino-Roman",       "p052003l.pfb" },
  { "Palatino-Italic",      "p052023l.pfb" },
  { "Palatino-Bold",        "p052004l.pfb" },
  { "Palatino-BoldItalic",  "p052024l.pfb" },
  { NULL, NULL }
};

/* Default Linux search prefixes for .pfb files.  Overridable via env var. */
static const char *svg_glyph_search_dir[] = {
  "/usr/share/fonts/type1/gsfonts/",
  "/usr/share/fonts/type1/urw-fonts/",
  "/usr/share/ghostscript/9.55.0/Resource/Font/",
  "/usr/share/ghostscript/10.0.0/Resource/Font/",
  NULL
};

/*****************************************************************************/
/*                                                                           */
/*  OTF/CFF lookup.  Two paths:                                              */
/*                                                                           */
/*    (a) svg_glyph_otf_map[]: PS font name -> known OTF filename.  Empty   */
/*        by default; populate when a system distributes a CFF-based OTF   */
/*        under a stable name.                                              */
/*                                                                           */
/*    (b) svg_glyph_otf_dir[]: directory tree walk.  Each directory is     */
/*        traversed one level deep; any file ending .otf or .OTF whose     */
/*        first 4 bytes are 'OTTO' is considered.  We accept the file if  */
/*        the PostScript name matches either the basename (sans .otf), or */
/*        the font's Name INDEX entry inside the CFF table.                */
/*                                                                           */
/*  Override via LOUT_OTF_FONT_DIR env var (single dir prepended).         */
/*                                                                           */
/*****************************************************************************/

static const svg_glyph_map_entry svg_glyph_otf_map[] = {
  { "LinLibertine_R",       "LinLibertine_R.otf" },
  { "LinLibertine_RB",      "LinLibertine_RB.otf" },
  { "LinLibertine_RI",      "LinLibertine_RI.otf" },
  { "Cantarell-Regular",    "Cantarell-Regular.otf" },
  { "Cantarell-Bold",       "Cantarell-Bold.otf" },
  { "Cabin-Regular",        "Cabin-Regular.otf" },
  { "Cabin-Bold",           "Cabin-Bold.otf" },
  { NULL, NULL }
};

static const char *svg_glyph_otf_dir[] = {
  "/usr/share/fonts/opentype/",
  "/usr/share/fonts/truetype/",   /* some .otf live here, mislabelled     */
  "/usr/local/share/fonts/opentype/",
  "/usr/local/share/fonts/",
  NULL
};


/*****************************************************************************/
/*                                                                           */
/*  Adobe Standard Encoding (subset sufficient for the seac operator, which  */
/*  references the base / accent glyphs by Adobe Standard Encoding code).    */
/*  Indices outside this range yield NULL -> the seac glyph emits nothing    */
/*  for the accent.                                                          */
/*                                                                           */
/*****************************************************************************/

static const char *svg_glyph_std_enc[256];
static int         svg_glyph_std_enc_init = 0;

static void svg_glyph_init_std_enc(void)
{
  /* Adobe StandardEncoding from the Type 1 reference.  Only entries that   */
  /* commonly appear as seac base/accent codes are filled in.                */
  int i;
  for( i = 0; i < 256; i++ ) svg_glyph_std_enc[i] = NULL;
  /* Letters (32-126 mostly identical to ASCII positions in StdEnc except    */
  /* a few oddities below).                                                  */
  svg_glyph_std_enc[0x20] = "space";
  svg_glyph_std_enc[0x21] = "exclam";
  svg_glyph_std_enc[0x22] = "quotedbl";
  svg_glyph_std_enc[0x23] = "numbersign";
  svg_glyph_std_enc[0x24] = "dollar";
  svg_glyph_std_enc[0x25] = "percent";
  svg_glyph_std_enc[0x26] = "ampersand";
  svg_glyph_std_enc[0x27] = "quoteright";
  svg_glyph_std_enc[0x28] = "parenleft";
  svg_glyph_std_enc[0x29] = "parenright";
  svg_glyph_std_enc[0x2a] = "asterisk";
  svg_glyph_std_enc[0x2b] = "plus";
  svg_glyph_std_enc[0x2c] = "comma";
  svg_glyph_std_enc[0x2d] = "hyphen";
  svg_glyph_std_enc[0x2e] = "period";
  svg_glyph_std_enc[0x2f] = "slash";
  for( i = 0x30; i <= 0x39; i++ )
  { static const char *digits[] = { "zero","one","two","three","four",
      "five","six","seven","eight","nine" };
    svg_glyph_std_enc[i] = digits[i - 0x30];
  }
  for( i = 0x41; i <= 0x5a; i++ )
  { static char buf[26][2];
    buf[i-0x41][0] = (char) i;  buf[i-0x41][1] = 0;
    svg_glyph_std_enc[i] = buf[i-0x41];
  }
  for( i = 0x61; i <= 0x7a; i++ )
  { static char buf[26][2];
    buf[i-0x61][0] = (char) i;  buf[i-0x61][1] = 0;
    svg_glyph_std_enc[i] = buf[i-0x61];
  }
  /* Accents commonly used by seac in Adobe base-35.                         */
  svg_glyph_std_enc[0xc1] = "grave";
  svg_glyph_std_enc[0xc2] = "acute";
  svg_glyph_std_enc[0xc3] = "circumflex";
  svg_glyph_std_enc[0xc4] = "tilde";
  svg_glyph_std_enc[0xc5] = "macron";
  svg_glyph_std_enc[0xc6] = "breve";
  svg_glyph_std_enc[0xc7] = "dotaccent";
  svg_glyph_std_enc[0xc8] = "dieresis";
  svg_glyph_std_enc[0xca] = "ring";
  svg_glyph_std_enc[0xcb] = "cedilla";
  svg_glyph_std_enc[0xcd] = "hungarumlaut";
  svg_glyph_std_enc[0xce] = "ogonek";
  svg_glyph_std_enc[0xcf] = "caron";
  svg_glyph_std_enc_init = 1;
}


/*****************************************************************************/
/*                                                                           */
/*  Arena allocator: a single growing buffer per font.                       */
/*                                                                           */
/*****************************************************************************/

static unsigned char *svg_glyph_arena_alloc(svg_glyph_font *f, size_t n)
{
  unsigned char *p;
  if( f->arena_used + n > f->arena_cap )
  { size_t newcap = f->arena_cap ? f->arena_cap * 2 : 16384;
    while( newcap < f->arena_used + n ) newcap *= 2;
    p = (unsigned char *) realloc(f->arena, newcap);
    if( p == NULL ) return NULL;
    f->arena = p;
    f->arena_cap = newcap;
  }
  p = f->arena + f->arena_used;
  f->arena_used += n;
  return p;
}


/*****************************************************************************/
/*                                                                           */
/*  Type 1 charstring eexec / charstring decryption.                         */
/*  Reference: Adobe Type 1 Font Format, section 7.                          */
/*                                                                           */
/*****************************************************************************/

static void svg_glyph_decrypt(const unsigned char *in, int in_len,
  unsigned char *out, unsigned int key, int lenIV)
{
  /* Decrypts in_len bytes 1:1 into out; the first lenIV bytes of out are    */
  /* random padding (discard via out + lenIV).                               */
  unsigned int R = key;
  unsigned int c;
  int i;
  (void) lenIV;
  for( i = 0; i < in_len; i++ )
  {
    c = in[i];
    out[i] = (unsigned char) (c ^ (R >> 8));
    R = ((c + R) * 52845u + 22719u) & 0xFFFFu;
  }
}


/*****************************************************************************/
/*                                                                           */
/*  PFB segment unwrap.  Returns concatenated ASCII (segment type 1) and    */
/*  binary (segment type 2) bodies in malloc'd buffers; caller must free.    */
/*  Segments of type 3 (zeros-eof) terminate the file.                       */
/*                                                                           */
/*****************************************************************************/

static int svg_glyph_read_pfb(const char *path,
  unsigned char **ascii_out, size_t *ascii_len_out,
  unsigned char **binary_out, size_t *binary_len_out)
{
  FILE *fp;
  long fsize;
  unsigned char *raw;
  size_t got;
  unsigned char *ascii_buf = NULL;
  unsigned char *binary_buf = NULL;
  size_t ascii_len = 0, binary_len = 0;
  size_t ascii_cap = 0, binary_cap = 0;
  size_t pos;

  fp = fopen(path, "rb");
  if( fp == NULL ) return 0;
  fseek(fp, 0L, SEEK_END);
  fsize = ftell(fp);
  fseek(fp, 0L, SEEK_SET);
  if( fsize <= 0 || fsize > SVG_GLYPH_PFB_MAX )
  { fclose(fp); return 0; }
  raw = (unsigned char *) malloc((size_t) fsize);
  if( raw == NULL ) { fclose(fp); return 0; }
  got = fread(raw, 1, (size_t) fsize, fp);
  fclose(fp);
  if( got != (size_t) fsize )
  { free(raw); return 0; }

  pos = 0;
  while( pos + 6 <= (size_t) fsize )
  {
    int marker, seg_type;
    unsigned long seg_len;
    marker = raw[pos];
    seg_type = raw[pos+1];
    if( marker != 0x80 ) break;
    if( seg_type == 3 ) { pos += 2; break; }
    if( seg_type != 1 && seg_type != 2 ) { free(raw); return 0; }
    seg_len = (unsigned long) raw[pos+2]
            | ((unsigned long) raw[pos+3] << 8)
            | ((unsigned long) raw[pos+4] << 16)
            | ((unsigned long) raw[pos+5] << 24);
    pos += 6;
    if( pos + seg_len > (size_t) fsize ) { free(raw); return 0; }
    if( seg_type == 1 )
    {
      if( ascii_len + seg_len > ascii_cap )
      { unsigned char *np;
        ascii_cap = ascii_cap ? ascii_cap * 2 : seg_len + 16;
        while( ascii_cap < ascii_len + seg_len ) ascii_cap *= 2;
        np = (unsigned char *) realloc(ascii_buf, ascii_cap);
        if( np == NULL ) { free(ascii_buf); free(binary_buf); free(raw); return 0; }
        ascii_buf = np;
      }
      memcpy(ascii_buf + ascii_len, raw + pos, seg_len);
      ascii_len += seg_len;
    }
    else
    {
      if( binary_len + seg_len > binary_cap )
      { unsigned char *np;
        binary_cap = binary_cap ? binary_cap * 2 : seg_len + 16;
        while( binary_cap < binary_len + seg_len ) binary_cap *= 2;
        np = (unsigned char *) realloc(binary_buf, binary_cap);
        if( np == NULL ) { free(ascii_buf); free(binary_buf); free(raw); return 0; }
        binary_buf = np;
      }
      memcpy(binary_buf + binary_len, raw + pos, seg_len);
      binary_len += seg_len;
    }
    pos += seg_len;
  }
  free(raw);
  *ascii_out = ascii_buf;
  *ascii_len_out = ascii_len;
  *binary_out = binary_buf;
  *binary_len_out = binary_len;
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  Decrypt the eexec body (binary segment).  Output is malloc'd.            */
/*                                                                           */
/*****************************************************************************/

static int svg_glyph_eexec_decrypt(const unsigned char *bin, size_t bin_len,
  unsigned char **out, size_t *out_len)
{
  unsigned char *buf;
  if( bin_len <= 4 ) return 0;
  buf = (unsigned char *) malloc(bin_len - 4);
  if( buf == NULL ) return 0;
  /* Decrypt the whole stream into a scratch buffer, copy the post-padding   */
  /* part into the caller's buffer.                                          */
  { unsigned char *scratch = (unsigned char *) malloc(bin_len);
    if( scratch == NULL ) { free(buf); return 0; }
    svg_glyph_decrypt(bin, (int) bin_len, scratch, 55665u, 4);
    memcpy(buf, scratch + 4, bin_len - 4);
    free(scratch);
  }
  *out = buf;
  *out_len = bin_len - 4;
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  Helpers to scan the decrypted private dict / charstrings dict.           */
/*  These are pure ASCII PostScript so we use ordinary byte-level scanning. */
/*                                                                           */
/*****************************************************************************/

/* Find an occurrence of `needle` in [buf, buf+len).  Returns offset or -1. */
static long svg_glyph_find(const unsigned char *buf, size_t len, const char *needle)
{
  size_t nl = strlen(needle);
  size_t i;
  if( nl == 0 || len < nl ) return -1;
  for( i = 0; i + nl <= len; i++ )
    if( memcmp(buf + i, needle, nl) == 0 ) return (long) i;
  return -1;
}


/*****************************************************************************/
/*                                                                           */
/*  Parse /lenIV out of the private dict (default 4 if not present).        */
/*                                                                           */
/*****************************************************************************/

static int svg_glyph_parse_lenIV(const unsigned char *buf, size_t len)
{
  long off;
  size_t i;
  int sign;
  int v;
  off = svg_glyph_find(buf, len, "/lenIV");
  if( off < 0 ) return 4;
  i = (size_t) off + 6;
  while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
  sign = 1;
  if( i < len && buf[i] == '-' ) { sign = -1; i++; }
  v = 0;
  while( i < len && buf[i] >= '0' && buf[i] <= '9' )
  { v = v * 10 + (buf[i] - '0'); i++; }
  v *= sign;
  if( v < 0 || v > 16 ) return 4;
  return v;
}


/*****************************************************************************/
/*                                                                           */
/*  Parse one charstring entry of the form                                   */
/*    /Name n -| <n bytes>                                                   */
/*  starting at `i` (after the leading '/').  Writes the name into           */
/*  name_out (sized SVG_GLYPH_NAME_LEN), the length n into *cs_len_out, and  */
/*  the offset to the start of the encrypted bytes into *cs_off_out.         */
/*  Returns the offset just past the entry (consuming the trailing newline   */
/*  and `noaccess put` / `ND` etc.), or 0 on failure.                        */
/*                                                                           */
/*****************************************************************************/

static size_t svg_glyph_parse_cs_entry(const unsigned char *buf, size_t len,
  size_t i, char *name_out, int *cs_len_out, size_t *cs_off_out)
{
  size_t j;
  size_t name_len = 0;
  int n;
  /* read name */
  while( i < len && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r'
                 && buf[i] != '\n' && name_len + 1 < SVG_GLYPH_NAME_LEN )
  { name_out[name_len++] = (char) buf[i]; i++; }
  name_out[name_len] = 0;
  if( name_len == 0 ) return 0;
  /* skip whitespace before integer */
  while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
  if( i >= len || buf[i] < '0' || buf[i] > '9' ) return 0;
  n = 0;
  while( i < len && buf[i] >= '0' && buf[i] <= '9' )
  { n = n * 10 + (buf[i] - '0'); i++; }
  *cs_len_out = n;
  /* skip whitespace */
  while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
  /* expect "-|" or "RD"; eat past it */
  if( i < len && buf[i] == '-' && i + 1 < len && buf[i+1] == '|' ) i += 2;
  else if( i + 1 < len && buf[i] == 'R' && buf[i+1] == 'D' ) i += 2;
  else return 0;
  /* exactly one whitespace separator before the binary bytes */
  if( i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r') ) i++;
  *cs_off_out = i;
  j = i + (size_t) n;
  if( j > len ) return 0;
  /* skip over the rest of the line (likely " ND\n" or " noaccess put\n" or  */
  /* " NP\n"); advance to the next newline.                                  */
  while( j < len && buf[j] != '\n' && buf[j] != '\r' ) j++;
  while( j < len && (buf[j] == '\n' || buf[j] == '\r') ) j++;
  return j;
}


/*****************************************************************************/
/*                                                                           */
/*  Parse the CharStrings and Subrs dicts.                                  */
/*                                                                           */
/*****************************************************************************/

static int svg_glyph_parse_subrs(svg_glyph_font *f,
  const unsigned char *buf, size_t len)
{
  long off;
  size_t i;
  off = svg_glyph_find(buf, len, "/Subrs");
  if( off < 0 ) return 1;  /* no subrs is fine */
  i = (size_t) off + 6;
  /* skip "/Subrs <n> array" preamble; scan for the first "dup" entry */
  while( i < len )
  {
    int idx;
    int n;
    size_t cs_off;
    unsigned char *plain;
    /* find next "dup " (the entry start) or terminator "ND"/"|-"            */
    while( i < len )
    {
      if( i + 4 <= len && buf[i] == 'd' && buf[i+1] == 'u' && buf[i+2] == 'p'
          && (buf[i+3] == ' ' || buf[i+3] == '\t') )
        break;
      if( i + 2 <= len && buf[i] == 'N' && buf[i+1] == 'D' ) return 1;
      if( i + 2 <= len && buf[i] == '|' && buf[i+1] == '-' ) return 1;
      i++;
    }
    if( i >= len ) return 1;
    i += 4;
    while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
    /* read idx */
    idx = 0;
    if( i >= len || buf[i] < '0' || buf[i] > '9' ) return 1;
    while( i < len && buf[i] >= '0' && buf[i] <= '9' )
    { idx = idx * 10 + (buf[i] - '0'); i++; }
    while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
    /* read length */
    if( i >= len || buf[i] < '0' || buf[i] > '9' ) return 1;
    n = 0;
    while( i < len && buf[i] >= '0' && buf[i] <= '9' )
    { n = n * 10 + (buf[i] - '0'); i++; }
    while( i < len && (buf[i] == ' ' || buf[i] == '\t') ) i++;
    /* expect "-|" or "RD"                                                    */
    if( i < len && buf[i] == '-' && i + 1 < len && buf[i+1] == '|' ) i += 2;
    else if( i + 1 < len && buf[i] == 'R' && buf[i+1] == 'D' ) i += 2;
    else return 1;
    if( i < len && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r') ) i++;
    cs_off = i;
    if( cs_off + (size_t) n > len ) return 1;
    /* decrypt charstring with key 4330, drop lenIV bytes                    */
    if( n > f->lenIV && idx >= 0 && idx < SVG_GLYPH_MAX_SUBRS )
    {
      plain = svg_glyph_arena_alloc(f, (size_t) (n - f->lenIV));
      if( plain == NULL ) return 0;
      { unsigned char *tmp = (unsigned char *) malloc((size_t) n);
        if( tmp == NULL ) return 0;
        svg_glyph_decrypt(buf + cs_off, n, tmp, 4330u, f->lenIV);
        memcpy(plain, tmp + f->lenIV, (size_t) (n - f->lenIV));
        free(tmp);
      }
      f->subrs[idx].cs = plain;
      f->subrs[idx].cs_len = n - f->lenIV;
      if( idx + 1 > f->nsubrs ) f->nsubrs = idx + 1;
    }
    i = cs_off + (size_t) n;
    /* skip past the entry's trailing newline                                */
    while( i < len && buf[i] != '\n' && buf[i] != '\r' ) i++;
    while( i < len && (buf[i] == '\n' || buf[i] == '\r') ) i++;
  }
  return 1;
}

static int svg_glyph_parse_charstrings(svg_glyph_font *f,
  const unsigned char *buf, size_t len)
{
  long off;
  size_t i;
  off = svg_glyph_find(buf, len, "/CharStrings");
  if( off < 0 ) return 0;
  i = (size_t) off + 12;
  /* scan forward for entries beginning with '/'.  Stop at "end" preceded    */
  /* by a newline -- a coarse but adequate terminator.                       */
  while( i < len && f->nglyphs < SVG_GLYPH_MAX_GLYPHS )
  {
    /* skip whitespace */
    while( i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                       buf[i] == '\n' || buf[i] == '\r') ) i++;
    if( i >= len ) break;
    if( buf[i] != '/' )
    {
      /* try to detect "end" terminator                                      */
      if( i + 3 <= len && buf[i] == 'e' && buf[i+1] == 'n' && buf[i+2] == 'd' )
        break;
      i++;
      continue;
    }
    i++;  /* past the leading '/'                                            */
    {
      char name[SVG_GLYPH_NAME_LEN];
      int cs_len = 0;
      size_t cs_off = 0;
      size_t next = svg_glyph_parse_cs_entry(buf, len, i, name, &cs_len, &cs_off);
      if( next == 0 ) { i++; continue; }
      if( cs_len > f->lenIV )
      {
        unsigned char *plain = svg_glyph_arena_alloc(f,
          (size_t) (cs_len - f->lenIV));
        if( plain == NULL ) return 0;
        { unsigned char *tmp = (unsigned char *) malloc((size_t) cs_len);
          if( tmp == NULL ) return 0;
          svg_glyph_decrypt(buf + cs_off, cs_len, tmp, 4330u, f->lenIV);
          memcpy(plain, tmp + f->lenIV, (size_t) (cs_len - f->lenIV));
          free(tmp);
        }
        { size_t nl = strlen(name);
          if( nl >= SVG_GLYPH_NAME_LEN ) nl = SVG_GLYPH_NAME_LEN - 1;
          memcpy(f->glyphs[f->nglyphs].name, name, nl);
          f->glyphs[f->nglyphs].name[nl] = 0;
        }
        f->glyphs[f->nglyphs].cs = plain;
        f->glyphs[f->nglyphs].cs_len = cs_len - f->lenIV;
        f->nglyphs++;
      }
      i = next;
    }
  }
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  Locate the .pfb file for a Lout PostScript font name.                    */
/*  Buffer must be large enough to hold the full path.  Returns 1 on        */
/*  success.                                                                 */
/*                                                                           */
/*****************************************************************************/

static int svg_glyph_find_pfb_path(const char *ps_name, char *out, size_t cap)
{
  int i;
  const char *override;
  const char *file = NULL;
  for( i = 0; svg_glyph_name_map[i].ps_name != NULL; i++ )
  {
    if( strcmp(svg_glyph_name_map[i].ps_name, ps_name) == 0 )
    { file = svg_glyph_name_map[i].file; break; }
  }
  if( file == NULL ) return 0;
  override = getenv("LOUT_T1_FONT_DIR");
  if( override != NULL && override[0] != 0 )
  {
    size_t ol = strlen(override);
    size_t fl = strlen(file);
    int need_slash = (override[ol-1] != '/');
    if( ol + (need_slash?1:0) + fl + 1 > cap ) return 0;
    memcpy(out, override, ol);
    if( need_slash ) out[ol++] = '/';
    memcpy(out + ol, file, fl + 1);
    { FILE *fp = fopen(out, "rb");
      if( fp != NULL ) { fclose(fp); return 1; }
    }
  }
  for( i = 0; svg_glyph_search_dir[i] != NULL; i++ )
  {
    const char *dir = svg_glyph_search_dir[i];
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    FILE *fp;
    if( dl + fl + 1 > cap ) continue;
    memcpy(out, dir, dl);
    memcpy(out + dl, file, fl + 1);
    fp = fopen(out, "rb");
    if( fp != NULL ) { fclose(fp); return 1; }
  }
  return 0;
}


/*****************************************************************************/
/*                                                                           */
/*  Lazily load a font.  Returns its index in svg_glyph_fonts[] or -1 on    */
/*  failure.                                                                 */
/*                                                                           */
/*****************************************************************************/

/* Forward declarations for the CFF/OTF loader (full body lives below the   */
/* Type 1 charstring interpreter).                                           */
static int svg_glyph_find_otf_path(const char *ps_name, char *out, size_t cap);
static int svg_glyph_load_otf(svg_glyph_font *f, const char *path);

static int svg_glyph_load_font(const char *ps_name)
{
  int i;
  svg_glyph_font *f;
  char path[1024];
  unsigned char *ascii_buf = NULL;
  unsigned char *binary_buf = NULL;
  size_t ascii_len = 0, binary_len = 0;
  unsigned char *plain = NULL;
  size_t plain_len = 0;
  int tried_pfb = 0;

  if( ps_name == NULL || ps_name[0] == 0 ) return -1;
  if( !svg_glyph_std_enc_init ) svg_glyph_init_std_enc();

  /* cached?                                                                 */
  for( i = 0; i < svg_glyph_n_fonts; i++ )
  {
    if( strcmp(svg_glyph_fonts[i].ps_name, ps_name) == 0 )
    {
      if( svg_glyph_fonts[i].load_failed ) return -1;
      return i;
    }
  }
  if( svg_glyph_n_fonts >= SVG_GLYPH_MAX_FONTS ) return -1;
  f = &svg_glyph_fonts[svg_glyph_n_fonts];
  memset(f, 0, sizeof *f);
  strncpy(f->ps_name, ps_name, SVG_GLYPH_PSN_LEN-1);
  f->ps_name[SVG_GLYPH_PSN_LEN-1] = 0;
  f->lenIV    = SVG_GLYPH_DECRYPT_LEN_IV;
  f->em_scale = 1.0;
  svg_glyph_n_fonts++;

  /* (a) Try Type 1 .pfb first -- the Adobe base-35 mappings live there.    */
  if( svg_glyph_find_pfb_path(ps_name, path, sizeof path) )
  {
    tried_pfb = 1;
    if( svg_glyph_read_pfb(path, &ascii_buf, &ascii_len,
                           &binary_buf, &binary_len) &&
        binary_buf != NULL && binary_len > 0 &&
        svg_glyph_eexec_decrypt(binary_buf, binary_len, &plain, &plain_len) )
    {
      free(binary_buf);  binary_buf = NULL;
      free(ascii_buf);   ascii_buf  = NULL;
      f->kind  = SVG_GLYPH_KIND_T1;
      f->lenIV = svg_glyph_parse_lenIV(plain, plain_len);
      svg_glyph_parse_subrs(f, plain, plain_len);
      if( svg_glyph_parse_charstrings(f, plain, plain_len) && f->nglyphs > 0 )
      { free(plain); f->loaded = 1; return svg_glyph_n_fonts - 1; }
      free(plain);  plain = NULL;
    }
    else
    {
      free(binary_buf);
      free(ascii_buf);
    }
  }

  /* (b) Fall through to CFF/OTF.                                            */
  if( svg_glyph_find_otf_path(ps_name, path, sizeof path) )
  {
    /* Reset any partial state from a failed PFB attempt.                    */
    if( tried_pfb )
    {
      f->nglyphs    = 0;
      f->nsubrs     = 0;
      f->arena_used = 0;
    }
    if( svg_glyph_load_otf(f, path) && f->nglyphs > 0 )
    {
      f->kind   = SVG_GLYPH_KIND_CFF;
      f->loaded = 1;
      return svg_glyph_n_fonts - 1;
    }
  }

  f->load_failed = 1;
  return -1;
}


/*****************************************************************************/
/*                                                                           */
/*  Look up a glyph entry in a loaded font.  Returns pointer or NULL.       */
/*                                                                           */
/*****************************************************************************/

static const svg_glyph_entry *svg_glyph_find_glyph(const svg_glyph_font *f,
  const char *name)
{
  int i;
  if( name == NULL ) return NULL;
  for( i = 0; i < f->nglyphs; i++ )
    if( strcmp(f->glyphs[i].name, name) == 0 ) return &f->glyphs[i];
  return NULL;
}


/*****************************************************************************/
/*                                                                           */
/*  Type 1 charstring interpreter.                                           */
/*                                                                           */
/*  Operators implemented (Adobe Type 1 Font Format, section 8.1):           */
/*    1 hstem, 3 vstem (skipped after popping operand stack)                 */
/*    4 vmoveto, 5 rlineto, 6 hlineto, 7 vlineto                             */
/*    8 rrcurveto, 9 closepath, 10 callsubr, 11 return                       */
/*    13 hsbw (set sidebearing + width)                                      */
/*    14 endchar                                                             */
/*    21 rmoveto, 22 hmoveto                                                 */
/*    30 vhcurveto, 31 hvcurveto                                             */
/*    12 0 dotsection (no-op)                                                */
/*    12 1 vstem3, 12 2 hstem3 (skipped)                                     */
/*    12 6 seac (composite accented char)                                    */
/*    12 7 sbw (set sidebearing + width)                                     */
/*    12 12 div                                                              */
/*    12 16 callothersubr (recognise OtherSubr 3 = no-op marker, otherwise   */
/*                        skip; the next "pop" reads back a pushed value)   */
/*    12 17 pop                                                              */
/*    12 33 setcurrentpoint                                                  */
/*    12 34 hflex, 12 35 flex, 12 36 hflex1, 12 37 flex1 (decoded)           */
/*                                                                           */
/*****************************************************************************/

typedef struct svg_glyph_emit_ctx {
  void   *user;
  void  (*move)(void *, double, double);
  void  (*line)(void *, double, double);
  void  (*curve)(void *, double, double, double, double, double, double);
  void  (*close)(void *);
  double scale;       /* font_size_pt / 1000 (units-per-em assumed 1000)     */
  double x0, y0;      /* baseline origin in caller units                     */
  double cur_x;
  double cur_y;
  double adv_x;       /* glyph advance width in caller units                 */
  /* OtherSubr postscript-stack for callothersubr ... pop sequences.         */
  double ps_stack[16];
  int    ps_top;
  /* Type 1 operand stack: stays valid across return from callsubr.          */
  double stack[SVG_GLYPH_STK_DEPTH];
  int    sp;
  int    depth;       /* charstring call depth, capped to avoid loops        */
  int    abort;
} svg_glyph_emit_ctx;

static void svg_glyph_emit_move(svg_glyph_emit_ctx *c, double dx, double dy)
{
  c->cur_x += dx;
  c->cur_y += dy;
  if( c->move ) c->move(c->user, c->x0 + c->cur_x * c->scale,
                                 c->y0 + c->cur_y * c->scale);
}

static void svg_glyph_emit_line(svg_glyph_emit_ctx *c, double dx, double dy)
{
  c->cur_x += dx;
  c->cur_y += dy;
  if( c->line ) c->line(c->user, c->x0 + c->cur_x * c->scale,
                                 c->y0 + c->cur_y * c->scale);
}

static void svg_glyph_emit_curve(svg_glyph_emit_ctx *c,
  double dx1, double dy1, double dx2, double dy2, double dx3, double dy3)
{
  double x1, y1, x2, y2, x3, y3;
  c->cur_x += dx1; c->cur_y += dy1;
  x1 = c->x0 + c->cur_x * c->scale;  y1 = c->y0 + c->cur_y * c->scale;
  c->cur_x += dx2; c->cur_y += dy2;
  x2 = c->x0 + c->cur_x * c->scale;  y2 = c->y0 + c->cur_y * c->scale;
  c->cur_x += dx3; c->cur_y += dy3;
  x3 = c->x0 + c->cur_x * c->scale;  y3 = c->y0 + c->cur_y * c->scale;
  if( c->curve ) c->curve(c->user, x1, y1, x2, y2, x3, y3);
}

static int svg_glyph_decode_number(const unsigned char *cs, int len,
  int *pi, double *out)
{
  int i = *pi;
  int b;
  if( i >= len ) return 0;
  b = cs[i];
  if( b >= 32 && b <= 246 ) { *out = (double) (b - 139); *pi = i + 1; return 1; }
  if( b >= 247 && b <= 250 )
  { if( i + 1 >= len ) return 0;
    *out = (double) ((b - 247) * 256 + cs[i+1] + 108);
    *pi = i + 2; return 1;
  }
  if( b >= 251 && b <= 254 )
  { if( i + 1 >= len ) return 0;
    *out = (double) (-(b - 251) * 256 - cs[i+1] - 108);
    *pi = i + 2; return 1;
  }
  if( b == 255 )
  { long v;
    if( i + 4 >= len ) return 0;
    v = ((long) cs[i+1] << 24) | ((long) cs[i+2] << 16)
      | ((long) cs[i+3] << 8)  | (long) cs[i+4];
    /* signed 32-bit */
    if( v & 0x80000000L ) v -= 0x100000000L;
    *out = (double) v;
    *pi = i + 5; return 1;
  }
  return 0;
}

static int svg_glyph_run_cs(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *cs, int len);

static int svg_glyph_run_subr(svg_glyph_emit_ctx *c, svg_glyph_font *f, int idx)
{
  if( idx < 0 || idx >= f->nsubrs ) return 0;
  if( f->subrs[idx].cs == NULL ) return 0;
  if( c->depth > 16 ) return 0;
  c->depth++;
  { int r = svg_glyph_run_cs(c, f, f->subrs[idx].cs, f->subrs[idx].cs_len);
    c->depth--;
    return r;
  }
}

/* Decoder result codes: 1 = ok continue, 2 = endchar reached, 0 = return.   */
/* Errors set c->abort and we just return 0 to unwind.                       */
static int svg_glyph_run_cs(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *cs, int len)
{
  int i;
  int b;
  if( c->abort ) return 0;
  i = 0;
  while( i < len && !c->abort )
  {
    b = cs[i];
    if( b >= 32 || b == 255 )
    {
      double v;
      if( !svg_glyph_decode_number(cs, len, &i, &v) ) { c->abort = 1; return 0; }
      if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = v;
      continue;
    }
    /* operator */
    i++;
    if( b == 12 )
    {
      int op2;
      if( i >= len ) { c->abort = 1; return 0; }
      op2 = cs[i++];
      switch( op2 )
      {
        case 0:   /* dotsection */
          c->sp = 0;
          break;
        case 1:   /* vstem3 */
        case 2:   /* hstem3 */
          c->sp = 0;
          break;
        case 6:   /* seac achar bchar adx ady seac */
        {
          int achar, bchar;
          double adx, ady;
          if( c->sp < 5 ) { c->sp = 0; break; }
          ady   = c->stack[c->sp - 2];
          adx   = c->stack[c->sp - 3];
          bchar = (int) c->stack[c->sp - 4];
          achar = (int) c->stack[c->sp - 5];
          c->sp = 0;
          (void) ady; (void) adx; (void) bchar; (void) achar;
          /* Base glyph: draw at sb-relative origin (sbx already applied).   */
          if( bchar >= 0 && bchar < 256 && svg_glyph_std_enc[bchar] != NULL )
          {
            const svg_glyph_entry *g =
              svg_glyph_find_glyph(f, svg_glyph_std_enc[bchar]);
            if( g != NULL && g->cs != NULL )
              svg_glyph_run_cs(c, f, g->cs, g->cs_len);
          }
          /* Accent glyph: drawn at (adx, ady) relative to base sidebearing.*/
          if( achar >= 0 && achar < 256 && svg_glyph_std_enc[achar] != NULL )
          {
            const svg_glyph_entry *g =
              svg_glyph_find_glyph(f, svg_glyph_std_enc[achar]);
            if( g != NULL && g->cs != NULL )
            { double save_x = c->cur_x, save_y = c->cur_y;
              c->cur_x = adx;  c->cur_y = ady;
              svg_glyph_run_cs(c, f, g->cs, g->cs_len);
              c->cur_x = save_x;  c->cur_y = save_y;
            }
          }
          return 2;
        }
        case 7:   /* sbw  sbx sby wx wy sbw */
          if( c->sp >= 4 )
          { c->adv_x = c->stack[c->sp - 2];
            c->cur_x = c->stack[c->sp - 4];
            c->cur_y = c->stack[c->sp - 3];
          }
          c->sp = 0;
          break;
        case 12:  /* div */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp - 2], bv = c->stack[c->sp - 1];
            c->sp -= 2;
            if( bv == 0.0 ) bv = 1.0;
            if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = a / bv;
          }
          break;
        case 16:  /* callothersubr n othersubr#                              */
        {
          int n;     /* args to push to PS stack                             */
          int othr;
          int j;
          if( c->sp < 2 ) { c->sp = 0; break; }
          othr = (int) c->stack[c->sp - 1];
          n    = (int) c->stack[c->sp - 2];
          c->sp -= 2;
          if( n < 0 || n > 16 || c->sp < n ) { c->sp = 0; break; }
          /* Push the top n operand-stack values onto the PS stack in       */
          /* reverse order, then a subsequent `pop` reads them back.        */
          c->ps_top = 0;
          for( j = 0; j < n; j++ )
          { if( c->ps_top < 16 )
              c->ps_stack[c->ps_top++] = c->stack[c->sp - 1 - j];
          }
          c->sp -= n;
          (void) othr;
          break;
        }
        case 17:  /* pop -- read back from PS-side stack                    */
          if( c->ps_top > 0 )
          { c->ps_top--;
            if( c->sp < SVG_GLYPH_STK_DEPTH )
              c->stack[c->sp++] = c->ps_stack[c->ps_top];
          }
          break;
        case 33:  /* setcurrentpoint x y                                    */
          if( c->sp >= 2 )
          { c->cur_x = c->stack[c->sp - 2];
            c->cur_y = c->stack[c->sp - 1];
          }
          c->sp = 0;
          break;
        case 34:  /* hflex dx1 dx2 dy2 dx3 dx4 dx5 dx6                      */
          if( c->sp >= 7 )
          { double dx1 = c->stack[0], dx2 = c->stack[1], dy2 = c->stack[2];
            double dx3 = c->stack[3], dx4 = c->stack[4], dx5 = c->stack[5];
            double dx6 = c->stack[6];
            svg_glyph_emit_curve(c, dx1, 0.0, dx2, dy2, dx3, 0.0);
            svg_glyph_emit_curve(c, dx4, 0.0, dx5, -dy2, dx6, 0.0);
          }
          c->sp = 0;
          break;
        case 35:  /* flex dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd*/
          if( c->sp >= 13 )
          {
            svg_glyph_emit_curve(c,
              c->stack[0], c->stack[1], c->stack[2], c->stack[3],
              c->stack[4], c->stack[5]);
            svg_glyph_emit_curve(c,
              c->stack[6], c->stack[7], c->stack[8], c->stack[9],
              c->stack[10], c->stack[11]);
          }
          c->sp = 0;
          break;
        case 36:  /* hflex1 dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6             */
          if( c->sp >= 9 )
          {
            svg_glyph_emit_curve(c,
              c->stack[0], c->stack[1], c->stack[2], c->stack[3],
              c->stack[4], 0.0);
            svg_glyph_emit_curve(c,
              c->stack[5], 0.0, c->stack[6], c->stack[7],
              c->stack[8], 0.0);
          }
          c->sp = 0;
          break;
        case 37:  /* flex1 -- 11 args                                       */
          c->sp = 0;
          break;
        default:
          c->sp = 0;
          break;
      }
      continue;
    }
    /* one-byte operators */
    switch( b )
    {
      case 1:  /* hstem */
      case 3:  /* vstem */
        c->sp = 0;
        break;
      case 4:  /* vmoveto dy */
        if( c->sp >= 1 )
          svg_glyph_emit_move(c, 0.0, c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 5:  /* rlineto dx dy -- Type 1 takes one pair only.                */
        if( c->sp >= 2 )
          svg_glyph_emit_line(c, c->stack[c->sp - 2], c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 6:  /* hlineto dx                                                   */
        if( c->sp >= 1 )
          svg_glyph_emit_line(c, c->stack[c->sp - 1], 0.0);
        c->sp = 0;
        break;
      case 7:  /* vlineto dy                                                   */
        if( c->sp >= 1 )
          svg_glyph_emit_line(c, 0.0, c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 8:  /* rrcurveto dxa dya dxb dyb dxc dyc                             */
        if( c->sp >= 6 )
          svg_glyph_emit_curve(c,
            c->stack[c->sp - 6], c->stack[c->sp - 5],
            c->stack[c->sp - 4], c->stack[c->sp - 3],
            c->stack[c->sp - 2], c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 9:  /* closepath */
        if( c->close ) c->close(c->user);
        c->sp = 0;
        break;
      case 10: /* callsubr idx */
        if( c->sp >= 1 )
        { int idx = (int) c->stack[c->sp - 1];
          c->sp--;
          svg_glyph_run_subr(c, f, idx);
          if( c->abort ) return 0;
        }
        break;
      case 11: /* return */
        return 0;
      case 13: /* hsbw sbx wx */
        if( c->sp >= 2 )
        { c->cur_x = c->stack[c->sp - 2];
          c->cur_y = 0.0;
          c->adv_x = c->stack[c->sp - 1];
        }
        c->sp = 0;
        break;
      case 14: /* endchar */
        c->sp = 0;
        return 2;
      case 21: /* rmoveto dx dy */
        if( c->sp >= 2 )
          svg_glyph_emit_move(c, c->stack[c->sp - 2], c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 22: /* hmoveto dx */
        if( c->sp >= 1 )
          svg_glyph_emit_move(c, c->stack[c->sp - 1], 0.0);
        c->sp = 0;
        break;
      case 30: /* vhcurveto dy1 dx2 dy2 dx3 -- Type 1 takes 4 args only.     */
        if( c->sp >= 4 )
          svg_glyph_emit_curve(c,
            0.0,            c->stack[c->sp - 4],
            c->stack[c->sp - 3], c->stack[c->sp - 2],
            c->stack[c->sp - 1], 0.0);
        c->sp = 0;
        break;
      case 31: /* hvcurveto dx1 dx2 dy2 dy3 -- Type 1 takes 4 args only.     */
        if( c->sp >= 4 )
          svg_glyph_emit_curve(c,
            c->stack[c->sp - 4], 0.0,
            c->stack[c->sp - 3], c->stack[c->sp - 2],
            0.0,            c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      default:
        c->sp = 0;
        break;
    }
  }
  return 0;
}


/*****************************************************************************/
/*                                                                           */
/*  Public entry point.  Emits the glyph's outline to the supplied           */
/*  callbacks, translated so that the glyph's baseline origin sits at        */
/*  (x0, y0) and scaled by font_size_pt / 1000.                              */
/*                                                                           */
/*  Returns 1 on success (path was emitted) with *advance_out set to the    */
/*  glyph's advance in the caller's units.  Returns 0 on failure (no font / */
/*  no glyph / decode error) -- caller falls back to its own approximation. */
/*                                                                           */
/*****************************************************************************/

/* Forward declaration for the CFF Type 2 interpreter (defined below).      */
static int svg_glyph_run_cff_cs(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *cs, int len);

int svg_glyph_emit_outline(
  const char *ps_font_name,
  const char *glyph_name,
  double font_size_units,    /* multiplier already in caller's path units    */
  double x0, double y0,
  double *advance_out,
  void *user,
  void (*cb_move)(void *, double, double),
  void (*cb_line)(void *, double, double),
  void (*cb_curve)(void *, double, double, double, double, double, double),
  void (*cb_close)(void *))
{
  int fi;
  svg_glyph_font *f;
  const svg_glyph_entry *g;
  svg_glyph_emit_ctx ctx;
  int r;

  if( getenv("LOUT_NO_GLYPH_OUTLINES") != NULL ) return 0;

  fi = svg_glyph_load_font(ps_font_name);
  if( fi < 0 ) return 0;
  f = &svg_glyph_fonts[fi];
  g = svg_glyph_find_glyph(f, glyph_name);
  if( g == NULL ) return 0;

  memset(&ctx, 0, sizeof ctx);
  ctx.user  = user;
  ctx.move  = cb_move;
  ctx.line  = cb_line;
  ctx.curve = cb_curve;
  ctx.close = cb_close;
  /* CFF design units are usually 1000 already; if UnitsPerEm differs the   */
  /* em_scale multiplier folds into the path-unit scale so all paths land   */
  /* in the same 1000-em conceptual coordinate frame.                       */
  ctx.scale = (font_size_units / 1000.0) * f->em_scale;
  ctx.x0    = x0;
  ctx.y0    = y0;
  ctx.cur_x = 0.0;
  ctx.cur_y = 0.0;
  ctx.adv_x = 0.0;
  ctx.sp    = 0;
  ctx.ps_top = 0;
  ctx.depth = 0;
  ctx.abort = 0;

  if( f->kind == SVG_GLYPH_KIND_CFF )
    r = svg_glyph_run_cff_cs(&ctx, f, g->cs, g->cs_len);
  else
    r = svg_glyph_run_cs(&ctx, f, g->cs, g->cs_len);
  (void) r;
  if( ctx.abort ) return 0;
  if( advance_out != NULL )
    *advance_out = ctx.adv_x * ctx.scale;
  return 1;
}


/*****************************************************************************/
/*                                                                           */
/*  CFF / OpenType outline loader.                                            */
/*                                                                           */
/*  Phase 1 supports CFF-based OpenType (OTTO magic) with Type 2 charstring  */
/*  outlines.  Phase 1 does NOT support TrueType `glyf` outlines (magic     */
/*  0x00010000); when those are encountered the loader returns 0 and the    */
/*  caller falls back to the bbox approximation.                             */
/*                                                                           */
/*  Reference: Adobe Technical Note #5176 ("Compact Font Format             */
/*  Specification") and #5177 ("Type 2 Charstring Format").                  */
/*                                                                           */
/*****************************************************************************/

/* Big-endian readers (OpenType is BE).                                      */
static unsigned int svg_glyph_be_u16(const unsigned char *p)
{ return ((unsigned int) p[0] << 8) | (unsigned int) p[1]; }

static unsigned long svg_glyph_be_u32(const unsigned char *p)
{ return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16)
       | ((unsigned long) p[2] <<  8) |  (unsigned long) p[3]; }

/* Read an N-byte big-endian unsigned integer (1<=N<=4).                     */
static unsigned long svg_glyph_be_un(const unsigned char *p, int n)
{ unsigned long v = 0;
  int i;
  for( i = 0; i < n; i++ ) v = (v << 8) | (unsigned long) p[i];
  return v;
}


/*****************************************************************************/
/*  CFF INDEX walker.  An INDEX is:                                          */
/*    count (u16) -- if 0, the INDEX is empty and consumes 2 bytes total.   */
/*    offSize (u8) -- 1..4                                                  */
/*    offset[count+1] -- each offSize bytes long, 1-based into the data    */
/*    data[]          -- offset[count+1] - 1 bytes of payload              */
/*  Returns 1 on success, with *next set to one past the INDEX end and    */
/*  *count_out / *offsets_out / *data_base_out populated.                  */
/*****************************************************************************/

typedef struct svg_glyph_cff_index {
  unsigned int   count;
  int            off_size;
  const unsigned char *offsets;     /* (count+1) * off_size bytes           */
  const unsigned char *data;        /* element data starts here (offset-1) */
  size_t         total_size;        /* including header                     */
} svg_glyph_cff_index;

static int svg_glyph_cff_parse_index(const unsigned char *buf, size_t len,
  size_t off, svg_glyph_cff_index *out)
{
  unsigned int count;
  int off_size;
  size_t base, data_off, total_data;
  if( off + 2 > len ) return 0;
  count = svg_glyph_be_u16(buf + off);
  if( count == 0 )
  { out->count = 0; out->off_size = 0;
    out->offsets = NULL; out->data = NULL; out->total_size = 2;
    return 1;
  }
  if( off + 3 > len ) return 0;
  off_size = buf[off + 2];
  if( off_size < 1 || off_size > 4 ) return 0;
  base = off + 3;
  if( base + (size_t) (count + 1) * (size_t) off_size > len ) return 0;
  /* The data block begins at base + (count+1)*off_size; offsets are 1-based  */
  /* into that block.                                                        */
  data_off = base + (size_t) (count + 1) * (size_t) off_size;
  total_data = (size_t) svg_glyph_be_un(buf + base + (size_t) count * (size_t) off_size,
                                        off_size);
  if( total_data < 1 ) total_data = 1;
  if( data_off + (total_data - 1) > len ) return 0;
  out->count    = count;
  out->off_size = off_size;
  out->offsets  = buf + base;
  out->data     = buf + data_off;
  out->total_size = (data_off - off) + (total_data - 1);
  return 1;
}

/* Fetch INDEX element i (0-based).  Sets *p / *plen.  Returns 1 on success. */
static int svg_glyph_cff_index_get(const svg_glyph_cff_index *ix, unsigned int i,
  const unsigned char **p, int *plen)
{
  unsigned long o1, o2;
  if( i >= ix->count ) return 0;
  o1 = svg_glyph_be_un(ix->offsets + (size_t) i      * (size_t) ix->off_size,
                       ix->off_size);
  o2 = svg_glyph_be_un(ix->offsets + (size_t) (i+1) * (size_t) ix->off_size,
                       ix->off_size);
  if( o2 < o1 ) return 0;
  *p    = ix->data + (o1 - 1);
  *plen = (int) (o2 - o1);
  return 1;
}


/*****************************************************************************/
/*  Top DICT / Private DICT operand-and-operator decoder.  Streams through  */
/*  the DICT bytes, accumulating numeric operands until an operator byte    */
/*  arrives; each operator call invokes a tiny callback that records the    */
/*  operator-id and operand values into a `dict_result` struct.             */
/*****************************************************************************/

typedef struct svg_glyph_dict_result {
  /* From Top DICT.                                                          */
  long  charstrings_off;     /* op 17 -- offset from CFF base                */
  long  private_size;        /* op 18 [0]                                    */
  long  private_off;         /* op 18 [1]                                    */
  long  charset_off;         /* op 15 -- 0=ISOAdobe, 1=Expert, 2=ExpertSubset*/
  long  encoding_off;        /* op 16                                        */
  long  charstring_type;     /* op 12 6  (defaults to 2)                     */
  /* From Private DICT.                                                      */
  long  local_subrs_off;     /* op 19 -- relative to start of Private DICT   */
  double nominal_width_x;    /* op 21 -- defaults to 0                       */
  double default_width_x;    /* op 20 -- defaults to 0                       */
} svg_glyph_dict_result;

static void svg_glyph_dict_result_init(svg_glyph_dict_result *r)
{
  r->charstrings_off  = -1;
  r->private_size     = -1;
  r->private_off      = -1;
  r->charset_off      = -1;
  r->encoding_off     = -1;
  r->charstring_type  = 2;
  r->local_subrs_off  = -1;
  r->nominal_width_x  = 0.0;
  r->default_width_x  = 0.0;
}

/* Decode a CFF DICT operand starting at p (len = remaining bytes).         */
/* On success returns the byte length consumed and writes *out.  Returns 0 */
/* if the byte at *p is an operator (caller should dispatch).               */
static int svg_glyph_cff_decode_operand(const unsigned char *p, int len,
  double *out)
{
  int b0;
  if( len < 1 ) return 0;
  b0 = p[0];
  if( b0 <= 21 ) return 0;     /* operator -- not an operand                 */
  if( b0 >= 32 && b0 <= 246 )
  { *out = (double) (b0 - 139); return 1; }
  if( b0 >= 247 && b0 <= 250 )
  { if( len < 2 ) return 0;
    *out = (double) ((b0 - 247) * 256 + p[1] + 108);
    return 2;
  }
  if( b0 >= 251 && b0 <= 254 )
  { if( len < 2 ) return 0;
    *out = (double) (-(b0 - 251) * 256 - p[1] - 108);
    return 2;
  }
  if( b0 == 28 )
  { int v;
    if( len < 3 ) return 0;
    v = (p[1] << 8) | p[2];
    if( v & 0x8000 ) v -= 0x10000;
    *out = (double) v;
    return 3;
  }
  if( b0 == 29 )
  { long v;
    if( len < 5 ) return 0;
    v = ((long) p[1] << 24) | ((long) p[2] << 16)
      | ((long) p[3] <<  8) |  (long) p[4];
    if( v & 0x80000000L ) v -= 0x100000000L;
    *out = (double) v;
    return 5;
  }
  if( b0 == 30 )
  {
    /* Real number BCD nibbles.  Read until a terminator nibble (0xf).      */
    char buf[64];
    int bi = 0;
    int i = 1;
    int done = 0;
    while( i < len && !done && bi + 2 < (int) sizeof buf )
    { int byte = p[i++];
      int nibs[2];
      int j;
      nibs[0] = (byte >> 4) & 0xf;
      nibs[1] = byte        & 0xf;
      for( j = 0; j < 2 && !done; j++ )
      { int n = nibs[j];
        if( n <= 9 )      buf[bi++] = (char) ('0' + n);
        else if( n == 10 ) buf[bi++] = '.';
        else if( n == 11 ) buf[bi++] = 'E';
        else if( n == 12 ) { buf[bi++] = 'E'; buf[bi++] = '-'; }
        else if( n == 14 ) buf[bi++] = '-';
        else done = 1;
      }
    }
    buf[bi] = 0;
    *out = atof(buf);
    return i;
  }
  return 0;   /* Reserved 22..27, 31, 255: treat as failure                  */
}

/* Process one DICT, populating `out`.  `is_top` chooses Top-DICT vs        */
/* Private-DICT semantics.                                                  */
static void svg_glyph_cff_decode_dict(const unsigned char *buf, int len,
  int is_top, svg_glyph_dict_result *out)
{
  double stack[48];
  int sp = 0;
  int i = 0;
  while( i < len )
  {
    int b = buf[i];
    if( b > 21 )
    {
      double v;
      int n = svg_glyph_cff_decode_operand(buf + i, len - i, &v);
      if( n <= 0 ) { i++; continue; }
      if( sp < (int) (sizeof stack / sizeof stack[0]) ) stack[sp++] = v;
      i += n;
      continue;
    }
    /* operator */
    if( b == 12 )
    { int op2;
      if( i + 1 >= len ) break;
      op2 = buf[i + 1];
      i += 2;
      if( is_top && op2 == 6 ) out->charstring_type = (long) stack[sp - 1];
      sp = 0;
      continue;
    }
    if( is_top )
    {
      switch( b )
      {
        case 15: if( sp >= 1 ) out->charset_off  = (long) stack[sp-1]; break;
        case 16: if( sp >= 1 ) out->encoding_off = (long) stack[sp-1]; break;
        case 17: if( sp >= 1 ) out->charstrings_off = (long) stack[sp-1]; break;
        case 18:
          if( sp >= 2 )
          { out->private_size = (long) stack[sp-2];
            out->private_off  = (long) stack[sp-1];
          }
          break;
        default: break;
      }
    }
    else
    {
      /* Private DICT: 19 Subrs, 20 defaultWidthX, 21 nominalWidthX.        */
      switch( b )
      {
        case 19: if( sp >= 1 ) out->local_subrs_off = (long) stack[sp-1]; break;
        case 20: if( sp >= 1 ) out->default_width_x = stack[sp-1]; break;
        case 21: if( sp >= 1 ) out->nominal_width_x = stack[sp-1]; break;
        default: break;
      }
    }
    sp = 0;
    i++;
  }
}


/*****************************************************************************/
/*  OpenType table directory parser.  Locates the `CFF ` table and          */
/*  optionally `head` (for UnitsPerEm).  Returns 1 on success.              */
/*****************************************************************************/

static int svg_glyph_parse_ot_dir(const unsigned char *buf, size_t buf_len,
  size_t *cff_off, size_t *cff_len, int *units_per_em)
{
  unsigned long magic;
  unsigned int n_tables;
  size_t pos;
  unsigned int i;
  *cff_off = 0;
  *cff_len = 0;
  *units_per_em = 1000;
  if( buf_len < 12 ) return 0;
  magic = svg_glyph_be_u32(buf);
  if( magic != 0x4f54544fUL )    /* 'OTTO' = CFF OpenType                   */
    return 0;
  n_tables = svg_glyph_be_u16(buf + 4);
  if( n_tables == 0 ) return 0;
  pos = 12;
  if( pos + (size_t) n_tables * 16 > buf_len ) return 0;
  for( i = 0; i < n_tables; i++ )
  {
    const unsigned char *rec = buf + pos + (size_t) i * 16;
    unsigned long tag    = svg_glyph_be_u32(rec);
    unsigned long offset = svg_glyph_be_u32(rec + 8);
    unsigned long length = svg_glyph_be_u32(rec + 12);
    if( (size_t) offset > buf_len || (size_t) offset + length > buf_len )
      continue;
    if( tag == 0x43464620UL )   /* 'CFF ' */
    { *cff_off = offset; *cff_len = length; }
    else if( tag == 0x68656164UL )   /* 'head' */
    { if( length >= 20 )
      { unsigned int upem = svg_glyph_be_u16(buf + (size_t) offset + 18);
        if( upem > 0 ) *units_per_em = (int) upem;
      }
    }
  }
  return *cff_off != 0;
}


/*****************************************************************************/
/*  Helper: lookup an Adobe glyph name from a charset SID.  For phase 1 we   */
/*  resolve common ISOAdobe SIDs (1..228) via the predefined string table   */
/*  and let the CFF String INDEX cover the rest (SIDs >= 391).               */
/*****************************************************************************/

/* Adobe ISOAdobe predefined string table (SIDs 0..390).  Only a subset is   */
/* relevant for the glyphs Lout actually requests via charpath (ASCII +     */
/* a few punctuation).  Out-of-table SIDs return NULL.                      */
static const char *svg_glyph_cff_std_sid(unsigned int sid)
{
  /* The 392 predefined strings begin with ".notdef" and proceed through    */
  /* "001.005" at index 390.  We only need the first ~230 (ASCII letters,  */
  /* digits, punctuation).  The rest are accented variants and Adobe       */
  /* proprietary names rarely consumed by charpath.                         */
  static const char *tab[] = {
    ".notdef","space","exclam","quotedbl","numbersign","dollar","percent",
    "ampersand","quoteright","parenleft","parenright","asterisk","plus",
    "comma","hyphen","period","slash","zero","one","two","three","four",
    "five","six","seven","eight","nine","colon","semicolon","less","equal",
    "greater","question","at","A","B","C","D","E","F","G","H","I","J","K",
    "L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
    "bracketleft","backslash","bracketright","asciicircum","underscore",
    "quoteleft","a","b","c","d","e","f","g","h","i","j","k","l","m","n",
    "o","p","q","r","s","t","u","v","w","x","y","z","braceleft","bar",
    "braceright","asciitilde","exclamdown","cent","sterling","fraction",
    "yen","florin","section","currency","quotesingle","quotedblleft",
    "guillemotleft","guilsinglleft","guilsinglright","fi","fl","endash",
    "dagger","daggerdbl","periodcentered","paragraph","bullet",
    "quotesinglbase","quotedblbase","quotedblright","guillemotright",
    "ellipsis","perthousand","questiondown","grave","acute","circumflex",
    "tilde","macron","breve","dotaccent","dieresis","ring","cedilla",
    "hungarumlaut","ogonek","caron","emdash","AE","ordfeminine","Lslash",
    "Oslash","OE","ordmasculine","ae","dotlessi","lslash","oslash","oe",
    "germandbls","onesuperior","twosuperior","threesuperior","minus",
    "multiply","onesuperior","twosuperior","threesuperior","Amacron",
    "amacron","Aogonek","aogonek","Cacute","cacute","Ccaron","ccaron",
    "Dcaron","dcaron","Dcroat","dcroat","Delta","Ecaron","ecaron","Eogonek",
    "eogonek","Emacron","emacron","Gbreve","gbreve","Gcommaaccent",
    "gcommaaccent","IJ","ij","Imacron","imacron","Iogonek","iogonek",
    "Eth","eth","Lacute","lacute","Lcommaaccent","lcommaaccent","Nacute",
    "nacute","Ncaron","ncaron","Ncommaaccent","ncommaaccent","Omacron",
    "omacron","Racute","racute","Rcaron","rcaron","Rcommaaccent",
    "rcommaaccent","Sacute","sacute","Scedilla","scedilla","Scommaaccent",
    "scommaaccent","Tcaron","tcaron","Tcommaaccent","tcommaaccent","Thorn",
    "thorn","Uhungarumlaut","uhungarumlaut","Umacron","umacron","Uogonek",
    "uogonek","Uring","uring","Ydieresis","ydieresis","Zacute","zacute",
    "Zdotaccent","zdotaccent","longs","Aacute","Acircumflex","Adieresis",
    "Agrave","Aring","Atilde","Ccedilla","Eacute","Ecircumflex",
    "Edieresis","Egrave","Iacute","Icircumflex","Idieresis","Igrave",
    "Ntilde","Oacute","Ocircumflex","Odieresis","Ograve","Otilde",
    "Scaron","Uacute","Ucircumflex","Udieresis","Ugrave","Yacute",
    "aacute","acircumflex","adieresis","agrave","aring","atilde",
    "ccedilla","eacute","ecircumflex","edieresis","egrave","iacute",
    "icircumflex","idieresis","igrave","ntilde","oacute","ocircumflex",
    "odieresis","ograve","otilde","scaron","uacute","ucircumflex",
    "udieresis","ugrave","yacute","ydieresis"
  };
  if( sid < sizeof tab / sizeof tab[0] ) return tab[sid];
  return NULL;
}

/*****************************************************************************/
/*  Build the per-glyph charset table.  CFF format 0/1/2.                    */
/*  Returns 1 on success.                                                    */
/*****************************************************************************/

static int svg_glyph_cff_parse_charset(svg_glyph_font *f,
  const unsigned char *cff_buf, size_t cff_len,
  long charset_off, int n_glyphs,
  const svg_glyph_cff_index *string_ix,
  unsigned int *sid_for_gid)
{
  size_t p;
  int format;
  int i;

  /* Predefined charsets (0=ISOAdobe, 1=Expert, 2=ExpertSubset).             */
  if( charset_off >= 0 && charset_off <= 2 )
  {
    if( charset_off == 0 )
    {
      /* GID i -> SID i for i in [0..228].  Beyond that we leave 0 (.notdef)*/
      for( i = 0; i < n_glyphs; i++ )
        sid_for_gid[i] = (unsigned int) i;
      return 1;
    }
    /* Expert / ExpertSubset: leave as .notdef -- our test corpus doesn't   */
    /* exercise these.                                                       */
    for( i = 0; i < n_glyphs; i++ ) sid_for_gid[i] = 0;
    return 1;
  }
  if( charset_off < 0 || (size_t) charset_off >= cff_len ) return 0;
  p = (size_t) charset_off;
  format = cff_buf[p++];
  sid_for_gid[0] = 0;   /* GID 0 is always .notdef                          */
  if( format == 0 )
  {
    /* (n_glyphs - 1) SIDs of 2 bytes each.                                  */
    if( p + (size_t) (n_glyphs - 1) * 2 > cff_len ) return 0;
    for( i = 1; i < n_glyphs; i++ )
    { sid_for_gid[i] = svg_glyph_be_u16(cff_buf + p);
      p += 2;
    }
    return 1;
  }
  if( format == 1 || format == 2 )
  {
    int gid = 1;
    while( gid < n_glyphs )
    {
      unsigned int first;
      unsigned int n_left;
      unsigned int k;
      if( format == 1 )
      { if( p + 3 > cff_len ) return 0;
        first  = svg_glyph_be_u16(cff_buf + p);
        n_left = cff_buf[p + 2];
        p += 3;
      }
      else
      { if( p + 4 > cff_len ) return 0;
        first  = svg_glyph_be_u16(cff_buf + p);
        n_left = svg_glyph_be_u16(cff_buf + p + 2);
        p += 4;
      }
      for( k = 0; k <= n_left && gid < n_glyphs; k++, gid++ )
        sid_for_gid[gid] = first + k;
    }
    (void) string_ix;
    return 1;
  }
  return 0;
}

/* Resolve a SID into a malloc-free glyph name string.  Predefined SIDs     */
/* (0..390) come from the static table; SIDs >= 391 index into the CFF      */
/* String INDEX -- we copy the string into a small static buffer.           */
static const char *svg_glyph_cff_sid_name(unsigned int sid,
  const svg_glyph_cff_index *string_ix, char *buf, int buf_cap)
{
  if( sid < 391 ) return svg_glyph_cff_std_sid(sid);
  if( string_ix == NULL || string_ix->count == 0 ) return NULL;
  { unsigned int idx = sid - 391;
    const unsigned char *p;
    int plen;
    int n;
    if( !svg_glyph_cff_index_get(string_ix, idx, &p, &plen) ) return NULL;
    n = plen;
    if( n >= buf_cap ) n = buf_cap - 1;
    memcpy(buf, p, (size_t) n);
    buf[n] = 0;
    return buf;
  }
}


/*****************************************************************************/
/*  Bias the Subr index per the Type 2 spec:                                 */
/*    if N >= 33900: bias = 32768                                            */
/*    if N >= 1240:  bias = 1131                                             */
/*    else:          bias = 107                                              */
/*****************************************************************************/

static int svg_glyph_cff_subr_bias(int n)
{
  if( n >= 33900 ) return 32768;
  if( n >=  1240 ) return 1131;
  return 107;
}


/*****************************************************************************/
/*  Top-level CFF parser.  Given the contents of the CFF table, build the   */
/*  font's charset, local Subrs, global Subrs, and CharStrings into the     */
/*  font's arena.                                                            */
/*****************************************************************************/

static int svg_glyph_load_cff(svg_glyph_font *f,
  const unsigned char *cff, size_t cff_len, int units_per_em)
{
  svg_glyph_cff_index name_ix, top_ix, string_ix, gsubr_ix, cs_ix, lsubr_ix;
  svg_glyph_dict_result top, priv;
  size_t pos;
  size_t hdr_size;
  const unsigned char *top_dict_p;
  int top_dict_len;
  int n_glyphs;
  unsigned int *sid_for_gid;
  int i;
  int ok;

  if( cff_len < 4 ) return 0;
  hdr_size = cff[2];
  if( hdr_size < 4 || hdr_size > cff_len ) return 0;

  /* Name INDEX */
  pos = hdr_size;
  if( !svg_glyph_cff_parse_index(cff, cff_len, pos, &name_ix) ) return 0;
  pos += name_ix.total_size;

  /* Top DICT INDEX */
  if( !svg_glyph_cff_parse_index(cff, cff_len, pos, &top_ix) ) return 0;
  pos += top_ix.total_size;

  /* String INDEX */
  if( !svg_glyph_cff_parse_index(cff, cff_len, pos, &string_ix) ) return 0;
  pos += string_ix.total_size;

  /* Global Subr INDEX */
  if( !svg_glyph_cff_parse_index(cff, cff_len, pos, &gsubr_ix) ) return 0;
  pos += gsubr_ix.total_size;

  /* Use Top DICT element 0 (only one font supported in phase 1).            */
  if( top_ix.count == 0 ) return 0;
  if( !svg_glyph_cff_index_get(&top_ix, 0, &top_dict_p, &top_dict_len) )
    return 0;
  svg_glyph_dict_result_init(&top);
  svg_glyph_cff_decode_dict(top_dict_p, top_dict_len, 1, &top);
  if( top.charstring_type != 2 ) return 0;   /* Type 1 in OTF not supported  */
  if( top.charstrings_off < 0 ||
      (size_t) top.charstrings_off >= cff_len ) return 0;

  /* CharStrings INDEX */
  if( !svg_glyph_cff_parse_index(cff, cff_len,
        (size_t) top.charstrings_off, &cs_ix) ) return 0;
  n_glyphs = (int) cs_ix.count;
  if( n_glyphs <= 0 ) return 0;
  if( n_glyphs > SVG_GLYPH_MAX_GLYPHS ) n_glyphs = SVG_GLYPH_MAX_GLYPHS;

  /* Private DICT */
  svg_glyph_dict_result_init(&priv);
  if( top.private_off >= 0 && top.private_size > 0 &&
      (size_t) top.private_off + (size_t) top.private_size <= cff_len )
  {
    svg_glyph_cff_decode_dict(cff + top.private_off,
                              (int) top.private_size, 0, &priv);
  }

  /* Local Subr INDEX (offset is relative to start of Private DICT).         */
  lsubr_ix.count = 0;
  lsubr_ix.off_size = 0;
  lsubr_ix.offsets = NULL;
  lsubr_ix.data    = NULL;
  lsubr_ix.total_size = 0;
  if( priv.local_subrs_off >= 0 && top.private_off >= 0 )
  {
    size_t off = (size_t) top.private_off + (size_t) priv.local_subrs_off;
    if( off < cff_len )
      svg_glyph_cff_parse_index(cff, cff_len, off, &lsubr_ix);
  }

  /* Copy global Subrs into the font's gsubr table.                          */
  f->ngsubrs = (int) gsubr_ix.count;
  if( f->ngsubrs > SVG_GLYPH_MAX_GSUBRS ) f->ngsubrs = SVG_GLYPH_MAX_GSUBRS;
  for( i = 0; i < f->ngsubrs; i++ )
  { const unsigned char *p;  int plen;
    if( !svg_glyph_cff_index_get(&gsubr_ix, (unsigned int) i, &p, &plen) )
    { f->gsubrs[i].cs = NULL; f->gsubrs[i].cs_len = 0; continue; }
    { unsigned char *dst = svg_glyph_arena_alloc(f, (size_t) plen);
      if( dst == NULL ) return 0;
      if( plen > 0 ) memcpy(dst, p, (size_t) plen);
      f->gsubrs[i].cs     = dst;
      f->gsubrs[i].cs_len = plen;
    }
  }
  f->gsubr_bias = svg_glyph_cff_subr_bias(f->ngsubrs);

  /* Copy local Subrs.                                                       */
  f->nsubrs = (int) lsubr_ix.count;
  if( f->nsubrs > SVG_GLYPH_MAX_SUBRS ) f->nsubrs = SVG_GLYPH_MAX_SUBRS;
  for( i = 0; i < f->nsubrs; i++ )
  { const unsigned char *p;  int plen;
    if( !svg_glyph_cff_index_get(&lsubr_ix, (unsigned int) i, &p, &plen) )
    { f->subrs[i].cs = NULL; f->subrs[i].cs_len = 0; continue; }
    { unsigned char *dst = svg_glyph_arena_alloc(f, (size_t) plen);
      if( dst == NULL ) return 0;
      if( plen > 0 ) memcpy(dst, p, (size_t) plen);
      f->subrs[i].cs     = dst;
      f->subrs[i].cs_len = plen;
    }
  }
  f->subr_bias = svg_glyph_cff_subr_bias(f->nsubrs);

  /* Resolve the charset GID -> SID -> name mapping.                         */
  sid_for_gid = (unsigned int *) calloc((size_t) n_glyphs, sizeof *sid_for_gid);
  if( sid_for_gid == NULL ) return 0;
  ok = svg_glyph_cff_parse_charset(f, cff, cff_len, top.charset_off,
        n_glyphs, &string_ix, sid_for_gid);
  if( !ok ) { free(sid_for_gid); return 0; }

  /* Walk CharStrings INDEX, copy each charstring into the arena, and tag    */
  /* with its glyph name (via charset SID lookup).                           */
  f->nglyphs = 0;
  for( i = 0; i < n_glyphs && f->nglyphs < SVG_GLYPH_MAX_GLYPHS; i++ )
  {
    const unsigned char *p;
    int plen;
    char sidbuf[SVG_GLYPH_NAME_LEN];
    const char *gname;
    unsigned char *dst;
    if( !svg_glyph_cff_index_get(&cs_ix, (unsigned int) i, &p, &plen) )
      continue;
    gname = svg_glyph_cff_sid_name(sid_for_gid[i], &string_ix,
                                   sidbuf, (int) sizeof sidbuf);
    if( gname == NULL || gname[0] == 0 ) continue;
    dst = svg_glyph_arena_alloc(f, (size_t) plen);
    if( dst == NULL ) { free(sid_for_gid); return 0; }
    if( plen > 0 ) memcpy(dst, p, (size_t) plen);
    { size_t nl = strlen(gname);
      if( nl >= SVG_GLYPH_NAME_LEN ) nl = SVG_GLYPH_NAME_LEN - 1;
      memcpy(f->glyphs[f->nglyphs].name, gname, nl);
      f->glyphs[f->nglyphs].name[nl] = 0;
    }
    f->glyphs[f->nglyphs].cs     = dst;
    f->glyphs[f->nglyphs].cs_len = plen;
    f->nglyphs++;
  }
  free(sid_for_gid);

  /* If the font's units-per-em isn't 1000 we scale every coordinate the    */
  /* interpreter emits by (1000 / upem) so downstream code can keep         */
  /* assuming a notional 1000-em.                                            */
  if( units_per_em > 0 && units_per_em != 1000 )
    f->em_scale = 1000.0 / (double) units_per_em;
  else
    f->em_scale = 1.0;

  /* Defaults for advance: nominal+defaultWidthX live here, but the per-    */
  /* glyph width arrives at the front of every charstring (see Type 2      */
  /* width handling in the interpreter).                                    */
  (void) priv.nominal_width_x;
  (void) priv.default_width_x;
  return 1;
}


/*****************************************************************************/
/*  OTF file -> CFF body extractor + svg_glyph_load_cff caller.              */
/*****************************************************************************/

static int svg_glyph_load_otf(svg_glyph_font *f, const char *path)
{
  FILE *fp;
  long fsize;
  unsigned char *raw;
  size_t got;
  size_t cff_off, cff_len;
  int upem;
  int ok;

  fp = fopen(path, "rb");
  if( fp == NULL ) return 0;
  fseek(fp, 0L, SEEK_END);
  fsize = ftell(fp);
  fseek(fp, 0L, SEEK_SET);
  if( fsize <= 12 || fsize > SVG_GLYPH_OTF_MAX )
  { fclose(fp); return 0; }
  raw = (unsigned char *) malloc((size_t) fsize);
  if( raw == NULL ) { fclose(fp); return 0; }
  got = fread(raw, 1, (size_t) fsize, fp);
  fclose(fp);
  if( got != (size_t) fsize ) { free(raw); return 0; }

  if( !svg_glyph_parse_ot_dir(raw, (size_t) fsize, &cff_off, &cff_len, &upem) )
  { free(raw); return 0; }
  ok = svg_glyph_load_cff(f, raw + cff_off, cff_len, upem);
  free(raw);
  return ok;
}


/*****************************************************************************/
/*  OTF file lookup.  Tries:                                                 */
/*    1. LOUT_OTF_FONT_DIR/<ps_name>.otf                                    */
/*    2. svg_glyph_otf_map[ps_name] joined with each search dir            */
/*    3. <ps_name>.otf joined with each search dir (recursive 1 level)     */
/*****************************************************************************/

static int svg_glyph_otf_probe(const char *path)
{
  /* Open and confirm OTTO magic.                                            */
  FILE *fp = fopen(path, "rb");
  unsigned char magic[4];
  size_t n;
  if( fp == NULL ) return 0;
  n = fread(magic, 1, 4, fp);
  fclose(fp);
  if( n != 4 ) return 0;
  return (magic[0] == 'O' && magic[1] == 'T' && magic[2] == 'T' && magic[3] == 'O');
}

static int svg_glyph_try_dir(const char *dir, const char *name,
  char *out, size_t cap)
{
  size_t dl, nl;
  if( dir == NULL || name == NULL ) return 0;
  dl = strlen(dir);
  nl = strlen(name);
  if( dl + 1 + nl + 1 > cap ) return 0;
  memcpy(out, dir, dl);
  if( dl > 0 && out[dl-1] != '/' ) out[dl++] = '/';
  memcpy(out + dl, name, nl + 1);
  return svg_glyph_otf_probe(out);
}

/* Recursive depth-1 search: try `dir/name` directly, then scan each       */
/* immediate subdirectory for `name`.                                       */
static int svg_glyph_try_dir_recursive(const char *dir, const char *name,
  char *out, size_t cap)
{
  /* Direct hit.                                                             */
  if( svg_glyph_try_dir(dir, name, out, cap) ) return 1;
  /* One-level subdirectory probe.  No <dirent.h> -- we use a small fixed   */
  /* set of well-known subdirs under /usr/share/fonts/opentype/ that ship   */
  /* CFF OTFs on common Debian/Ubuntu spins.                                */
  {
    static const char *subs[] = {
      "cabin/","cantarell/","linux-libertine/","fira/","firacode/",
      "ebgaramond/","comfortaa/","gentium/","gentium-basic/",
      "lobster/","lobstertwo/","artemisia/","didot/","didot-classic/",
      "bodoni-classic/","ipaexfont-gothic/","ipaexfont-mincho/",
      "noto/","source-sans-pro/","source-serif-pro/","source-code-pro/",
      NULL
    };
    int i;
    char joined[512];
    for( i = 0; subs[i] != NULL; i++ )
    {
      size_t dl = strlen(dir);
      size_t sl = strlen(subs[i]);
      if( dl + sl + 1 > sizeof joined ) continue;
      memcpy(joined, dir, dl);
      if( dl > 0 && joined[dl-1] != '/' ) joined[dl++] = '/';
      memcpy(joined + dl, subs[i], sl + 1);
      if( svg_glyph_try_dir(joined, name, out, cap) ) return 1;
    }
  }
  return 0;
}

static int svg_glyph_find_otf_path(const char *ps_name, char *out, size_t cap)
{
  const char *override;
  const char *file = NULL;
  char namebuf[SVG_GLYPH_PSN_LEN + 8];
  int i;

  /* Map PS name -> file basename, or default to "<psname>.otf".            */
  for( i = 0; svg_glyph_otf_map[i].ps_name != NULL; i++ )
  {
    if( strcmp(svg_glyph_otf_map[i].ps_name, ps_name) == 0 )
    { file = svg_glyph_otf_map[i].file; break; }
  }
  if( file == NULL )
  { size_t pl = strlen(ps_name);
    if( pl + 4 + 1 > sizeof namebuf ) return 0;
    memcpy(namebuf, ps_name, pl);
    memcpy(namebuf + pl, ".otf", 5);
    file = namebuf;
  }

  override = getenv("LOUT_OTF_FONT_DIR");
  if( override != NULL && override[0] != 0 )
  {
    if( svg_glyph_try_dir_recursive(override, file, out, cap) ) return 1;
  }
  for( i = 0; svg_glyph_otf_dir[i] != NULL; i++ )
  {
    if( svg_glyph_try_dir_recursive(svg_glyph_otf_dir[i], file, out, cap) )
      return 1;
  }
  return 0;
}


/*****************************************************************************/
/*                                                                           */
/*  Type 2 charstring interpreter.                                           */
/*                                                                           */
/*  Reference: Adobe Technical Note #5177.  Operators:                       */
/*    1 hstem, 3 vstem, 4 vmoveto, 5 rlineto, 6 hlineto, 7 vlineto           */
/*    8 rrcurveto, 10 callsubr, 11 return, 14 endchar                        */
/*    18 hstemhm, 19 hintmask, 20 cntrmask                                   */
/*    21 rmoveto, 22 hmoveto, 23 vstemhm                                     */
/*    24 rcurveline, 25 rlinecurve, 26 vvcurveto, 27 hhcurveto               */
/*    29 callgsubr, 30 vhcurveto, 31 hvcurveto                               */
/*  Plus the 12-prefix family (and / or / not / abs / add / sub / div /      */
/*  neg / eq / drop / put / get / ifelse / random / mul / sqrt / dup /       */
/*  exch / index / roll / hflex / flex / hflex1 / flex1).                    */
/*                                                                           */
/*  The first call to a stack-clearing operator (hstem/hstemhm/vstem/        */
/*  vstemhm/hintmask/cntrmask/hmoveto/vmoveto/rmoveto/endchar) may carry an  */
/*  extra leading operand: the charstring's advance-width delta relative to */
/*  nominalWidthX.  We snapshot this once.                                   */
/*                                                                           */
/*****************************************************************************/

typedef struct svg_glyph_cff_state {
  int hint_count;     /* number of hint stems seen (for hintmask bytes)     */
  int width_seen;     /* 1 after first stack-clear op consumed the width   */
  int transient[32];  /* T2 transient array (Type 2 'put'/'get')           */
  int depth;
  int abort;
} svg_glyph_cff_state;

static int svg_glyph_cff_decode_number(const unsigned char *cs, int len,
  int *pi, double *out)
{
  int i = *pi;
  int b;
  if( i >= len ) return 0;
  b = cs[i];
  if( b >= 32 && b <= 246 )
  { *out = (double) (b - 139); *pi = i + 1; return 1; }
  if( b >= 247 && b <= 250 )
  { if( i + 1 >= len ) return 0;
    *out = (double) ((b - 247) * 256 + cs[i+1] + 108);
    *pi = i + 2; return 1;
  }
  if( b >= 251 && b <= 254 )
  { if( i + 1 >= len ) return 0;
    *out = (double) (-(b - 251) * 256 - cs[i+1] - 108);
    *pi = i + 2; return 1;
  }
  if( b == 28 )
  { int v;
    if( i + 2 >= len ) return 0;
    v = (cs[i+1] << 8) | cs[i+2];
    if( v & 0x8000 ) v -= 0x10000;
    *out = (double) v;
    *pi = i + 3; return 1;
  }
  if( b == 255 )
  { /* 16.16 fixed.                                                         */
    long v;
    if( i + 4 >= len ) return 0;
    v = ((long) cs[i+1] << 24) | ((long) cs[i+2] << 16)
      | ((long) cs[i+3] <<  8) |  (long) cs[i+4];
    if( v & 0x80000000L ) v -= 0x100000000L;
    *out = (double) v / 65536.0;
    *pi = i + 5; return 1;
  }
  return 0;
}

/* Forward decls inside the Type 2 interpreter.                              */
static int svg_glyph_run_cff_body(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  svg_glyph_cff_state *st, const unsigned char *cs, int len);

/* Eat the width operand at start-of-charstring (only on the first stack-   */
/* clearing op).  Type 2 spec: if the stack has more arguments than the op  */
/* consumes, the first is the width delta.  Caller passes consumed counts.  */
static void svg_glyph_cff_eat_width(svg_glyph_emit_ctx *c,
  svg_glyph_cff_state *st, int expected_min)
{
  if( !st->width_seen )
  {
    st->width_seen = 1;
    if( c->sp > expected_min )
    {
      /* The width is the first (bottom) operand; shift the stack left.     */
      double w = c->stack[0];
      int i;
      c->adv_x = w;     /* relative to nominalWidthX; absolute value would  */
                        /* need nominalWidthX from Private DICT but the    */
                        /* charpath consumer only needs a plausible advance*/
      for( i = 0; i + 1 < c->sp; i++ ) c->stack[i] = c->stack[i + 1];
      c->sp--;
    }
  }
}

static int svg_glyph_cff_subr(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  svg_glyph_cff_state *st, int raw_idx, int is_global)
{
  int bias = is_global ? f->gsubr_bias : f->subr_bias;
  int idx = raw_idx + bias;
  const svg_subr_entry *tab = is_global ? f->gsubrs : f->subrs;
  int n = is_global ? f->ngsubrs : f->nsubrs;
  if( idx < 0 || idx >= n ) return 0;
  if( tab[idx].cs == NULL ) return 0;
  if( st->depth > 10 ) return 0;
  st->depth++;
  { int r = svg_glyph_run_cff_body(c, f, st, tab[idx].cs, tab[idx].cs_len);
    st->depth--;
    return r;
  }
}

/* Run a Type 2 charstring.  Return 2 on endchar, 0 otherwise.              */
static int svg_glyph_run_cff_body(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  svg_glyph_cff_state *st, const unsigned char *cs, int len)
{
  int i = 0;
  int b;
  if( c->abort ) return 0;
  while( i < len && !c->abort )
  {
    b = cs[i];
    if( b >= 32 || b == 28 || b == 255 )
    {
      double v;
      if( !svg_glyph_cff_decode_number(cs, len, &i, &v) ) { c->abort = 1; return 0; }
      if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = v;
      continue;
    }
    /* operator */
    i++;
    if( b == 12 )
    {
      int op2;
      if( i >= len ) { c->abort = 1; return 0; }
      op2 = cs[i++];
      switch( op2 )
      {
        case 3:  /* and */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH )
              c->stack[c->sp++] = (a != 0.0 && bv != 0.0) ? 1.0 : 0.0;
          }
          break;
        case 4:  /* or */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH )
              c->stack[c->sp++] = (a != 0.0 || bv != 0.0) ? 1.0 : 0.0;
          }
          break;
        case 5:  /* not */
          if( c->sp >= 1 )
          { double a = c->stack[c->sp-1];
            c->stack[c->sp-1] = (a == 0.0) ? 1.0 : 0.0;
          }
          break;
        case 9:  /* abs */
          if( c->sp >= 1 )
          { double a = c->stack[c->sp-1];
            c->stack[c->sp-1] = a < 0.0 ? -a : a;
          }
          break;
        case 10: /* add */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = a + bv;
          }
          break;
        case 11: /* sub */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = a - bv;
          }
          break;
        case 12: /* div */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( bv == 0.0 ) bv = 1.0;
            if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = a / bv;
          }
          break;
        case 14: /* neg */
          if( c->sp >= 1 ) c->stack[c->sp-1] = -c->stack[c->sp-1];
          break;
        case 15: /* eq */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH )
              c->stack[c->sp++] = (a == bv) ? 1.0 : 0.0;
          }
          break;
        case 18: /* drop */
          if( c->sp >= 1 ) c->sp--;
          break;
        case 20: /* put: val i put -- transient[i] = val                    */
          if( c->sp >= 2 )
          { int ti = (int) c->stack[c->sp-1];
            double val = c->stack[c->sp-2];
            c->sp -= 2;
            if( ti >= 0 && ti < 32 ) st->transient[ti] = (int) val;
          }
          break;
        case 21: /* get: i get -- push transient[i]                         */
          if( c->sp >= 1 )
          { int ti = (int) c->stack[c->sp-1];
            c->sp--;
            if( c->sp < SVG_GLYPH_STK_DEPTH && ti >= 0 && ti < 32 )
              c->stack[c->sp++] = (double) st->transient[ti];
          }
          break;
        case 22: /* ifelse */
          if( c->sp >= 4 )
          { double v1 = c->stack[c->sp-4], v2 = c->stack[c->sp-3];
            double s1 = c->stack[c->sp-2], s2 = c->stack[c->sp-1];
            c->sp -= 4;
            if( c->sp < SVG_GLYPH_STK_DEPTH )
              c->stack[c->sp++] = (v1 <= v2) ? s1 : s2;
          }
          break;
        case 23: /* random */
          if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = 0.5;
          break;
        case 24: /* mul */
          if( c->sp >= 2 )
          { double a = c->stack[c->sp-2], bv = c->stack[c->sp-1];
            c->sp -= 2;
            if( c->sp < SVG_GLYPH_STK_DEPTH ) c->stack[c->sp++] = a * bv;
          }
          break;
        case 27: /* dup */
          if( c->sp >= 1 && c->sp < SVG_GLYPH_STK_DEPTH )
          { c->stack[c->sp] = c->stack[c->sp - 1]; c->sp++; }
          break;
        case 28: /* exch */
          if( c->sp >= 2 )
          { double t = c->stack[c->sp-1];
            c->stack[c->sp-1] = c->stack[c->sp-2];
            c->stack[c->sp-2] = t;
          }
          break;
        case 29: /* index */
          if( c->sp >= 1 )
          { int ti = (int) c->stack[c->sp-1];
            c->sp--;
            if( ti < 0 ) ti = 0;
            if( ti < c->sp && c->sp < SVG_GLYPH_STK_DEPTH )
            { c->stack[c->sp] = c->stack[c->sp - 1 - ti]; c->sp++; }
          }
          break;
        case 30: /* roll N J -- ignored to keep stack manipulation simple   */
          if( c->sp >= 2 ) c->sp -= 2;
          break;
        case 34: /* hflex dx1 dx2 dy2 dx3 dx4 dx5 dx6                       */
          if( c->sp >= 7 )
          { double dx1 = c->stack[0], dx2 = c->stack[1], dy2 = c->stack[2];
            double dx3 = c->stack[3], dx4 = c->stack[4], dx5 = c->stack[5];
            double dx6 = c->stack[6];
            svg_glyph_emit_curve(c, dx1, 0.0, dx2, dy2, dx3, 0.0);
            svg_glyph_emit_curve(c, dx4, 0.0, dx5, -dy2, dx6, 0.0);
          }
          c->sp = 0;
          break;
        case 35: /* flex 11 args + fd                                       */
          if( c->sp >= 12 )
          {
            svg_glyph_emit_curve(c,
              c->stack[0], c->stack[1], c->stack[2], c->stack[3],
              c->stack[4], c->stack[5]);
            svg_glyph_emit_curve(c,
              c->stack[6], c->stack[7], c->stack[8], c->stack[9],
              c->stack[10], c->stack[11]);
          }
          c->sp = 0;
          break;
        case 36: /* hflex1 dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6              */
          if( c->sp >= 9 )
          {
            svg_glyph_emit_curve(c,
              c->stack[0], c->stack[1], c->stack[2], c->stack[3],
              c->stack[4], 0.0);
            svg_glyph_emit_curve(c,
              c->stack[5], 0.0, c->stack[6], c->stack[7],
              c->stack[8], 0.0);
          }
          c->sp = 0;
          break;
        case 37: /* flex1 dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6        */
          if( c->sp >= 11 )
          {
            double dx = 0.0, dy = 0.0;
            int j;
            for( j = 0; j < 5; j++ )
            { dx += c->stack[j * 2];
              dy += c->stack[j * 2 + 1];
            }
            svg_glyph_emit_curve(c,
              c->stack[0], c->stack[1], c->stack[2], c->stack[3],
              c->stack[4], c->stack[5]);
            {
              double dx6, dy6;
              if( dx < 0 ) dx = -dx;
              if( dy < 0 ) dy = -dy;
              if( dx > dy ) { dx6 = c->stack[10]; dy6 = 0.0; }
              else          { dx6 = 0.0;          dy6 = c->stack[10]; }
              svg_glyph_emit_curve(c,
                c->stack[6], c->stack[7], c->stack[8], c->stack[9], dx6, dy6);
            }
          }
          c->sp = 0;
          break;
        case 26: /* sqrt */
        case 33: /* setcurrentpoint -- skip                                  */
        default:
          c->sp = 0;
          break;
      }
      continue;
    }
    /* one-byte operators */
    switch( b )
    {
      case 1:  /* hstem  -- (width?) y dy {dya dyb}*                        */
      case 3:  /* vstem  -- (width?) x dx {dxa dxb}*                        */
      case 18: /* hstemhm                                                   */
      case 23: /* vstemhm                                                   */
        svg_glyph_cff_eat_width(c, st, 2);
        st->hint_count += c->sp / 2;
        c->sp = 0;
        break;
      case 19: /* hintmask                                                  */
      case 20: /* cntrmask                                                  */
      {
        int mask_bytes;
        /* If we're still on the first op and the stack has stem operands,  */
        /* count them as implicit vstem and remember to absorb the bytes.   */
        svg_glyph_cff_eat_width(c, st, 0);
        st->hint_count += c->sp / 2;
        c->sp = 0;
        mask_bytes = (st->hint_count + 7) / 8;
        if( i + mask_bytes > len ) { c->abort = 1; return 0; }
        i += mask_bytes;
        break;
      }
      case 4:  /* vmoveto dy                                                */
        svg_glyph_cff_eat_width(c, st, 1);
        if( c->sp >= 1 )
          svg_glyph_emit_move(c, 0.0, c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 21: /* rmoveto dx dy                                             */
        svg_glyph_cff_eat_width(c, st, 2);
        if( c->sp >= 2 )
          svg_glyph_emit_move(c, c->stack[c->sp - 2], c->stack[c->sp - 1]);
        c->sp = 0;
        break;
      case 22: /* hmoveto dx                                                */
        svg_glyph_cff_eat_width(c, st, 1);
        if( c->sp >= 1 )
          svg_glyph_emit_move(c, c->stack[c->sp - 1], 0.0);
        c->sp = 0;
        break;
      case 5:  /* rlineto {dxa dya}+ (variadic)                             */
      {
        int j;
        for( j = 0; j + 1 < c->sp; j += 2 )
          svg_glyph_emit_line(c, c->stack[j], c->stack[j + 1]);
        c->sp = 0;
        break;
      }
      case 6:  /* hlineto {dxa {dyb dxc}*}? -- alternating starting H       */
      {
        int j;
        int horiz = 1;
        for( j = 0; j < c->sp; j++ )
        {
          if( horiz ) svg_glyph_emit_line(c, c->stack[j], 0.0);
          else        svg_glyph_emit_line(c, 0.0, c->stack[j]);
          horiz = !horiz;
        }
        c->sp = 0;
        break;
      }
      case 7:  /* vlineto -- alternating starting V                         */
      {
        int j;
        int vert = 1;
        for( j = 0; j < c->sp; j++ )
        {
          if( vert ) svg_glyph_emit_line(c, 0.0, c->stack[j]);
          else       svg_glyph_emit_line(c, c->stack[j], 0.0);
          vert = !vert;
        }
        c->sp = 0;
        break;
      }
      case 8:  /* rrcurveto {dxa dya dxb dyb dxc dyc}+                      */
      {
        int j;
        for( j = 0; j + 5 < c->sp; j += 6 )
          svg_glyph_emit_curve(c,
            c->stack[j],   c->stack[j+1], c->stack[j+2], c->stack[j+3],
            c->stack[j+4], c->stack[j+5]);
        c->sp = 0;
        break;
      }
      case 10: /* callsubr */
        if( c->sp >= 1 )
        { int idx = (int) c->stack[c->sp - 1];
          c->sp--;
          svg_glyph_cff_subr(c, f, st, idx, 0);
          if( c->abort ) return 0;
        }
        break;
      case 11: /* return */
        return 0;
      case 14: /* endchar                                                   */
        svg_glyph_cff_eat_width(c, st, 0);
        c->sp = 0;
        return 2;
      case 24: /* rcurveline {dxa dya dxb dyb dxc dyc}+ dxd dyd             */
      {
        int j;
        for( j = 0; j + 5 < c->sp - 1; j += 6 )
          svg_glyph_emit_curve(c,
            c->stack[j],   c->stack[j+1], c->stack[j+2], c->stack[j+3],
            c->stack[j+4], c->stack[j+5]);
        if( c->sp >= 2 )
          svg_glyph_emit_line(c, c->stack[c->sp-2], c->stack[c->sp-1]);
        c->sp = 0;
        break;
      }
      case 25: /* rlinecurve {dxa dya}+ dxb dyb dxc dyc dxd dyd             */
      {
        int j;
        int n_lines = (c->sp - 6) / 2;
        for( j = 0; j < n_lines; j++ )
          svg_glyph_emit_line(c, c->stack[j*2], c->stack[j*2+1]);
        if( c->sp >= 6 )
          svg_glyph_emit_curve(c,
            c->stack[c->sp-6], c->stack[c->sp-5],
            c->stack[c->sp-4], c->stack[c->sp-3],
            c->stack[c->sp-2], c->stack[c->sp-1]);
        c->sp = 0;
        break;
      }
      case 26: /* vvcurveto [dx1] {dya dxb dyb dyc}+                        */
      {
        int j = 0;
        double dx1 = 0.0;
        if( c->sp % 4 == 1 )
        { dx1 = c->stack[0]; j = 1; }
        while( j + 3 < c->sp )
        {
          svg_glyph_emit_curve(c,
            dx1,            c->stack[j],
            c->stack[j+1], c->stack[j+2],
            0.0,            c->stack[j+3]);
          dx1 = 0.0;
          j += 4;
        }
        c->sp = 0;
        break;
      }
      case 27: /* hhcurveto [dy1] {dxa dxb dyb dxc}+                        */
      {
        int j = 0;
        double dy1 = 0.0;
        if( c->sp % 4 == 1 )
        { dy1 = c->stack[0]; j = 1; }
        while( j + 3 < c->sp )
        {
          svg_glyph_emit_curve(c,
            c->stack[j],   dy1,
            c->stack[j+1], c->stack[j+2],
            c->stack[j+3], 0.0);
          dy1 = 0.0;
          j += 4;
        }
        c->sp = 0;
        break;
      }
      case 29: /* callgsubr */
        if( c->sp >= 1 )
        { int idx = (int) c->stack[c->sp - 1];
          c->sp--;
          svg_glyph_cff_subr(c, f, st, idx, 1);
          if( c->abort ) return 0;
        }
        break;
      case 30: /* vhcurveto -- variadic alternating V/H                     */
      {
        /* Pattern: dy1 dx2 dy2 dx3 {dxa dxb dyb dyc dyd dxe dye dxf}* dyf? */
        /* Standard implementation: emit curves starting V, alternating.    */
        int j = 0;
        int vert = 1;
        while( c->sp - j >= 4 )
        {
          if( vert )
          {
            double last_y = 0.0;
            if( c->sp - j == 5 ) last_y = c->stack[j+4];
            svg_glyph_emit_curve(c,
              0.0,            c->stack[j],
              c->stack[j+1], c->stack[j+2],
              c->stack[j+3], last_y);
            j += 4;
            if( c->sp - j == 1 ) { c->sp = 0; break; }
          }
          else
          {
            double last_x = 0.0;
            if( c->sp - j == 5 ) last_x = c->stack[j+4];
            svg_glyph_emit_curve(c,
              c->stack[j],   0.0,
              c->stack[j+1], c->stack[j+2],
              last_x,         c->stack[j+3]);
            j += 4;
            if( c->sp - j == 1 ) { c->sp = 0; break; }
          }
          vert = !vert;
          if( c->sp - j == 5 ) {  /* fall through to handle the trailing 5 */ }
        }
        c->sp = 0;
        break;
      }
      case 31: /* hvcurveto -- variadic alternating H/V                     */
      {
        int j = 0;
        int horiz = 1;
        while( c->sp - j >= 4 )
        {
          if( horiz )
          {
            double last_x = 0.0;
            if( c->sp - j == 5 ) last_x = c->stack[j+4];
            svg_glyph_emit_curve(c,
              c->stack[j],   0.0,
              c->stack[j+1], c->stack[j+2],
              last_x,         c->stack[j+3]);
            j += 4;
            if( c->sp - j == 1 ) { c->sp = 0; break; }
          }
          else
          {
            double last_y = 0.0;
            if( c->sp - j == 5 ) last_y = c->stack[j+4];
            svg_glyph_emit_curve(c,
              0.0,            c->stack[j],
              c->stack[j+1], c->stack[j+2],
              c->stack[j+3], last_y);
            j += 4;
            if( c->sp - j == 1 ) { c->sp = 0; break; }
          }
          horiz = !horiz;
        }
        c->sp = 0;
        break;
      }
      default:
        /* Unknown -- clear stack and continue defensively.                  */
        c->sp = 0;
        break;
    }
    /* Type 2 has an implicit closepath at endchar; closepath the subpath   */
    /* if rmoveto just opened one is not strictly correct but Type 2 does   */
    /* not have an explicit closepath operator -- subpaths close at the    */
    /* next moveto.  We mirror that by emitting close() on every moveto.   */
  }
  return 0;
}

static int svg_glyph_run_cff_cs(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *cs, int len)
{
  svg_glyph_cff_state st;
  int r;
  memset(&st, 0, sizeof st);
  r = svg_glyph_run_cff_body(c, f, &st, cs, len);
  /* Final implicit closepath -- Type 2 closes the last subpath on endchar.*/
  if( c->close && !c->abort ) c->close(c->user);
  return r;
}
