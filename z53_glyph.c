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
/*  MODULE:       SVG Back End / Type 1 + CFF + TrueType Glyph Service       */
/*  EXTERNS:      svg_glyph_emit_outline                                     */
/*                                                                           */
/*  STATUS:       Three outline back ends behind one cache:                  */
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
/*                (c) TrueType .ttf (sfnt magic 0x00010000).  OT table     */
/*                    directory walk -> head (UnitsPerEm, loca format) +   */
/*                    maxp (numGlyphs) + cmap (format 4 + optional 12)     */
/*                    + loca + glyf.  Per-glyph glyf record decoded on     */
/*                    demand: simple outlines (contours of on-/off-curve   */
/*                    points) emitted as quadratic Beziers converted to    */
/*                    cubic via the standard P0 + 2/3(Q-P0), P2 + 2/3(Q-   */
/*                    P2) formula; composites resolved iteratively with    */
/*                    a small component stack (translation + optional      */
/*                    affine 2x2 matrix; scale / xy-scale / two-by-two    */
/*                    forms honoured).                                     */
/*                                                                           */
/*                All three share the same arena allocator, cache record,  */
/*                and public entry point (svg_glyph_emit_outline).          */
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
#define SVG_GLYPH_KIND_TTF  3

#define SVG_GLYPH_TTF_MAX_GLYPHS  65536    /* hard cap from u16 numGlyphs    */
#define SVG_GLYPH_TTF_RECURSE     8        /* composite-glyph recursion cap  */


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
  /* TrueType-only state.  The raw `glyf` table is kept inside the arena    */
  /* but referenced by *offset* (not pointer) because the arena may be      */
  /* realloc'd by subsequent allocations -- pointers stored here would      */
  /* dangle.  Same goes for the (ttf_n_glyphs + 1) loca offsets array.      */
  size_t          glyf_arena_off;
  size_t          glyf_len;
  size_t          glyf_off_arena_off;  /* offset to unsigned long[]         */
  int             ttf_n_glyphs;
  /* simple arena -- one malloc, grown as needed; freed at process exit (no  */
  /* explicit cleanup hook in this back end).  All cs / subrs pointers       */
  /* point inside arena.                                                     */
  unsigned char  *arena;
  size_t          arena_used;
  size_t          arena_cap;
  /* OpenType GSUB feature substitution maps (CFF/OTF only, phase 1).        */
  /* Indexed by Latin-1 codepoint (0..255); the value is the substituted     */
  /* glyph-id within the CFF, or 0 if no substitution.  Populated by         */
  /* svg_glyph_otf_parse_gsub() when the OTF carries an smcp/onum feature   */
  /* lookup of type 1 (single substitution).  Phase 1 records the table     */
  /* but does NOT yet consume it at <text> emission time -- see the         */
  /* comment on svg_glyph_otf_parse_gsub() for the architectural reason.    */
  unsigned short  smcp_subst[256];
  unsigned short  onum_subst[256];
  int             has_smcp;     /* 1 if any smcp_subst entry is nonzero    */
  int             has_onum;     /* 1 if any onum_subst entry is nonzero    */
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
/*  TrueType .ttf lookup.  Same shape as the OTF lookup: a PS-name -> file   */
/*  map for hand-curated aliases (DejaVu / Liberation / Noto), plus a list  */
/*  of system directories walked one level deep.  Both .ttf and .TTF are   */
/*  accepted by the probe.                                                  */
/*                                                                           */
/*  Override via LOUT_TTF_FONT_DIR env var (single dir prepended).          */
/*                                                                           */
/*****************************************************************************/

static const svg_glyph_map_entry svg_glyph_ttf_map[] = {
  /* DejaVu family (Debian/Ubuntu default).  PS names match GhostScript's   */
  /* DejaVu CIDFontInfo entries.                                             */
  { "DejaVuSans",                "DejaVuSans.ttf" },
  { "DejaVuSans-Bold",           "DejaVuSans-Bold.ttf" },
  { "DejaVuSans-Oblique",        "DejaVuSans-Oblique.ttf" },
  { "DejaVuSans-BoldOblique",    "DejaVuSans-BoldOblique.ttf" },
  { "DejaVuSerif",               "DejaVuSerif.ttf" },
  { "DejaVuSerif-Bold",          "DejaVuSerif-Bold.ttf" },
  { "DejaVuSerif-Italic",        "DejaVuSerif-Italic.ttf" },
  { "DejaVuSerif-BoldItalic",    "DejaVuSerif-BoldItalic.ttf" },
  { "DejaVuSansMono",            "DejaVuSansMono.ttf" },
  { "DejaVuSansMono-Bold",       "DejaVuSansMono-Bold.ttf" },
  { "DejaVuSansMono-Oblique",    "DejaVuSansMono-Oblique.ttf" },
  { "DejaVuSansMono-BoldOblique","DejaVuSansMono-BoldOblique.ttf" },
  /* Liberation -- metric-compatible with Times / Arial / Courier.          */
  { "LiberationSerif-Regular",   "LiberationSerif-Regular.ttf" },
  { "LiberationSerif-Bold",      "LiberationSerif-Bold.ttf" },
  { "LiberationSerif-Italic",    "LiberationSerif-Italic.ttf" },
  { "LiberationSerif-BoldItalic","LiberationSerif-BoldItalic.ttf" },
  { "LiberationSans-Regular",    "LiberationSans-Regular.ttf" },
  { "LiberationSans-Bold",       "LiberationSans-Bold.ttf" },
  { "LiberationSans-Italic",     "LiberationSans-Italic.ttf" },
  { "LiberationSans-BoldItalic", "LiberationSans-BoldItalic.ttf" },
  { "LiberationMono-Regular",    "LiberationMono-Regular.ttf" },
  { "LiberationMono-Bold",       "LiberationMono-Bold.ttf" },
  /* Noto Sans / Serif (most-common Linux distros).                          */
  { "NotoSans-Regular",          "NotoSans-Regular.ttf" },
  { "NotoSans-Bold",             "NotoSans-Bold.ttf" },
  { "NotoSerif-Regular",         "NotoSerif-Regular.ttf" },
  { "NotoSerif-Bold",            "NotoSerif-Bold.ttf" },
  { NULL, NULL }
};

static const char *svg_glyph_ttf_dir[] = {
  "/usr/share/fonts/truetype/",
  "/usr/share/fonts/TTF/",
  "/usr/local/share/fonts/truetype/",
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

/* Ensure the arena has capacity for at least `n` more bytes without        */
/* triggering a realloc on the next allocation.  Used by the TrueType       */
/* loader so per-glyph cs4 pointers stored during the registration loop     */
/* keep pointing at the same arena block.  Also called by the Type 1 and    */
/* CFF loaders to amortise growth and -- critically -- guarantee that the   */
/* arena buffer cannot move while subr/charstring pointers are being        */
/* recorded into f->subrs[] / f->gsubrs[] / f->glyphs[].cs.  See the        */
/* comment on glyf_arena_off in the struct definition: a realloc that      */
/* moves the arena leaves every previously stored pointer dangling.        */
static int svg_glyph_arena_reserve(svg_glyph_font *f, size_t extra)
{
  size_t want = f->arena_used + extra;
  if( want <= f->arena_cap ) return 1;
  { size_t newcap = f->arena_cap ? f->arena_cap : 16384;
    unsigned char *p;
    while( newcap < want ) newcap *= 2;
    p = (unsigned char *) realloc(f->arena, newcap);
    if( p == NULL ) return 0;
    f->arena = p;
    f->arena_cap = newcap;
    return 1;
  }
}

/* DEBUG-only sanity check: verify that every cs / subr / gsubr pointer       */
/* stored in `f` lies within the current arena [arena, arena+arena_used).     */
/* Run this at the end of each loader to catch the realloc-aliasing bug       */
/* class that PR #123 surfaced.  In release builds the body is empty and       */
/* the compiler folds the call away.                                          */
#if DEBUG_ON
static void svg_glyph_arena_audit(const svg_glyph_font *f, const char *where)
{
  const unsigned char *base = f->arena;
  const unsigned char *end  = base + f->arena_used;
  int i;
  if( base == NULL ) return;
  for( i = 0; i < f->nglyphs; i++ )
  { const unsigned char *p = f->glyphs[i].cs;
    if( p == NULL ) continue;
    if( p < base || p >= end ||
        p + f->glyphs[i].cs_len > end )
      fprintf(stderr,
        "lout: z53_glyph arena audit FAIL (%s): glyph[%d].cs out of range\n",
        where, i);
  }
  for( i = 0; i < f->nsubrs; i++ )
  { const unsigned char *p = f->subrs[i].cs;
    if( p == NULL ) continue;
    if( p < base || p >= end ||
        p + f->subrs[i].cs_len > end )
      fprintf(stderr,
        "lout: z53_glyph arena audit FAIL (%s): subr[%d].cs out of range\n",
        where, i);
  }
  for( i = 0; i < f->ngsubrs; i++ )
  { const unsigned char *p = f->gsubrs[i].cs;
    if( p == NULL ) continue;
    if( p < base || p >= end ||
        p + f->gsubrs[i].cs_len > end )
      fprintf(stderr,
        "lout: z53_glyph arena audit FAIL (%s): gsubr[%d].cs out of range\n",
        where, i);
  }
}
#else
#define svg_glyph_arena_audit(f, where) ((void) 0)
#endif


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
  /* Amortise growth and -- more importantly -- pin the arena: every plain   */
  /* charstring we drop in is <= its source bytes, so reserving the         */
  /* remaining post-/Subrs slice bounds the loop's total allocation and     */
  /* guarantees the subr pointers we store can't be invalidated mid-loop   */
  /* by a realloc that moves the underlying buffer.  Failure here is       */
  /* fatal: degrading to per-alloc growth would reintroduce the dangling-  */
  /* pointer bug that PR #123 fixed.                                       */
  if( !svg_glyph_arena_reserve(f, len - (size_t) off) ) return 0;
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
  /* See the matching comment in svg_glyph_parse_subrs.  Reserving the     */
  /* remaining post-/CharStrings slice keeps the arena stationary across   */
  /* the loop, so the f->glyphs[].cs pointers we record can't be          */
  /* invalidated by a later realloc.                                       */
  if( !svg_glyph_arena_reserve(f, len - (size_t) off) ) return 0;
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

/* Forward declarations for the CFF/OTF + TrueType loaders (full bodies     */
/* live below the Type 1 charstring interpreter).                            */
struct svg_glyph_emit_ctx;
static int svg_glyph_find_otf_path(const char *ps_name, char *out, size_t cap);
static int svg_glyph_load_otf(svg_glyph_font *f, const char *path);
static int svg_glyph_find_ttf_path(const char *ps_name, char *out, size_t cap);
static int svg_glyph_load_ttf(svg_glyph_font *f, const char *path);
static int svg_glyph_run_ttf(struct svg_glyph_emit_ctx *c, svg_glyph_font *f,
  int gid, int depth);

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
      { svg_glyph_arena_audit(f, "T1");
        free(plain); f->loaded = 1; return svg_glyph_n_fonts - 1; }
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
      svg_glyph_arena_audit(f, "CFF");
      return svg_glyph_n_fonts - 1;
    }
  }

  /* (c) Fall through to TrueType .ttf.                                      */
  if( svg_glyph_find_ttf_path(ps_name, path, sizeof path) )
  {
    /* Reset any partial state from prior failed attempts.                   */
    f->nglyphs    = 0;
    f->nsubrs     = 0;
    f->ngsubrs    = 0;
    f->arena_used = 0;
    if( svg_glyph_load_ttf(f, path) && f->nglyphs > 0 )
    {
      f->kind   = SVG_GLYPH_KIND_TTF;
      f->loaded = 1;
      svg_glyph_arena_audit(f, "TTF");
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
  else if( f->kind == SVG_GLYPH_KIND_TTF )
  {
    /* TrueType per-glyph: the `cs` slot holds a 4-byte little-endian       */
    /* glyph index, dereferenced via the in-arena glyf data + loca offsets */
    /* inside svg_glyph_run_ttf.                                            */
    int gid;
    if( g->cs_len != 4 || g->cs == NULL ) return 0;
    gid = (int) ((unsigned int) g->cs[0]
                | ((unsigned int) g->cs[1] <<  8)
                | ((unsigned int) g->cs[2] << 16)
                | ((unsigned int) g->cs[3] << 24));
    r = svg_glyph_run_ttf((struct svg_glyph_emit_ctx *) &ctx, f, gid, 0);
    /* TrueType outlines carry an implicit final closepath.                  */
    if( ctx.close && !ctx.abort ) ctx.close(ctx.user);
  }
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
/*  CFF / OpenType outline loader (OTTO-tagged container only).             */
/*                                                                           */
/*  TrueType `glyf` outlines (sfnt magic 0x00010000) are handled by the    */
/*  separate svg_glyph_load_ttf loader near the bottom of this file; the   */
/*  load-font dispatcher in svg_glyph_load_font tries .pfb, .otf, then    */
/*  .ttf in turn.                                                            */
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
  /*                                                                          */
  /* Per CFF 1.0 Appendix C, charsets 1 and 2 are *fixed GID->SID maps*       */
  /* baked into the spec (Expert: 166 entries; ExpertSubset: 87).  We don't   */
  /* embed the full tables here because: (a) the SIDs reference Adobe Expert  */
  /* encoding glyph names ("fl", "centsuperior", "Aacutesmall", ...) that     */
  /* svg_glyph_cff_std_sid() does not enumerate beyond ~390 anyway, and       */
  /* (b) Lout's font corpus is exclusively ISOAdobe-encoded -- no document    */
  /* in tests/ or examples/ links a font that declares charset 1 or 2.        */
  /*                                                                          */
  /* If a future font does, the glyphs beyond .notdef will resolve to NULL    */
  /* names and silently fall through to the AGL/cmap fast paths in the TTF    */
  /* loader (or be skipped); rendering will degrade gracefully rather than    */
  /* crash.  Tracked in NEXT_OPTIMIZATIONS.md (CFF Expert charset gap).       */
  if( charset_off >= 0 && charset_off <= 2 )
  {
    if( charset_off == 0 )
    {
      /* GID i -> SID i for i in [0..228].  Beyond that we leave 0 (.notdef)*/
      for( i = 0; i < n_glyphs; i++ )
        sid_for_gid[i] = (unsigned int) i;
      return 1;
    }
    /* Expert / ExpertSubset: see comment above.                              */
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
  /* Reserve the full INDEX payload up front: each cs we record points into  */
  /* the arena, and a mid-loop realloc would invalidate the pointers from   */
  /* earlier iterations (the bug class PR #123 found).                      */
  f->ngsubrs = (int) gsubr_ix.count;
  if( f->ngsubrs > SVG_GLYPH_MAX_GSUBRS ) f->ngsubrs = SVG_GLYPH_MAX_GSUBRS;
  if( !svg_glyph_arena_reserve(f, gsubr_ix.total_size) ) return 0;
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
  if( !svg_glyph_arena_reserve(f, lsubr_ix.total_size) ) return 0;
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
  /* Reserve the full CharStrings INDEX payload up front so the per-glyph    */
  /* dst pointers we record into f->glyphs[].cs cannot be invalidated by a  */
  /* mid-loop arena realloc.  See PR #123 for the failure mode.             */
  if( !svg_glyph_arena_reserve(f, cs_ix.total_size) )
  { free(sid_for_gid); return 0; }
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
/*  OpenType GSUB feature substitution parser.                                */
/*                                                                            */
/*  Scope:                                                                    */
/*    - Locates the `GSUB` table in the OT table directory.                  */
/*    - Walks Script/Feature/Lookup lists to find smcp / onum features.      */
/*    - Only Lookup Type 1 (Single Substitution) is implemented.  Other     */
/*      lookup types (alternate, ligature, contextual, chained-contextual)   */
/*      are skipped; this covers the common case where smcp and onum are    */
/*      compiled as one-to-one GID->GID maps.                                */
/*    - Records the substitutions as a GID->GID array.  Then walks the      */
/*      Adobe StandardEncoding to project Latin-1 codepoint -> source GID  */
/*      and writes the resulting codepoint-keyed table into                 */
/*      f->smcp_subst[256] / f->onum_subst[256].                            */
/*                                                                            */
/*  Phase 1 deferral note:                                                    */
/*    The SVG back-end currently emits text via <text> elements containing  */
/*    Unicode codepoints, NOT raw glyph-ids.  Small-caps glyphs and old-    */
/*    style figures live at glyph positions that have no Unicode codepoint  */
/*    of their own -- the only way to render them is to emit a path or to   */
/*    reference the glyph by id inside an SVG <font>/`@font-face` block.    */
/*    Until z53.c grows a glyph-path emission path for body text, the      */
/*    substitution table is built but never consumed.  The public API      */
/*    (svg_glyph_font_smcp_substitute / _onum_substitute) is provided so   */
/*    a future change can flip the consumer side on without re-touching    */
/*    the parser.                                                            */
/*****************************************************************************/

/* Locate the GSUB table inside the OT table directory.  Returns 1 on hit.  */
static int svg_glyph_find_gsub(const unsigned char *buf, size_t buf_len,
  size_t *gsub_off, size_t *gsub_len)
{
  unsigned int n_tables;
  size_t pos;
  unsigned int i;
  *gsub_off = 0;
  *gsub_len = 0;
  if( buf_len < 12 ) return 0;
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
    if( (size_t) offset > buf_len ||
        (size_t) offset + length > buf_len ) continue;
    if( tag == 0x47535542UL )   /* 'GSUB' */
    { *gsub_off = offset; *gsub_len = length; return 1; }
  }
  return 0;
}

/* Callback context for coverage walks below.                              */
struct svg_glyph_gsub_ctx {
  const unsigned char *g;     /* GSUB body                                 */
  size_t        g_len;
  size_t        subtable_off; /* offset of the SingleSubst subtable        */
  unsigned int  fmt;          /* 1 or 2                                    */
  int           delta;        /* fmt 1: GID delta                          */
  size_t        sub_arr_off;  /* fmt 2: offset of Substitute[] array       */
  unsigned int  sub_arr_n;    /* fmt 2: count of Substitute[]              */
  unsigned short *out_gid_map; /* GID-indexed substitution result          */
  int           out_cap;      /* size of out_gid_map (typically nglyphs)   */
};

static void svg_glyph_single_subst_cb(unsigned int gid, unsigned int idx,
  void *ud)
{
  struct svg_glyph_gsub_ctx *c = (struct svg_glyph_gsub_ctx *) ud;
  int sub_gid = 0;
  if( (int) gid >= c->out_cap ) return;
  if( c->fmt == 1 )
  {
    /* delta is signed, wraps modulo 65536 per the OT spec.                */
    sub_gid = ((int) gid + c->delta) & 0xFFFF;
  }
  else if( c->fmt == 2 )
  {
    if( idx >= c->sub_arr_n ) return;
    if( c->sub_arr_off + (size_t) (idx + 1) * 2 > c->g_len ) return;
    sub_gid = (int) svg_glyph_be_u16(c->g + c->sub_arr_off + (size_t) idx * 2);
  }
  if( sub_gid > 0 && sub_gid < c->out_cap )
    c->out_gid_map[gid] = (unsigned short) sub_gid;
}

/* Walk a GSUB Coverage table at `cov_off` (relative to GSUB body) and call */
/* the callback for each (covered_gid, coverage_index) pair.  Returns 1 on */
/* success.  Used by the Lookup Type 1 walker below.                       */
static int svg_glyph_walk_coverage(const unsigned char *g, size_t g_len,
  size_t cov_off,
  void (*cb)(unsigned int gid, unsigned int idx, void *ud), void *ud)
{
  unsigned int fmt;
  if( cov_off + 4 > g_len ) return 0;
  fmt = svg_glyph_be_u16(g + cov_off);
  if( fmt == 1 )
  {
    unsigned int n = svg_glyph_be_u16(g + cov_off + 2);
    size_t p = cov_off + 4;
    unsigned int j;
    if( p + (size_t) n * 2 > g_len ) return 0;
    for( j = 0; j < n; j++ )
    { unsigned int gid = svg_glyph_be_u16(g + p + (size_t) j * 2);
      cb(gid, j, ud);
    }
    return 1;
  }
  if( fmt == 2 )
  {
    unsigned int n = svg_glyph_be_u16(g + cov_off + 2);
    size_t p = cov_off + 4;
    unsigned int j;
    if( p + (size_t) n * 6 > g_len ) return 0;
    for( j = 0; j < n; j++ )
    { const unsigned char *r = g + p + (size_t) j * 6;
      unsigned int start = svg_glyph_be_u16(r);
      unsigned int end   = svg_glyph_be_u16(r + 2);
      unsigned int sidx  = svg_glyph_be_u16(r + 4);
      unsigned int gid;
      if( end < start ) continue;
      for( gid = start; gid <= end; gid++ )
        cb(gid, sidx + (gid - start), ud);
    }
    return 1;
  }
  return 0;
}

/* Apply a single GSUB lookup (type 1) into out_gid_map.  Returns 1 on    */
/* success (parsed cleanly), 0 on malformed input.  Non-type-1 lookups   */
/* are silently skipped (caller treats as a no-op).                       */
static int svg_glyph_apply_lookup(const unsigned char *g, size_t g_len,
  size_t lookup_off, unsigned short *out_gid_map, int out_cap)
{
  unsigned int lookup_type, sub_count;
  size_t p;
  unsigned int i;
  if( lookup_off + 6 > g_len ) return 0;
  lookup_type = svg_glyph_be_u16(g + lookup_off);
  /* lookup_flag at +2; mark_filter at +4 (skip).                          */
  sub_count   = svg_glyph_be_u16(g + lookup_off + 4);
  if( lookup_type != 1 ) return 1;  /* only Single Substitution in phase 1 */
  p = lookup_off + 6;
  if( p + (size_t) sub_count * 2 > g_len ) return 0;
  for( i = 0; i < sub_count; i++ )
  {
    size_t st_off = lookup_off + (size_t) svg_glyph_be_u16(g + p + (size_t) i * 2);
    unsigned int fmt;
    struct svg_glyph_gsub_ctx ctx;
    if( st_off + 6 > g_len ) continue;
    fmt = svg_glyph_be_u16(g + st_off);
    ctx.g            = g;
    ctx.g_len        = g_len;
    ctx.subtable_off = st_off;
    ctx.fmt          = fmt;
    ctx.delta        = 0;
    ctx.sub_arr_off  = 0;
    ctx.sub_arr_n    = 0;
    ctx.out_gid_map  = out_gid_map;
    ctx.out_cap      = out_cap;
    if( fmt == 1 )
    {
      size_t cov = st_off + (size_t) svg_glyph_be_u16(g + st_off + 2);
      short delta_s = (short) svg_glyph_be_u16(g + st_off + 4);
      ctx.delta = (int) delta_s;
      svg_glyph_walk_coverage(g, g_len, cov, svg_glyph_single_subst_cb, &ctx);
    }
    else if( fmt == 2 )
    {
      size_t cov = st_off + (size_t) svg_glyph_be_u16(g + st_off + 2);
      unsigned int n = svg_glyph_be_u16(g + st_off + 4);
      if( st_off + 6 + (size_t) n * 2 > g_len ) continue;
      ctx.sub_arr_off = st_off + 6;
      ctx.sub_arr_n   = n;
      svg_glyph_walk_coverage(g, g_len, cov, svg_glyph_single_subst_cb, &ctx);
    }
  }
  return 1;
}

/* Resolve a single GSUB feature (by tag) into a GID->GID substitution    */
/* map.  Walks the default-language-system of the first script in the    */
/* GSUB ScriptList (i.e. uses the script's default language).  This is   */
/* sufficient for the Latin-only fonts shipped in the Adobe base-35 and  */
/* common Linux desktop spins; multi-script fonts (CJK, etc.) are out of  */
/* phase 1 scope.  Returns 1 if any substitution was recorded.            */
static int svg_glyph_resolve_feature(const unsigned char *g, size_t g_len,
  unsigned long want_tag, unsigned short *out_gid_map, int out_cap)
{
  size_t script_list, feature_list, lookup_list;
  unsigned int n_scripts, n_features, n_lookups;
  size_t pos;
  unsigned int i;
  unsigned int *want_lookup_idxs = NULL;
  unsigned int want_n = 0;
  int any = 0;
  if( g_len < 10 ) return 0;
  /* Header: majorVersion(u16) minorVersion(u16)                          */
  /*         scriptListOffset(u16) featureListOffset(u16) lookupListOffset(u16) */
  script_list  = (size_t) svg_glyph_be_u16(g + 4);
  feature_list = (size_t) svg_glyph_be_u16(g + 6);
  lookup_list  = (size_t) svg_glyph_be_u16(g + 8);
  if( script_list  >= g_len || feature_list >= g_len ||
      lookup_list  >= g_len ) return 0;

  /* ScriptList: u16 scriptCount, then scriptCount * (Tag(4) + offset(u16)). */
  /* We take the first script's defaultLangSysOff to gather the feature    */
  /* indices that are active.                                              */
  n_scripts = svg_glyph_be_u16(g + script_list);
  if( n_scripts == 0 ) return 0;
  pos = script_list + 2;
  if( pos + 6 > g_len ) return 0;
  {
    size_t script_off = script_list +
      (size_t) svg_glyph_be_u16(g + pos + 4);
    size_t default_off;
    unsigned int n_feat_idx;
    size_t fp;
    unsigned int j;
    if( script_off + 4 > g_len ) return 0;
    default_off = (size_t) svg_glyph_be_u16(g + script_off);
    if( default_off == 0 ) return 0;
    default_off += script_off;
    if( default_off + 4 > g_len ) return 0;
    /* defaultLangSys: u16 lookupOrderOff (=0) + u16 requiredFeatureIdx +  */
    /*                 u16 featureIndexCount + u16[]                       */
    n_feat_idx = svg_glyph_be_u16(g + default_off + 4);
    fp = default_off + 6;
    if( fp + (size_t) n_feat_idx * 2 > g_len ) return 0;
    /* FeatureList: u16 featureCount + featureCount*(Tag(4) + offset(u16)).*/
    if( feature_list + 2 > g_len ) return 0;
    n_features = svg_glyph_be_u16(g + feature_list);
    for( j = 0; j < n_feat_idx; j++ )
    {
      unsigned int fi = svg_glyph_be_u16(g + fp + (size_t) j * 2);
      size_t frec, foff;
      unsigned long ftag;
      if( fi >= n_features ) continue;
      frec = feature_list + 2 + (size_t) fi * 6;
      if( frec + 6 > g_len ) continue;
      ftag = svg_glyph_be_u32(g + frec);
      if( ftag != want_tag ) continue;
      foff = feature_list + (size_t) svg_glyph_be_u16(g + frec + 4);
      if( foff + 4 > g_len ) continue;
      /* Feature: u16 featureParamsOff + u16 lookupIndexCount + u16[]      */
      want_n = svg_glyph_be_u16(g + foff + 2);
      if( foff + 4 + (size_t) want_n * 2 > g_len ) { want_n = 0; continue; }
      want_lookup_idxs = (unsigned int *) malloc(sizeof(unsigned int) * want_n);
      if( want_lookup_idxs == NULL ) { want_n = 0; return 0; }
      { unsigned int k;
        for( k = 0; k < want_n; k++ )
          want_lookup_idxs[k] = svg_glyph_be_u16(g + foff + 4 + (size_t) k * 2);
      }
      break;
    }
  }
  if( want_lookup_idxs == NULL || want_n == 0 )
  { if( want_lookup_idxs != NULL ) free(want_lookup_idxs);
    return 0;
  }

  /* LookupList: u16 lookupCount + lookupCount * u16 offset.               */
  if( lookup_list + 2 > g_len ) { free(want_lookup_idxs); return 0; }
  n_lookups = svg_glyph_be_u16(g + lookup_list);
  for( i = 0; i < want_n; i++ )
  {
    unsigned int li = want_lookup_idxs[i];
    size_t lk_off;
    if( li >= n_lookups ) continue;
    if( lookup_list + 2 + (size_t) li * 2 + 2 > g_len ) continue;
    lk_off = lookup_list +
      (size_t) svg_glyph_be_u16(g + lookup_list + 2 + (size_t) li * 2);
    if( svg_glyph_apply_lookup(g, g_len, lk_off, out_gid_map, out_cap) )
      any = 1;
  }
  free(want_lookup_idxs);
  return any;
}

/* Resolve a glyph name to its GID via f->glyphs[].  Returns -1 if absent. */
static int svg_glyph_name_to_gid(const svg_glyph_font *f, const char *name)
{
  int i;
  if( name == NULL || name[0] == 0 ) return -1;
  for( i = 0; i < f->nglyphs; i++ )
    if( strcmp(f->glyphs[i].name, name) == 0 ) return i;
  return -1;
}

/* Project a GID->GID map into a Latin-1 codepoint -> GID map via the     */
/* StandardEncoding glyph names.  Only nonzero entries are written.        */
static void svg_glyph_project_subst(const svg_glyph_font *f,
  const unsigned short *gid_map, int gid_cap, unsigned short *out_cp)
{
  unsigned int cp;
  for( cp = 0; cp < 256; cp++ )
  {
    const char *gname = svg_glyph_std_enc[cp];
    int gid;
    if( gname == NULL ) continue;
    gid = svg_glyph_name_to_gid(f, gname);
    if( gid < 0 || gid >= gid_cap ) continue;
    if( gid_map[gid] != 0 ) out_cp[cp] = gid_map[gid];
  }
}

/* Top-level GSUB parser, called once per OTF after svg_glyph_load_cff.    */
/* Populates f->smcp_subst[] / f->onum_subst[] and sets has_smcp/has_onum. */
static void svg_glyph_otf_parse_gsub(svg_glyph_font *f,
  const unsigned char *raw, size_t raw_len)
{
  size_t gsub_off, gsub_len;
  unsigned short *gid_map = NULL;
  int gid_cap;
  if( f->nglyphs <= 0 ) return;
  if( !svg_glyph_find_gsub(raw, raw_len, &gsub_off, &gsub_len) ) return;
  if( gsub_len < 10 ) return;
  gid_cap = f->nglyphs;
  if( gid_cap > 65536 ) gid_cap = 65536;
  gid_map = (unsigned short *) calloc((size_t) gid_cap, sizeof *gid_map);
  if( gid_map == NULL ) return;

  /* smcp = 0x736D6370 ('s','m','c','p')                                  */
  if( svg_glyph_resolve_feature(raw + gsub_off, gsub_len,
        0x736D6370UL, gid_map, gid_cap) )
  {
    svg_glyph_project_subst(f, gid_map, gid_cap, f->smcp_subst);
    { int i;
      for( i = 0; i < 256; i++ )
        if( f->smcp_subst[i] != 0 ) { f->has_smcp = 1; break; }
    }
  }

  /* Reset for the next feature.                                          */
  memset(gid_map, 0, (size_t) gid_cap * sizeof *gid_map);

  /* onum = 0x6F6E756D ('o','n','u','m')                                  */
  if( svg_glyph_resolve_feature(raw + gsub_off, gsub_len,
        0x6F6E756DUL, gid_map, gid_cap) )
  {
    svg_glyph_project_subst(f, gid_map, gid_cap, f->onum_subst);
    { int i;
      for( i = 0; i < 256; i++ )
        if( f->onum_subst[i] != 0 ) { f->has_onum = 1; break; }
    }
  }
  free(gid_map);
}


/*****************************************************************************/
/*  Public API: GSUB feature substitution lookup.                            */
/*                                                                            */
/*  These two functions are the consumer-side entry points for OpenType       */
/*  small-caps (smcp) and old-style figures (onum) substitutions.  They       */
/*  return the substituted glyph-id (within the font's CFF charset) for the   */
/*  given Latin-1 codepoint, or 0 if no substitution exists (font lacks the   */
/*  feature, is not CFF/OTF, the codepoint is unmapped, or the substitution   */
/*  uses a Lookup Type beyond Type 1).                                        */
/*                                                                            */
/*  Phase 1 status: the parser is live but no SVG emission path consumes the */
/*  result yet.  See the deferral note above svg_glyph_otf_parse_gsub() and  */
/*  lout/SVG_PORTING.md for the architectural blocker (small-caps glyphs    */
/*  have no Unicode codepoint, so the current <text>-based emission cannot   */
/*  reference them; glyph-path emission is the planned consumer).            */
/*****************************************************************************/

int svg_glyph_font_smcp_substitute(const char *ps_name, unsigned int cp)
{
  int fi;
  if( ps_name == NULL || cp >= 256 ) return 0;
  fi = svg_glyph_load_font(ps_name);
  if( fi < 0 ) return 0;
  if( !svg_glyph_fonts[fi].has_smcp ) return 0;
  return (int) svg_glyph_fonts[fi].smcp_subst[cp];
}

int svg_glyph_font_onum_substitute(const char *ps_name, unsigned int cp)
{
  int fi;
  if( ps_name == NULL || cp >= 256 ) return 0;
  fi = svg_glyph_load_font(ps_name);
  if( fi < 0 ) return 0;
  if( !svg_glyph_fonts[fi].has_onum ) return 0;
  return (int) svg_glyph_fonts[fi].onum_subst[cp];
}

int svg_glyph_font_has_feature(const char *ps_name, const char *tag4)
{
  int fi;
  if( ps_name == NULL || tag4 == NULL ) return 0;
  fi = svg_glyph_load_font(ps_name);
  if( fi < 0 ) return 0;
  if( strcmp(tag4, "smcp") == 0 ) return svg_glyph_fonts[fi].has_smcp;
  if( strcmp(tag4, "onum") == 0 ) return svg_glyph_fonts[fi].has_onum;
  return 0;
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
  /* Best-effort GSUB parse for smcp / onum.  Failures are silent: the      */
  /* font simply ends up with empty substitution tables.  This must run    */
  /* AFTER svg_glyph_load_cff because the projection step looks glyphs up   */
  /* by name in f->glyphs[].                                                 */
  if( ok ) svg_glyph_otf_parse_gsub(f, raw, (size_t) fsize);
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
        case 26: /* sqrt -- pop 1, push sqrt(top); negative input -> 0     */
          if( c->sp >= 1 )
          { double a = c->stack[c->sp - 1];
            if( a <= 0.0 )
              c->stack[c->sp - 1] = 0.0;
            else
            { /* Newton-Raphson; avoids pulling in libm.                    */
              double y = a * 0.5 + 0.5;
              int it;
              for( it = 0; it < 16; it++ ) y = 0.5 * (y + a / y);
              c->stack[c->sp - 1] = y;
            }
          }
          break;
        case 33: /* setcurrentpoint -- Type 1 op, ignored in Type 2          */
        default:
          /* Reserved or unknown escape op: clear stack defensively.         */
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
        /* Reserved Type 2 one-byte ops (0, 2, 9, 13, 15-17, 28, 32) and    */
        /* anything else: clear stack and continue defensively.  None of   */
        /* the corpus Lout emits exercises these.                            */
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


/*****************************************************************************/
/*                                                                           */
/*  TrueType (`glyf` table) outline support.                                 */
/*                                                                           */
/*  Reference: Apple TrueType Reference Manual / Microsoft OpenType `glyf`. */
/*                                                                           */
/*  Format detection: the leading 4-byte sfnt magic is 0x00010000.  We      */
/*  share the OT table directory walk with the CFF code path but accept    */
/*  this alternative magic in svg_glyph_parse_tt_dir below.                  */
/*                                                                           */
/*  Glyph naming: the back end calls us with Adobe glyph names ("A", "a",  */
/*  "exclam", ...).  We walk the TTF cmap (format 4 for BMP, format 12     */
/*  for supplementary planes) to build a Unicode-codepoint -> GID array,   */
/*  then use a static reverse-AGL table to translate the names we know     */
/*  about into codepoints.  Names not in our reverse-AGL table fall back   */
/*  to the .notdef rectangle (handled upstream by the bbox path).          */
/*                                                                           */
/*****************************************************************************/

/* Reverse Adobe Glyph List entries we care about.  Mirrors the forward    */
/* table in svg_ascii_glyph_name (z53.c) so charpath consumers find every  */
/* glyph the PS source can request.                                        */
typedef struct {
  const char    *name;
  unsigned int   cp;
} svg_glyph_agl_rev;

static const svg_glyph_agl_rev svg_glyph_agl_rev_table[] = {
  /* ASCII letters and digits handled via fast-path in name->cp below.      */
  { "space",         0x0020 }, { "exclam",        0x0021 },
  { "quotedbl",      0x0022 }, { "numbersign",    0x0023 },
  { "dollar",        0x0024 }, { "percent",       0x0025 },
  { "ampersand",     0x0026 }, { "quoteright",    0x2019 },
  { "parenleft",     0x0028 }, { "parenright",    0x0029 },
  { "asterisk",      0x002a }, { "plus",          0x002b },
  { "comma",         0x002c }, { "hyphen",        0x002d },
  { "period",        0x002e }, { "slash",         0x002f },
  { "colon",         0x003a }, { "semicolon",     0x003b },
  { "less",          0x003c }, { "equal",         0x003d },
  { "greater",       0x003e }, { "question",      0x003f },
  { "at",            0x0040 }, { "bracketleft",   0x005b },
  { "backslash",     0x005c }, { "bracketright",  0x005d },
  { "asciicircum",   0x005e }, { "underscore",    0x005f },
  { "quoteleft",     0x2018 }, { "braceleft",     0x007b },
  { "bar",           0x007c }, { "braceright",    0x007d },
  { "asciitilde",    0x007e },
  /* Accents (used by seac and by @Graph callers).                          */
  { "grave",         0x0060 }, { "acute",         0x00b4 },
  { "circumflex",    0x02c6 }, { "tilde",         0x02dc },
  { "macron",        0x00af }, { "breve",         0x02d8 },
  { "dotaccent",     0x02d9 }, { "dieresis",      0x00a8 },
  { "ring",          0x02da }, { "cedilla",       0x00b8 },
  { "hungarumlaut",  0x02dd }, { "ogonek",        0x02db },
  { "caron",         0x02c7 },
  /* Common punctuation/quotation mark variants.                            */
  { "endash",        0x2013 }, { "emdash",        0x2014 },
  { "bullet",        0x2022 }, { "quotedblleft",  0x201c },
  { "quotedblright", 0x201d }, { "quotesinglbase",0x201a },
  { "quotedblbase",  0x201e }, { "ellipsis",      0x2026 },
  { "dagger",        0x2020 }, { "daggerdbl",     0x2021 },
  { "perthousand",   0x2030 }, { "questiondown",  0x00bf },
  { "exclamdown",    0x00a1 },
  { NULL,            0 }
};

/*****************************************************************************/
/*  TrueType-flavoured OT table directory parser.  Returns table offsets    */
/*  via out-params; missing tables set offset=0.  Accepts the 0x00010000    */
/*  (Apple sfnt) and 'true' (NeXT) and 'typ1' (legacy) magics.              */
/*****************************************************************************/

typedef struct {
  size_t        off;
  size_t        len;
} svg_glyph_tt_table;

typedef struct {
  svg_glyph_tt_table head, maxp, loca, glyf, cmap, hhea, hmtx, post;
  int               units_per_em;
  int               index_to_loc_format;  /* 0=short, 1=long                */
} svg_glyph_tt_dir;

static int svg_glyph_parse_tt_dir(const unsigned char *buf, size_t buf_len,
  svg_glyph_tt_dir *out)
{
  unsigned long magic;
  unsigned int n_tables;
  size_t pos;
  unsigned int i;

  memset(out, 0, sizeof *out);
  out->units_per_em = 1000;
  if( buf_len < 12 ) return 0;
  magic = svg_glyph_be_u32(buf);
  /* 0x00010000 = TrueType sfnt; 'true' = legacy Apple; 'typ1' = legacy.     */
  if( magic != 0x00010000UL &&
      magic != 0x74727565UL &&   /* 'true' */
      magic != 0x74797031UL )    /* 'typ1' */
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
    if( (size_t) offset > buf_len ||
        (size_t) offset + length > buf_len ) continue;
    switch( tag )
    {
      case 0x68656164UL: /* 'head' */
        out->head.off = (size_t) offset; out->head.len = (size_t) length;
        if( length >= 54 )
        { unsigned int upem;
          int itlf;
          upem = svg_glyph_be_u16(buf + (size_t) offset + 18);
          if( upem > 0 ) out->units_per_em = (int) upem;
          itlf = (int) svg_glyph_be_u16(buf + (size_t) offset + 50);
          out->index_to_loc_format = (itlf != 0) ? 1 : 0;
        }
        break;
      case 0x6d617870UL: /* 'maxp' */
        out->maxp.off = (size_t) offset; out->maxp.len = (size_t) length;
        break;
      case 0x6c6f6361UL: /* 'loca' */
        out->loca.off = (size_t) offset; out->loca.len = (size_t) length;
        break;
      case 0x676c7966UL: /* 'glyf' */
        out->glyf.off = (size_t) offset; out->glyf.len = (size_t) length;
        break;
      case 0x636d6170UL: /* 'cmap' */
        out->cmap.off = (size_t) offset; out->cmap.len = (size_t) length;
        break;
      case 0x68686561UL: /* 'hhea' */
        out->hhea.off = (size_t) offset; out->hhea.len = (size_t) length;
        break;
      case 0x686d7478UL: /* 'hmtx' */
        out->hmtx.off = (size_t) offset; out->hmtx.len = (size_t) length;
        break;
      case 0x706f7374UL: /* 'post' */
        out->post.off = (size_t) offset; out->post.len = (size_t) length;
        break;
      default:
        break;
    }
  }
  return out->head.off != 0 && out->maxp.off != 0 &&
         out->loca.off != 0 && out->glyf.off != 0 &&
         out->cmap.off != 0;
}


/*****************************************************************************/
/*  cmap parser.  Walks the encoding-record table for the highest-priority  */
/*  Unicode subtable we recognise (preferring format 12 over format 4).     */
/*  Fills cp_to_gid[cp] for every codepoint in [0, 0x10000) covered by the */
/*  selected subtable; the caller supplies an array sized 0x10000 (BMP).    */
/*****************************************************************************/

static int svg_glyph_parse_cmap(const unsigned char *buf, size_t buf_len,
  size_t cmap_off, size_t cmap_len,
  unsigned short *cp_to_gid /* sized 0x10000 */)
{
  unsigned int n_sub;
  unsigned int i;
  size_t best_off = 0;
  int best_fmt = -1;
  int found_unicode = 0;

  if( cmap_off + 4 > buf_len ) return 0;
  if( cmap_len < 4 ) return 0;
  n_sub = svg_glyph_be_u16(buf + cmap_off + 2);
  if( cmap_off + 4 + n_sub * 8 > buf_len ) return 0;

  /* First pass: pick the best encoding record.  Format-12 BMP+supp-Unicode  */
  /* > Format-4 Unicode BMP > Format-4 Windows BMP.                          */
  for( i = 0; i < n_sub; i++ )
  {
    const unsigned char *rec = buf + cmap_off + 4 + (size_t) i * 8;
    unsigned int platform_id = svg_glyph_be_u16(rec);
    unsigned int encoding_id = svg_glyph_be_u16(rec + 2);
    unsigned long sub_off    = svg_glyph_be_u32(rec + 4);
    size_t fmt_pos;
    unsigned int fmt;
    int is_unicode_bmp = 0;
    int is_unicode_full = 0;
    if( cmap_off + (size_t) sub_off + 2 > buf_len ) continue;
    fmt_pos = cmap_off + (size_t) sub_off;
    fmt = svg_glyph_be_u16(buf + fmt_pos);
    /* Unicode platform = 0; Microsoft = 3, Symbol = 3/0, UCS-2 = 3/1,      */
    /* UCS-4 = 3/10.                                                         */
    if( platform_id == 0 ) is_unicode_bmp = 1;
    if( platform_id == 0 && (encoding_id == 4 || encoding_id == 6) )
      is_unicode_full = 1;
    if( platform_id == 3 && encoding_id == 1 ) is_unicode_bmp = 1;
    if( platform_id == 3 && encoding_id == 10 ) is_unicode_full = 1;
    if( !is_unicode_bmp && !is_unicode_full ) continue;
    found_unicode = 1;
    /* Score: format 12 (full Unicode) outranks format 4 (BMP).              */
    if( fmt == 12 && best_fmt < 12 )
    { best_fmt = 12; best_off = fmt_pos; }
    else if( fmt == 4 && best_fmt < 4 )
    { best_fmt = 4; best_off = fmt_pos; }
  }
  (void) found_unicode;
  if( best_fmt < 0 ) return 0;

  memset(cp_to_gid, 0, 0x10000 * sizeof *cp_to_gid);

  if( best_fmt == 4 )
  {
    unsigned int seg_count_x2;
    unsigned int seg_count;
    size_t end_off, start_off, delta_off, range_off;
    unsigned int seg;
    size_t length;
    if( best_off + 14 > buf_len ) return 0;
    length = svg_glyph_be_u16(buf + best_off + 2);
    if( best_off + length > buf_len ) length = buf_len - best_off;
    seg_count_x2 = svg_glyph_be_u16(buf + best_off + 6);
    seg_count    = seg_count_x2 / 2;
    if( seg_count == 0 || seg_count > 16384 ) return 0;
    end_off   = best_off + 14;
    start_off = end_off + seg_count_x2 + 2;
    delta_off = start_off + seg_count_x2;
    range_off = delta_off + seg_count_x2;
    if( range_off + seg_count_x2 > buf_len ) return 0;
    for( seg = 0; seg < seg_count; seg++ )
    {
      unsigned int end_code   = svg_glyph_be_u16(buf + end_off   + seg * 2);
      unsigned int start_code = svg_glyph_be_u16(buf + start_off + seg * 2);
      int          id_delta   = (int) svg_glyph_be_u16(buf + delta_off + seg * 2);
      unsigned int id_range   = svg_glyph_be_u16(buf + range_off + seg * 2);
      unsigned int cp;
      /* id_delta is technically int16, ensure sign-extended.                 */
      if( id_delta >= 0x8000 ) id_delta -= 0x10000;
      if( start_code == 0xffff && end_code == 0xffff ) break;
      for( cp = start_code; cp <= end_code && cp <= 0xffff; cp++ )
      {
        unsigned int gid;
        if( id_range == 0 )
        { gid = (cp + (unsigned int) id_delta) & 0xffff; }
        else
        {
          /* idRangeOffset semantics: address = &idRangeOffset[seg] +       */
          /*   idRangeOffset[seg] + 2*(cp - startCode).                      */
          size_t addr = range_off + seg * 2
                      + id_range
                      + 2 * (size_t) (cp - start_code);
          if( addr + 2 > buf_len ) { gid = 0; }
          else
          { gid = svg_glyph_be_u16(buf + addr);
            if( gid != 0 ) gid = (gid + (unsigned int) id_delta) & 0xffff;
          }
        }
        if( gid != 0 ) cp_to_gid[cp] = (unsigned short) gid;
        if( cp == 0xffff ) break;
      }
    }
    return 1;
  }
  if( best_fmt == 12 )
  {
    unsigned long n_groups;
    unsigned long g;
    size_t groups_off;
    if( best_off + 16 > buf_len ) return 0;
    n_groups   = svg_glyph_be_u32(buf + best_off + 12);
    if( n_groups > 1000000UL ) return 0;
    groups_off = best_off + 16;
    if( groups_off + n_groups * 12 > buf_len ) return 0;
    for( g = 0; g < n_groups; g++ )
    {
      const unsigned char *rec = buf + groups_off + (size_t) g * 12;
      unsigned long start_cp = svg_glyph_be_u32(rec);
      unsigned long end_cp   = svg_glyph_be_u32(rec + 4);
      unsigned long start_gid= svg_glyph_be_u32(rec + 8);
      unsigned long cp;
      if( start_cp > 0xffff ) continue;   /* BMP only for now                */
      if( end_cp > 0xffff ) end_cp = 0xffff;
      for( cp = start_cp; cp <= end_cp; cp++ )
      {
        unsigned long gid = start_gid + (cp - start_cp);
        if( gid > 0xffff ) gid = 0;
        cp_to_gid[cp] = (unsigned short) gid;
      }
    }
    return 1;
  }
  return 0;
}


/*****************************************************************************/
/*  Parse the `loca` table into a u32 offsets array (n_glyphs+1 entries).   */
/*  Caller frees the returned malloc'd array.                                */
/*****************************************************************************/

static unsigned long *svg_glyph_parse_loca(const unsigned char *buf,
  size_t buf_len, const svg_glyph_tt_dir *dir, int n_glyphs)
{
  unsigned long *out;
  int i;
  size_t need;
  if( n_glyphs <= 0 || n_glyphs >= SVG_GLYPH_TTF_MAX_GLYPHS ) return NULL;
  out = (unsigned long *) malloc(sizeof *out * (size_t) (n_glyphs + 1));
  if( out == NULL ) return NULL;
  if( dir->index_to_loc_format == 0 )
  {
    need = (size_t) (n_glyphs + 1) * 2;
    if( dir->loca.off + need > buf_len ) { free(out); return NULL; }
    for( i = 0; i <= n_glyphs; i++ )
      out[i] = (unsigned long) svg_glyph_be_u16(
        buf + dir->loca.off + (size_t) i * 2) * 2UL;
  }
  else
  {
    need = (size_t) (n_glyphs + 1) * 4;
    if( dir->loca.off + need > buf_len ) { free(out); return NULL; }
    for( i = 0; i <= n_glyphs; i++ )
      out[i] = svg_glyph_be_u32(buf + dir->loca.off + (size_t) i * 4);
  }
  return out;
}


/*****************************************************************************/
/*                                                                           */
/*  glyf record decoder.  Walks one simple glyph's contour data and emits   */
/*  via the supplied move/line/curve/close callbacks (with a quadratic ->   */
/*  cubic conversion since the BACK_END interface advertises cubics).       */
/*  Composite glyphs recurse into svg_glyph_run_ttf with the supplied      */
/*  translation; affine 2x2 matrices are honoured if present.               */
/*                                                                           */
/*****************************************************************************/

/* glyf simple flags.                                                        */
#define SVG_TTF_FLG_ON_CURVE  0x01
#define SVG_TTF_FLG_XSHORT    0x02
#define SVG_TTF_FLG_YSHORT    0x04
#define SVG_TTF_FLG_REPEAT    0x08
#define SVG_TTF_FLG_XSAMEPOS  0x10
#define SVG_TTF_FLG_YSAMEPOS  0x20

/* glyf composite flags.  We honour the geometry-bearing flags (ARG_WORDS, */
/* ARGS_XY, HAVE_SCALE, HAVE_XY_SC, HAVE_TWO_X_2, MORE_COMPS).  The         */
/* renderer is unhinted, so WE_HAVE_INSTRUCTIONS (0x0100) is parsed only    */
/* to know we've finished components -- the trailing instruction stream     */
/* sits after the last component and we never read past it.                 */
/* OVERLAP_COMPOUND (0x0400) is a hint to fill-rule consumers; SVG's        */
/* default non-zero fill behaves correctly so we ignore it.                 */
/* USE_MY_METRICS / SCALED_COMPONENT_OFFSET / UNSCALED_COMPONENT_OFFSET     */
/* affect advance-width and translation-scaling subtleties not exercised    */
/* by the Lout corpus; we treat them as no-ops.                             */
#define SVG_TTF_CF_ARG_WORDS    0x0001
#define SVG_TTF_CF_ARGS_XY      0x0002
#define SVG_TTF_CF_HAVE_SCALE   0x0008
#define SVG_TTF_CF_MORE_COMPS   0x0020
#define SVG_TTF_CF_HAVE_XY_SC   0x0040
#define SVG_TTF_CF_HAVE_TWO_X_2 0x0080
#define SVG_TTF_CF_INSTR        0x0100
#define SVG_TTF_CF_OVERLAP      0x0400

/* The path-emission transform is a 2x2 matrix [a b; c d] + translation     */
/* (tx, ty), applied as (x', y') = (a*x + c*y + tx, b*x + d*y + ty).        */
typedef struct {
  double a, b, c, d;
  double tx, ty;
} svg_glyph_ttf_xform;

static double svg_glyph_ttf_2d14(int v)
{
  if( v >= 0x8000 ) v -= 0x10000;
  return (double) v / 16384.0;
}

static void svg_glyph_ttf_apply(const svg_glyph_ttf_xform *x,
  double xi, double yi, double *xo, double *yo)
{
  *xo = x->a * xi + x->c * yi + x->tx;
  *yo = x->b * xi + x->d * yi + x->ty;
}

/* Identity transform helper.                                                */
static void svg_glyph_ttf_identity(svg_glyph_ttf_xform *x)
{
  x->a = 1.0; x->b = 0.0; x->c = 0.0; x->d = 1.0;
  x->tx = 0.0; x->ty = 0.0;
}

/* Forward: simple-glyph emit.                                               */
static int svg_glyph_emit_simple(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *gd, size_t gd_len,
  int n_contours, const svg_glyph_ttf_xform *xf);

/* Forward: composite-glyph emit.                                            */
static int svg_glyph_emit_composite(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *gd, size_t gd_len, int depth,
  const svg_glyph_ttf_xform *xf);


static int svg_glyph_run_ttf(struct svg_glyph_emit_ctx *c_arg,
  svg_glyph_font *f, int gid, int depth)
{
  svg_glyph_emit_ctx *c = (svg_glyph_emit_ctx *) c_arg;
  unsigned long g_start, g_end;
  size_t gd_len;
  const unsigned char *gd;
  int n_contours;
  svg_glyph_ttf_xform xf;

  if( depth > SVG_GLYPH_TTF_RECURSE ) return 0;
  if( gid < 0 || gid >= f->ttf_n_glyphs ) return 0;
  if( f->arena == NULL ) return 0;
  {
    unsigned long *glyf_off_p =
      (unsigned long *) (f->arena + f->glyf_off_arena_off);
    g_start = glyf_off_p[gid];
    g_end   = glyf_off_p[gid + 1];
  }
  /* Two flavours of "empty" glyph that we must handle without erroring:    */
  /*   (a) zero-length glyf entry (loca[gid] == loca[gid+1]) -- common for  */
  /*       .notdef-equivalent or unmapped slots; emit nothing.              */
  /*   (b) a present 10-byte glyf header with numberOfContours == 0 --      */
  /*       handled in svg_glyph_emit_simple (see early return below).       */
  if( g_end <= g_start ) return 1;     /* case (a)                          */
  if( (size_t) g_end > f->glyf_len ) return 0;
  gd_len = (size_t) (g_end - g_start);
  if( gd_len < 10 ) return 0;
  gd = f->arena + f->glyf_arena_off + g_start;
  n_contours = (int) (signed short) svg_glyph_be_u16(gd);
  svg_glyph_ttf_identity(&xf);
  if( n_contours >= 0 )
    return svg_glyph_emit_simple(c, f, gd, gd_len, n_contours, &xf);
  return svg_glyph_emit_composite(c, f, gd, gd_len, depth, &xf);
}


/* Plot a single point (in design units) through xform and the emit ctx     */
/* scale/origin.                                                             */
static void svg_glyph_ttf_move(svg_glyph_emit_ctx *c,
  const svg_glyph_ttf_xform *xf, double x, double y)
{
  double xt, yt;
  svg_glyph_ttf_apply(xf, x, y, &xt, &yt);
  if( c->move ) c->move(c->user,
    c->x0 + xt * c->scale, c->y0 + yt * c->scale);
}

static void svg_glyph_ttf_line(svg_glyph_emit_ctx *c,
  const svg_glyph_ttf_xform *xf, double x, double y)
{
  double xt, yt;
  svg_glyph_ttf_apply(xf, x, y, &xt, &yt);
  if( c->line ) c->line(c->user,
    c->x0 + xt * c->scale, c->y0 + yt * c->scale);
}

/* Emit a quadratic bezier as an equivalent cubic.  P0 is the implicit      */
/* current point passed in by the caller (xp0, yp0).                        */
static void svg_glyph_ttf_quad(svg_glyph_emit_ctx *c,
  const svg_glyph_ttf_xform *xf,
  double xp0, double yp0, double xq, double yq, double xp1, double yp1)
{
  double c1x = xp0 + 2.0 / 3.0 * (xq - xp0);
  double c1y = yp0 + 2.0 / 3.0 * (yq - yp0);
  double c2x = xp1 + 2.0 / 3.0 * (xq - xp1);
  double c2y = yp1 + 2.0 / 3.0 * (yq - yp1);
  double X1, Y1, X2, Y2, X3, Y3;
  svg_glyph_ttf_apply(xf, c1x, c1y, &X1, &Y1);
  svg_glyph_ttf_apply(xf, c2x, c2y, &X2, &Y2);
  svg_glyph_ttf_apply(xf, xp1, yp1, &X3, &Y3);
  if( c->curve ) c->curve(c->user,
    c->x0 + X1 * c->scale, c->y0 + Y1 * c->scale,
    c->x0 + X2 * c->scale, c->y0 + Y2 * c->scale,
    c->x0 + X3 * c->scale, c->y0 + Y3 * c->scale);
}


static int svg_glyph_emit_simple(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *gd, size_t gd_len,
  int n_contours, const svg_glyph_ttf_xform *xf)
{
  size_t pos;
  unsigned int n_points;
  unsigned int instr_len;
  unsigned int i;
  unsigned short *end_pts = NULL;
  unsigned char  *flags   = NULL;
  double         *xs      = NULL;
  double         *ys      = NULL;
  unsigned char  *oncurve = NULL;
  int             contour;
  int             ok = 0;

  (void) f;
  if( n_contours == 0 ) return 1;
  if( n_contours > 4096 ) return 0;
  /* Header: i16 ncont + 4*i16 bbox = 10 bytes.                              */
  pos = 10;
  if( pos + (size_t) n_contours * 2 + 2 > gd_len ) return 0;
  end_pts = (unsigned short *) malloc(sizeof *end_pts * (size_t) n_contours);
  if( end_pts == NULL ) return 0;
  for( i = 0; i < (unsigned int) n_contours; i++ )
  {
    end_pts[i] = (unsigned short) svg_glyph_be_u16(gd + pos);
    pos += 2;
  }
  n_points = end_pts[n_contours - 1] + 1;
  if( n_points == 0 || n_points > 8192 ) { free(end_pts); return 0; }
  instr_len = svg_glyph_be_u16(gd + pos); pos += 2;
  if( pos + instr_len > gd_len ) { free(end_pts); return 0; }
  pos += instr_len;     /* skip hinting instructions                         */

  flags   = (unsigned char *)  malloc((size_t) n_points);
  xs      = (double *)         malloc(sizeof *xs * (size_t) n_points);
  ys      = (double *)         malloc(sizeof *ys * (size_t) n_points);
  oncurve = (unsigned char *)  malloc((size_t) n_points);
  if( flags == NULL || xs == NULL || ys == NULL || oncurve == NULL )
    goto cleanup;

  /* Decode flags (with repeat-byte expansion).                              */
  {
    unsigned int k = 0;
    while( k < n_points )
    {
      unsigned char fl;
      if( pos + 1 > gd_len ) goto cleanup;
      fl = gd[pos++];
      flags[k] = fl;
      oncurve[k] = (unsigned char) ((fl & SVG_TTF_FLG_ON_CURVE) ? 1 : 0);
      k++;
      if( fl & SVG_TTF_FLG_REPEAT )
      {
        unsigned int rep;
        if( pos + 1 > gd_len ) goto cleanup;
        rep = gd[pos++];
        while( rep > 0 && k < n_points )
        { flags[k] = fl;
          oncurve[k] = (unsigned char) ((fl & SVG_TTF_FLG_ON_CURVE) ? 1 : 0);
          k++;
          rep--;
        }
      }
    }
  }

  /* Decode X coords (deltas).                                               */
  {
    double cur = 0.0;
    unsigned int k;
    for( k = 0; k < n_points; k++ )
    {
      unsigned char fl = flags[k];
      if( fl & SVG_TTF_FLG_XSHORT )
      { int v;
        if( pos + 1 > gd_len ) goto cleanup;
        v = (int) gd[pos++];
        if( !(fl & SVG_TTF_FLG_XSAMEPOS) ) v = -v;
        cur += (double) v;
      }
      else if( !(fl & SVG_TTF_FLG_XSAMEPOS) )
      { int v;
        if( pos + 2 > gd_len ) goto cleanup;
        v = (int) (signed short) svg_glyph_be_u16(gd + pos);
        pos += 2;
        cur += (double) v;
      }
      /* else: x same as previous -- delta is zero.                          */
      xs[k] = cur;
    }
  }

  /* Decode Y coords (deltas).                                               */
  {
    double cur = 0.0;
    unsigned int k;
    for( k = 0; k < n_points; k++ )
    {
      unsigned char fl = flags[k];
      if( fl & SVG_TTF_FLG_YSHORT )
      { int v;
        if( pos + 1 > gd_len ) goto cleanup;
        v = (int) gd[pos++];
        if( !(fl & SVG_TTF_FLG_YSAMEPOS) ) v = -v;
        cur += (double) v;
      }
      else if( !(fl & SVG_TTF_FLG_YSAMEPOS) )
      { int v;
        if( pos + 2 > gd_len ) goto cleanup;
        v = (int) (signed short) svg_glyph_be_u16(gd + pos);
        pos += 2;
        cur += (double) v;
      }
      ys[k] = cur;
    }
  }

  /* Walk each contour and emit.  In TT, both endpoints of a contour can     */
  /* be off-curve, in which case an implicit on-curve point sits at their   */
  /* midpoint; the same rule applies between any two consecutive off-      */
  /* curves.  We materialise these implicit points into a local extended    */
  /* point list per contour and then walk it normally.                      */
  {
    int start = 0;
    for( contour = 0; contour < n_contours; contour++ )
    {
      int end = end_pts[contour];
      int len_c = end - start + 1;
      int ext_cap = len_c * 2 + 2;
      double *ex = NULL, *ey = NULL;
      unsigned char *eo = NULL;
      int ne = 0;
      int j;
      double sx, sy;
      double prev_x, prev_y;
      if( len_c <= 0 ) { start = end + 1; continue; }
      ex = (double *) malloc(sizeof *ex * (size_t) ext_cap);
      ey = (double *) malloc(sizeof *ey * (size_t) ext_cap);
      eo = (unsigned char *) malloc((size_t) ext_cap);
      if( ex == NULL || ey == NULL || eo == NULL )
      { free(ex); free(ey); free(eo); goto cleanup; }
      /* Step 1: synthesise the start point.  If point[start] is off-      */
      /* curve, the contour's start is the midpoint of the last and first */
      /* points (or just the first if both endpoints are on-curve).        */
      if( !oncurve[start] && !oncurve[end] )
      { sx = 0.5 * (xs[start] + xs[end]);
        sy = 0.5 * (ys[start] + ys[end]);
        ex[ne] = sx; ey[ne] = sy; eo[ne] = 1; ne++;
      }
      else if( !oncurve[start] && oncurve[end] )
      { sx = xs[end]; sy = ys[end];
        ex[ne] = sx; ey[ne] = sy; eo[ne] = 1; ne++;
      }
      else
      { sx = xs[start]; sy = ys[start];
        ex[ne] = sx; ey[ne] = sy; eo[ne] = 1; ne++;
      }
      /* Step 2: append remaining points, inserting implicit midpoints     */
      /* between two consecutive off-curve points.                         */
      for( j = (oncurve[start] ? start + 1 : start);
           j <= end; j++ )
      {
        if( ne > 1 && eo[ne-1] == 0 && oncurve[j] == 0 )
        {
          /* implicit on-curve midpoint                                    */
          double mx = 0.5 * (ex[ne-1] + xs[j]);
          double my = 0.5 * (ey[ne-1] + ys[j]);
          ex[ne] = mx; ey[ne] = my; eo[ne] = 1; ne++;
        }
        if( ne >= ext_cap )
        { /* grow */
          int new_cap = ext_cap * 2;
          double *nx = (double *) realloc(ex, sizeof *nx * (size_t) new_cap);
          double *ny = (double *) realloc(ey, sizeof *ny * (size_t) new_cap);
          unsigned char *no =
            (unsigned char *) realloc(eo, (size_t) new_cap);
          if( nx == NULL || ny == NULL || no == NULL )
          { free(nx ? nx : ex); free(ny ? ny : ey); free(no ? no : eo);
            goto cleanup; }
          ex = nx; ey = ny; eo = no; ext_cap = new_cap;
        }
        ex[ne] = xs[j]; ey[ne] = ys[j];
        eo[ne] = (unsigned char) (oncurve[j] ? 1 : 0);
        ne++;
      }

      /* Step 3: emit MoveTo to first on-curve point, then walk.            */
      svg_glyph_ttf_move(c, xf, ex[0], ey[0]);
      prev_x = ex[0];  prev_y = ey[0];
      j = 1;
      while( j < ne )
      {
        if( eo[j] )                  /* on-curve -> straight line          */
        {
          svg_glyph_ttf_line(c, xf, ex[j], ey[j]);
          prev_x = ex[j]; prev_y = ey[j];
          j++;
        }
        else                          /* off-curve -> quadratic            */
        {
          double qx = ex[j], qy = ey[j];
          double tx, ty;
          if( j + 1 < ne )
          { tx = ex[j+1]; ty = ey[j+1]; }
          else
          /* End of extended list -- close back onto start with a quad.    */
          { tx = ex[0]; ty = ey[0]; }
          svg_glyph_ttf_quad(c, xf, prev_x, prev_y, qx, qy, tx, ty);
          prev_x = tx; prev_y = ty;
          j += 2;
        }
      }
      /* TT contours implicitly close back to the first on-curve point.    */
      if( c->close ) c->close(c->user);
      free(ex); free(ey); free(eo);
      start = end + 1;
    }
  }
  ok = 1;

cleanup:
  free(end_pts);
  free(flags);
  free(xs);
  free(ys);
  free(oncurve);
  return ok;
}


static int svg_glyph_emit_composite(svg_glyph_emit_ctx *c, svg_glyph_font *f,
  const unsigned char *gd, size_t gd_len, int depth,
  const svg_glyph_ttf_xform *parent_xf)
{
  size_t pos = 10;       /* skip header                                     */
  unsigned int flags;
  int more = 1;
  while( more )
  {
    unsigned int sub_gid;
    int arg1, arg2;
    svg_glyph_ttf_xform xf;
    /* Per-Microsoft spec: components are 16-bit aligned packets.            */
    if( pos + 4 > gd_len ) return 0;
    flags   = svg_glyph_be_u16(gd + pos);     pos += 2;
    sub_gid = svg_glyph_be_u16(gd + pos);     pos += 2;
    if( flags & SVG_TTF_CF_ARG_WORDS )
    {
      if( pos + 4 > gd_len ) return 0;
      arg1 = (int) (signed short) svg_glyph_be_u16(gd + pos); pos += 2;
      arg2 = (int) (signed short) svg_glyph_be_u16(gd + pos); pos += 2;
    }
    else
    {
      if( pos + 2 > gd_len ) return 0;
      arg1 = (int) (signed char) gd[pos++];
      arg2 = (int) (signed char) gd[pos++];
    }
    /* Build the per-component transform.                                    */
    svg_glyph_ttf_identity(&xf);
    if( flags & SVG_TTF_CF_HAVE_SCALE )
    {
      if( pos + 2 > gd_len ) return 0;
      { double s = svg_glyph_ttf_2d14(
          (int) svg_glyph_be_u16(gd + pos)); pos += 2;
        xf.a = s; xf.d = s; }
    }
    else if( flags & SVG_TTF_CF_HAVE_XY_SC )
    {
      if( pos + 4 > gd_len ) return 0;
      xf.a = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
      xf.d = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
    }
    else if( flags & SVG_TTF_CF_HAVE_TWO_X_2 )
    {
      if( pos + 8 > gd_len ) return 0;
      xf.a = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
      xf.b = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
      xf.c = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
      xf.d = svg_glyph_ttf_2d14((int) svg_glyph_be_u16(gd + pos)); pos += 2;
    }
    /* Translation (only the ARGS_ARE_XY_VALUES interpretation; the "match  */
    /* points" alternative is rare in real fonts and we treat it as zero    */
    /* translation if not explicitly XY values).                            */
    if( flags & SVG_TTF_CF_ARGS_XY )
    { xf.tx = (double) arg1; xf.ty = (double) arg2; }

    /* Compose with parent transform: child first, then parent.              */
    { svg_glyph_ttf_xform composed;
      composed.a  = parent_xf->a * xf.a + parent_xf->c * xf.b;
      composed.b  = parent_xf->b * xf.a + parent_xf->d * xf.b;
      composed.c  = parent_xf->a * xf.c + parent_xf->c * xf.d;
      composed.d  = parent_xf->b * xf.c + parent_xf->d * xf.d;
      composed.tx = parent_xf->a * xf.tx + parent_xf->c * xf.ty
                  + parent_xf->tx;
      composed.ty = parent_xf->b * xf.tx + parent_xf->d * xf.ty
                  + parent_xf->ty;
      /* Recurse: parse the referenced glyph with the composed transform.    */
      if( (int) sub_gid < f->ttf_n_glyphs )
      {
        unsigned long *glyf_off_p =
          (unsigned long *) (f->arena + f->glyf_off_arena_off);
        unsigned long g_start = glyf_off_p[sub_gid];
        unsigned long g_end   = glyf_off_p[sub_gid + 1];
        if( g_end > g_start && (size_t) g_end <= f->glyf_len )
        {
          const unsigned char *sgd =
            f->arena + f->glyf_arena_off + g_start;
          size_t sgd_len = (size_t) (g_end - g_start);
          if( sgd_len >= 10 )
          {
            int sn = (int) (signed short) svg_glyph_be_u16(sgd);
            if( sn >= 0 )
              svg_glyph_emit_simple(c, f, sgd, sgd_len, sn, &composed);
            else if( depth + 1 <= SVG_GLYPH_TTF_RECURSE )
              svg_glyph_emit_composite(c, f, sgd, sgd_len,
                                       depth + 1, &composed);
          }
        }
      }
    }
    /* WE_HAVE_INSTRUCTIONS (0x0100): the spec puts u16 instructionLength + */
    /* instructions[] after the last component.  Since we exit the loop    */
    /* immediately when MORE_COMPONENTS is clear, we never read those      */
    /* trailing bytes -- the unhinted renderer correctly ignores them.     */
    /* OVERLAP_COMPOUND (0x0400): hint for fill-rule consumers; ignored.   */
    more = (flags & SVG_TTF_CF_MORE_COMPS) ? 1 : 0;
  }
  return 1;
}


/*****************************************************************************/
/*  Top-level TrueType loader.                                              */
/*****************************************************************************/

static int svg_glyph_load_ttf(svg_glyph_font *f, const char *path)
{
  FILE *fp;
  long fsize;
  unsigned char *raw = NULL;
  size_t got;
  svg_glyph_tt_dir dir;
  int n_glyphs;
  unsigned long *loca_arr = NULL;
  unsigned short *cp_to_gid = NULL;
  unsigned char *glyf_arena = NULL;
  int upem;

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

  if( !svg_glyph_parse_tt_dir(raw, (size_t) fsize, &dir) )
  { free(raw); return 0; }
  upem = dir.units_per_em;
  if( dir.maxp.len < 6 ) { free(raw); return 0; }
  n_glyphs = (int) svg_glyph_be_u16(raw + dir.maxp.off + 4);
  if( n_glyphs <= 0 || n_glyphs >= SVG_GLYPH_TTF_MAX_GLYPHS )
  { free(raw); return 0; }

  loca_arr = svg_glyph_parse_loca(raw, (size_t) fsize, &dir, n_glyphs);
  if( loca_arr == NULL ) { free(raw); return 0; }

  cp_to_gid = (unsigned short *) calloc(0x10000, sizeof *cp_to_gid);
  if( cp_to_gid == NULL )
  { free(loca_arr); free(raw); return 0; }
  if( !svg_glyph_parse_cmap(raw, (size_t) fsize,
        dir.cmap.off, dir.cmap.len, cp_to_gid) )
  { free(cp_to_gid); free(loca_arr); free(raw); return 0; }

  /* Copy the glyf table into the arena so it outlives `raw`, then store    */
  /* the loca offsets array (also in the arena) for the runtime decoder.   */
  /* We track these by *offset* into the arena, not pointer, because the   */
  /* arena's realloc can move the underlying buffer when later cs4 slots   */
  /* are allocated and any stored pointers would dangle.                   */
  f->glyf_arena_off = f->arena_used;
  glyf_arena = svg_glyph_arena_alloc(f, dir.glyf.len);
  if( glyf_arena == NULL )
  { free(cp_to_gid); free(loca_arr); free(raw); return 0; }
  memcpy(glyf_arena, raw + dir.glyf.off, dir.glyf.len);
  f->glyf_len     = dir.glyf.len;
  f->ttf_n_glyphs = n_glyphs;

  /* Persist the loca offsets array inside the arena too.                    */
  {
    size_t need = sizeof(unsigned long) * (size_t) (n_glyphs + 1);
    unsigned char *off_arena;
    f->glyf_off_arena_off = f->arena_used;
    off_arena = svg_glyph_arena_alloc(f, need);
    if( off_arena == NULL )
    { free(cp_to_gid); free(loca_arr); free(raw); return 0; }
    memcpy(off_arena, loca_arr, need);
  }

  /* Build per-name glyph entries.  We walk the reverse AGL table plus the  */
  /* ASCII fast-path range, look up each codepoint, and create an entry    */
  /* per resolved GID.  The cs slot holds a 4-byte little-endian GID.      */
  /*                                                                        */
  /* Reserve enough arena capacity upfront so the cs4 pointers we store    */
  /* through this loop don't get invalidated by a mid-loop realloc.        */
  {
    int reserve_count = 26 + 26 + 10;
    int rt;
    for( rt = 0; svg_glyph_agl_rev_table[rt].name != NULL; rt++ )
      reserve_count++;
    if( !svg_glyph_arena_reserve(f, (size_t) reserve_count * 4) )
    { free(cp_to_gid); free(loca_arr); free(raw); return 0; }
  }
  f->nglyphs = 0;

  /* Helper inline-pattern: register one name -> gid pair into glyphs[].    */
  /* Implemented as nested block to avoid mid-block decls.                  */
  {
    int reg;
    unsigned int cp;
    const char *nm;
    int idx;
    unsigned int gid;
    /* ASCII letters/digits.                                                 */
    for( reg = 0; reg < 26; reg++ )
    {
      char buf[2];
      buf[0] = (char) ('A' + reg); buf[1] = 0;
      cp = (unsigned int) buf[0];
      gid = cp_to_gid[cp];
      if( gid == 0 ) continue;
      if( f->nglyphs >= SVG_GLYPH_MAX_GLYPHS ) break;
      idx = f->nglyphs++;
      f->glyphs[idx].name[0] = buf[0]; f->glyphs[idx].name[1] = 0;
      { unsigned char *cs4 = svg_glyph_arena_alloc(f, 4);
        if( cs4 == NULL ) break;
        cs4[0] = (unsigned char) (gid & 0xff);
        cs4[1] = (unsigned char) ((gid >> 8) & 0xff);
        cs4[2] = 0; cs4[3] = 0;
        f->glyphs[idx].cs = cs4; f->glyphs[idx].cs_len = 4; }
    }
    for( reg = 0; reg < 26; reg++ )
    {
      char buf[2];
      buf[0] = (char) ('a' + reg); buf[1] = 0;
      cp = (unsigned int) buf[0];
      gid = cp_to_gid[cp];
      if( gid == 0 ) continue;
      if( f->nglyphs >= SVG_GLYPH_MAX_GLYPHS ) break;
      idx = f->nglyphs++;
      f->glyphs[idx].name[0] = buf[0]; f->glyphs[idx].name[1] = 0;
      { unsigned char *cs4 = svg_glyph_arena_alloc(f, 4);
        if( cs4 == NULL ) break;
        cs4[0] = (unsigned char) (gid & 0xff);
        cs4[1] = (unsigned char) ((gid >> 8) & 0xff);
        cs4[2] = 0; cs4[3] = 0;
        f->glyphs[idx].cs = cs4; f->glyphs[idx].cs_len = 4; }
    }
    {
      static const char *digit_names[10] = {
        "zero","one","two","three","four",
        "five","six","seven","eight","nine"
      };
      for( reg = 0; reg < 10; reg++ )
      {
        cp = (unsigned int) ('0' + reg);
        gid = cp_to_gid[cp];
        if( gid == 0 ) continue;
        if( f->nglyphs >= SVG_GLYPH_MAX_GLYPHS ) break;
        idx = f->nglyphs++;
        nm = digit_names[reg];
        { size_t L = strlen(nm);
          if( L >= SVG_GLYPH_NAME_LEN ) L = SVG_GLYPH_NAME_LEN - 1;
          memcpy(f->glyphs[idx].name, nm, L);
          f->glyphs[idx].name[L] = 0; }
        { unsigned char *cs4 = svg_glyph_arena_alloc(f, 4);
          if( cs4 == NULL ) break;
          cs4[0] = (unsigned char) (gid & 0xff);
          cs4[1] = (unsigned char) ((gid >> 8) & 0xff);
          cs4[2] = 0; cs4[3] = 0;
          f->glyphs[idx].cs = cs4; f->glyphs[idx].cs_len = 4; }
      }
    }
    /* Named entries from svg_glyph_agl_rev_table.                           */
    for( reg = 0; svg_glyph_agl_rev_table[reg].name != NULL; reg++ )
    {
      cp = svg_glyph_agl_rev_table[reg].cp;
      if( cp >= 0x10000 ) continue;
      gid = cp_to_gid[cp];
      if( gid == 0 ) continue;
      if( f->nglyphs >= SVG_GLYPH_MAX_GLYPHS ) break;
      idx = f->nglyphs++;
      nm = svg_glyph_agl_rev_table[reg].name;
      { size_t L = strlen(nm);
        if( L >= SVG_GLYPH_NAME_LEN ) L = SVG_GLYPH_NAME_LEN - 1;
        memcpy(f->glyphs[idx].name, nm, L);
        f->glyphs[idx].name[L] = 0; }
      { unsigned char *cs4 = svg_glyph_arena_alloc(f, 4);
        if( cs4 == NULL ) break;
        cs4[0] = (unsigned char) (gid & 0xff);
        cs4[1] = (unsigned char) ((gid >> 8) & 0xff);
        cs4[2] = 0; cs4[3] = 0;
        f->glyphs[idx].cs = cs4; f->glyphs[idx].cs_len = 4; }
    }
  }

  /* em_scale: design-units -> 1000-em (matches CFF convention).             */
  if( upem > 0 && upem != 1000 )
    f->em_scale = 1000.0 / (double) upem;
  else
    f->em_scale = 1.0;

  free(cp_to_gid);
  free(loca_arr);
  free(raw);
  return f->nglyphs > 0;
}


/*****************************************************************************/
/*  .ttf file probe + search.                                                */
/*****************************************************************************/

static int svg_glyph_ttf_probe(const char *path)
{
  FILE *fp = fopen(path, "rb");
  unsigned char magic[4];
  size_t n;
  unsigned long m;
  if( fp == NULL ) return 0;
  n = fread(magic, 1, 4, fp);
  fclose(fp);
  if( n != 4 ) return 0;
  m = ((unsigned long) magic[0] << 24) | ((unsigned long) magic[1] << 16)
    | ((unsigned long) magic[2] <<  8) |  (unsigned long) magic[3];
  return (m == 0x00010000UL || m == 0x74727565UL || m == 0x74797031UL);
}

static int svg_glyph_try_ttf_dir(const char *dir, const char *name,
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
  return svg_glyph_ttf_probe(out);
}

static int svg_glyph_try_ttf_dir_recursive(const char *dir, const char *name,
  char *out, size_t cap)
{
  if( svg_glyph_try_ttf_dir(dir, name, out, cap) ) return 1;
  {
    static const char *subs[] = {
      "dejavu/","liberation/","liberation2/","noto/","ttf-dejavu/",
      "msttcorefonts/","freefont/","ubuntu/","cabin/","croscore/",
      "open-sans/","roboto/","lato/","crosextra/","lyx/","ttf-bitstream-vera/",
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
      if( svg_glyph_try_ttf_dir(joined, name, out, cap) ) return 1;
    }
  }
  return 0;
}

static int svg_glyph_find_ttf_path(const char *ps_name, char *out, size_t cap)
{
  const char *override;
  const char *file = NULL;
  char namebuf[SVG_GLYPH_PSN_LEN + 8];
  int i;

  for( i = 0; svg_glyph_ttf_map[i].ps_name != NULL; i++ )
    if( strcmp(svg_glyph_ttf_map[i].ps_name, ps_name) == 0 )
    { file = svg_glyph_ttf_map[i].file; break; }
  if( file == NULL )
  { size_t pl = strlen(ps_name);
    if( pl + 4 + 1 > sizeof namebuf ) return 0;
    memcpy(namebuf, ps_name, pl);
    memcpy(namebuf + pl, ".ttf", 5);
    file = namebuf;
  }

  override = getenv("LOUT_TTF_FONT_DIR");
  if( override != NULL && override[0] != 0 )
    if( svg_glyph_try_ttf_dir_recursive(override, file, out, cap) ) return 1;
  /* LOUT_T1_FONT_DIR doubles as a TTF override too -- the user task spec   */
  /* exercises this on the DejaVu directory.                                */
  override = getenv("LOUT_T1_FONT_DIR");
  if( override != NULL && override[0] != 0 )
    if( svg_glyph_try_ttf_dir_recursive(override, file, out, cap) ) return 1;
  for( i = 0; svg_glyph_ttf_dir[i] != NULL; i++ )
    if( svg_glyph_try_ttf_dir_recursive(svg_glyph_ttf_dir[i], file, out, cap) )
      return 1;
  return 0;
}


