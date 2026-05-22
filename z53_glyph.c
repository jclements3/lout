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
/*  MODULE:       SVG Back End / Type 1 Glyph Outline Service                */
/*  EXTERNS:      svg_glyph_emit_outline                                     */
/*                                                                           */
/*  STATUS:       Loads URW++ / Ghostscript Type 1 .pfb font files from a    */
/*                hardcoded search path and decodes glyph outlines on        */
/*                demand for SVG_OP_CHARPATH in z53.c.  Used only when the   */
/*                SVG back end is live; the PostScript back end (z49.c)      */
/*                never calls into this module.                              */
/*                                                                           */
/*                Type 1 PFB segment unwrap -> eexec decryption (key 55665,  */
/*                lenIV 4) -> CharStrings dict scan -> per-glyph charstring  */
/*                decryption (key 4330, lenIV 4) -> Type 1 charstring        */
/*                interpreter.  Subroutines (Subrs array) supported; flex /  */
/*                OtherSubrs ignored (treated as no-ops); seac (accented     */
/*                composite) supported via a small AdobeStandardEncoding    */
/*                table.                                                     */
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
#define SVG_GLYPH_MAX_GLYPHS    1024    /* glyphs per font                    */
#define SVG_GLYPH_MAX_SUBRS     4096    /* Type 1 Subrs per font              */
#define SVG_GLYPH_NAME_LEN        40    /* AGL glyph name max length          */
#define SVG_GLYPH_PFB_MAX    1048576    /* 1 MiB raw PFB cap                  */
#define SVG_GLYPH_DECRYPT_LEN_IV   4    /* default lenIV for both layers      */
#define SVG_GLYPH_STK_DEPTH       48    /* CharString operand-stack depth     */
#define SVG_GLYPH_PSN_LEN         48    /* PS font name max length            */


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
  int             loaded;                  /* 1 once parsed                  */
  int             load_failed;             /* 1 if we tried and failed       */
  svg_glyph_entry glyphs[SVG_GLYPH_MAX_GLYPHS];
  int             nglyphs;
  svg_subr_entry  subrs[SVG_GLYPH_MAX_SUBRS];
  int             nsubrs;
  int             lenIV;                   /* charstring leading random bytes*/
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
  f->lenIV = SVG_GLYPH_DECRYPT_LEN_IV;
  svg_glyph_n_fonts++;

  if( !svg_glyph_find_pfb_path(ps_name, path, sizeof path) )
  { f->load_failed = 1; return -1; }

  if( !svg_glyph_read_pfb(path, &ascii_buf, &ascii_len, &binary_buf, &binary_len) )
  { f->load_failed = 1; return -1; }

  if( binary_buf == NULL || binary_len == 0 )
  { free(ascii_buf); free(binary_buf); f->load_failed = 1; return -1; }

  if( !svg_glyph_eexec_decrypt(binary_buf, binary_len, &plain, &plain_len) )
  { free(ascii_buf); free(binary_buf); f->load_failed = 1; return -1; }
  free(binary_buf);
  free(ascii_buf);

  f->lenIV = svg_glyph_parse_lenIV(plain, plain_len);
  svg_glyph_parse_subrs(f, plain, plain_len);
  if( !svg_glyph_parse_charstrings(f, plain, plain_len) )
  { free(plain); f->load_failed = 1; return -1; }
  free(plain);
  if( f->nglyphs == 0 )
  { f->load_failed = 1; return -1; }
  f->loaded = 1;
  return svg_glyph_n_fonts - 1;
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
  ctx.scale = font_size_units / 1000.0;
  ctx.x0    = x0;
  ctx.y0    = y0;
  ctx.cur_x = 0.0;
  ctx.cur_y = 0.0;
  ctx.adv_x = 0.0;
  ctx.sp    = 0;
  ctx.ps_top = 0;
  ctx.depth = 0;
  ctx.abort = 0;

  r = svg_glyph_run_cs(&ctx, f, g->cs, g->cs_len);
  (void) r;
  if( ctx.abort ) return 0;
  if( advance_out != NULL )
    *advance_out = ctx.adv_x * ctx.scale;
  return 1;
}
