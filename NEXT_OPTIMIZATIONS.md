# Next SVG Back-End Optimizations

Living tracker.  Items above the horizontal rule are the current
queue; items below the rule are "completed -- archived for history".

Read-only survey refreshed 2026-05-21.  After commit `2a33e3d`
("z53.c: hash svg_ps_exec_op; svgmacros: add @DMath for KaTeX") the
quick-win list opened by SVG_PERFORMANCE.md's section 4 ranking is
closed: FNV-1a dict hash, op_id hash dispatch, `filledsquare/...`
strcmp hoist, `setvbuf` on `out_fp`, @Graphic token memoization, and
the `charpath` bbox approximation have all landed.  User's Guide
wall-time is down from ~77 s to ~32 s (30-45% wall, 40-50% user) --
ahead of the 10% target.

This file lists the next round of opportunities, ranked by
impact-per-effort.  Each entry: description / impact / complexity /
observable / files touched.

---

## 1. Symbol-font glyph table extension (~150 Adobe Symbol -> Unicode names)

Description: `svg_glyph_table` in `z53.c` covers Latin-1 + a handful of
typographic punctuation but lacks **138 of the 188 named glyphs** in
`maps/Symb.LCM`.  Every Greek letter (`alpha`..`omega`,
`Alpha`..`Omega`), every math operator (`integral`, `summation`,
`product`, `partialdiff`, `infinity`, `lessequal`, `notequal`, ...),
every set/logic operator (`element`, `subset`, `union`, `logicaland`,
...), every arrow (`arrowleft`/`right`/`up`/`down`, ...), every big-
fence variant (`parenlefttp`, `bracketrighttp`, ...), and the card
suits fall through to the `cp = c` Latin-1 fallback in
`svg_emit_word_text`, producing visibly wrong glyphs.  User's Guide
SVG build: 1182 `font-family="Symbol"` references but zero correctly
mapped Greek / math hits.  Audit lives in SVG_PORTING.md section 10
"Remaining known issues -> @Sym / @Char Symbol-font glyph gap".

Fix is a ~150-entry static-array extension covering Greek
(U+0370-U+03FF), Math Operators (U+2200-U+22FF), Misc Math
(U+27C0-U+27EF), Arrows (U+2190-U+21FF), plus a handful of fence /
Dingbats names.  Source mapping is Adobe's `glyphlist.txt`.

Impact: correctness.  No performance change.  This is the single
biggest visible quality gap in the SVG back-end today: equations,
"alpha = beta", "n -> infinity", etc. all render as garbage Latin
characters.

Complexity: small-medium (~150 array entries plus matching one-line
test in `svg_glyph_to_unicode`; mechanical).

Observable: `grep -c '[α-ωΑ-Ω]' /tmp/user.svg` jumps from 0 to
several hundred.  Visual: User's Guide pages 86-95 (equation
gallery), 248, 262 stop showing stray Latin letters where Greek was
intended.

Touches: just `z53.c`.

Rank: **#1** (highest user-visible quality impact).

---

## 2. `symbolsize` tracking for @Graph plot symbols (pages 248 / 262 follow-up)

Description: commit `421c0dc` fixed the gross `ss` symbolsize drift
that caused page 248 / 262 plot symbols to grow to 250 pt.  Symbols
now render at the correct ~3 pt size on the User's Guide build.
However, the C-side allowlist that shadows the graphf.lpg dict procs
hard-codes the lookup of `symbolsize` from the current dict stack at
shape-emission time.  This works as long as the @Graph macro's
prologue has already executed `def`s for `symbolsize`,
`symbollinewidth`, `xcurr`, `ycurr` -- which is the standard case.
But user-customised paint procedures that compute `symbolsize`
inline (rather than via the graphf.lpg `def` path) currently fall
back to a fixed 3 pt default.  Audit and either (a) accept any
on-stack numeric arg before the dict lookup, or (b) document the
constraint and emit a warning when the dict-lookup fails.

Impact: correctness on custom @Graph paint procs.  No effect on
the standard graphf.lpg-based plots.

Complexity: small (~30 LOC; the audit might surface that the dict
lookup is fine and only a documentation note is needed).

Observable: a contrived @Graph doc that computes `symbolsize` via
`xsize 40 div def` inside the paint proc.  Before: 3 pt symbols
regardless.  After: symbols sized to xsize/40.

Touches: just `z53.c`.

Rank: #2.  Lower than #1 because the affected use case is rare; but
the underlying mechanism is well-understood and the fix is short.

---

## 3. Real OTF / Type1 outline parsing -> `charpath` (LANDED 2026-05-22)

Status: implemented.  `SVG_OP_CHARPATH` now consults a new module
`z53_glyph.c` that loads the system URW++ Type 1 `.pfb` outline for
the active PostScript font name (Times-*, Helvetica-*, Courier-*,
Symbol, ZapfDingbats, ZapfChancery, Bookman-*, NewCenturySchlbk-*,
AvantGarde-*, Palatino-*), decrypts the eexec body, parses the
`/Subrs` array and `/CharStrings` dict, and runs the Type 1
charstring per character through the path accumulator's existing
`svg_ps_moveto / _lineto / _curveto / _closepath` helpers.

When the font (or one of its glyphs) can't be located the per-
character fallback is the original 0.5 em x 1.0 em bbox rectangle,
so `coltex`'s `charpath flattenpath pathbbox` consumer continues to
see a plausible bbox.  Environment override:
`LOUT_T1_FONT_DIR=<dir>` prepends a search dir for the `.pfb`
file; `LOUT_NO_GLYPH_OUTLINES=1` disables the new path entirely.

Implementation footprint: ~750 LOC in `z53_glyph.c` (PFB segment
unwrap; eexec + charstring RC4-variant decryption; Type 1
charstring interpreter with `hsbw / sbw / rmoveto / hmoveto /
vmoveto / rlineto / hlineto / vlineto / rrcurveto / vhcurveto /
hvcurveto / closepath / callsubr / return / endchar / seac / div /
callothersubr / pop / dotsection / hstem / vstem` and the `flex`
family).  Plus ~80 LOC of ASCII-to-glyph-name mapping and four
callback shims in `z53.c`.

Update 2026-05-22: CFF/OpenType outline support landed (extends
`z53_glyph.c`).  The loader now probes `.pfb` first (Adobe base-35)
then falls through to `.otf` via a parallel search path
(`LOUT_OTF_FONT_DIR` env override + `/usr/share/fonts/opentype/`
and `/usr/share/fonts/truetype/` with one-level subdirectory walk).
The OpenType table directory parser locates the `CFF ` table,
decodes the Top DICT (`CharStrings` op 17, `Private` op 18,
`charset` op 15) and Private DICT (`Subrs` op 19), then runs a
Type 2 charstring interpreter that mirrors the Type 1 one but with
proper variadic operators (rlineto / hlineto / vlineto / rrcurveto /
vhcurveto / hvcurveto / hhcurveto / vvcurveto / rcurveline /
rlinecurve), biased subroutine calls (callsubr / callgsubr with
107 / 1131 / 32768 bias), the flex family (hflex / flex / hflex1 /
flex1 expanded to two rrcurveto's), and the implicit width-delta
operand handling.

Update 2026-05-22 (later): TrueType `.ttf` (`glyf` table) support
landed alongside.  Detection branches on the sfnt magic
(`0x00010000` / `'true'` / `'typ1'`).  The loader walks the same OT
table directory used by the CFF path, locates `head` (UnitsPerEm,
indexToLocFormat), `maxp` (numGlyphs), `cmap` (format 4 BMP + format
12 supplementary, ordered Unicode-platform > Windows-Unicode),
`loca` (short or long indexed by the head bit), and `glyf` (cached
into the per-font arena by offset, not pointer, since the arena
realloc can move the backing buffer mid-load).  At emit time the
selected glyph's record is parsed: simple outlines walk the flag /
x / y delta streams with repeat-byte expansion, implicit on-curve
midpoints between two off-curve points, and quadratic Beziers
converted to cubics via the standard P0 + 2/3(Q-P0), P2 + 2/3(Q-P2)
formula.  Composites recurse with a 2x2 affine matrix (scale,
xy-scale, and two-by-two forms decoded; translation via
ARGS_ARE_XY_VALUES; depth cap 8).  Aliases for the DejaVu /
Liberation / Noto families live in `svg_glyph_ttf_map`; an
environment override `LOUT_TTF_FONT_DIR` (and `LOUT_T1_FONT_DIR` as a
shared convenience override) prepends a search dir.

Out-of-scope follow-ups (left for a future round):
  - Honouring the active LCM for non-Latin1 character encodings
    inside `charpath` (the operand is whatever bytes the PS source
    pushed, with no FONT_NUM in scope; we use Adobe
    StandardEncoding glyph names as a coarse approximation).
  - TrueType hinting / variation fonts (`fvar`/`gvar`).  Outlines
    are emitted unhinted; variation axes are not honoured.
  - TrueType collection (`.ttc`) wrappers.  We only accept top-
    level sfnt files; embedded TTCs fall through to the bbox.
  - CFF predefined charsets 1 (Expert) and 2 (ExpertSubset).
    These are fixed GID->SID maps baked into the CFF 1.0 spec
    (Appendix C: 166 and 87 entries respectively).  No font in
    the Lout corpus declares them, so they remain stubbed to
    `.notdef`.  See the comment block above
    `svg_glyph_cff_parse_charset` in `z53_glyph.c`.

Update 2026-05-23: small audit follow-ups in `z53_glyph.c`:
  - Type 2 escape op 26 (`sqrt`) now implements a Newton-Raphson
    iteration in place of the prior silent drop.  Op 33
    (`setcurrentpoint`) is documented as a Type 1 leftover that
    Type 2 charstrings should never emit.
  - Composite-glyph WE_HAVE_INSTRUCTIONS (0x0100) and
    OVERLAP_COMPOUND (0x0400) handling explicitly documented: the
    former is naturally skipped because we return on the last
    component before reaching the instruction stream; the latter
    is a fill-rule hint that SVG's non-zero default already honours.
  - Empty TTF glyph cases (zero-length glyf entry, and 10-byte
    header with `numberOfContours == 0`) both verified safe -- the
    first returns early in `svg_glyph_run_ttf`, the second in
    `svg_glyph_emit_simple`.

Touches: `z53.c` + new `z53_glyph.c` + `makefile`.  No
`include/` changes; the PostScript back end (`z49.c`) is
untouched.

Rank: closed.

---

## 4. More aggressive `@Graphic` raw-PS -> SVG translation

Description: When an `@Graphic` block contains raw PostScript that
the embedded interpreter does not understand (e.g. an obscure
prologue procedure from a custom user macro, or `image` /
`imagemask` raster ops), `svg_ps_run` currently falls back to
emitting the offending PS as an XML comment so the output stays
valid.  The comment is invisible to readers, so the affected
graphic silently disappears.

Fix path (incremental): (a) survey the User's Guide and the test
corpus for the most common XML-comment-fallback ops and add
explicit translations for them; (b) for raster ops in particular,
add a one-shot "rasterise via the PS interpreter into a base64
data: URI" fallback so that even unparseable graphics produce a
visible bitmap instead of nothing.

Impact: completeness.  The current XML-comment fallback is graceful
in the sense of not crashing, but it produces silently-degraded
output.  Coverage today is high enough that the test corpus
exercises this rarely (53 Pass-Excellent).  Real-world third-party
documents may hit it more often.

Complexity: medium (incremental; each new op is a small addition,
but the long tail is unbounded).  The rasterisation fallback alone
is ~150-300 LOC if we use a tiny bitmap routine.

Observable: count XML-comment fallbacks in the User's Guide SVG
build: `grep -c '<!-- svg_ps' /tmp/user.svg`.  Drop should be
proportional to coverage gained.

Touches: just `z53.c`.

Rank: #4.  Lower priority because the current behaviour is "wrong
but doesn't crash".  Schedule when a user reports a missing
graphic.

---

## 5. `SVG_NullBackEnd` refinements if convergence issues surface

Description: `SVG_NullBackEnd` already received a round of fixes
(commit `c618ce3`, dedicated stubs for non-final-pass passes) and
no convergence issues have surfaced in the last ~20 commits.
This entry is a placeholder for tracking: if cross-reference
resolution ever stalls or produces inconsistent page numbers on
the SVG path, the first suspect is the null back-end accidentally
emitting partial output that the final-pass back-end then has to
work around.

Impact: correctness (latent).

Complexity: small per fix (most null-back-end stubs are 2-5 LOC
each).

Observable: a doc whose page count or @PageOf cross-references
disagree between PS and SVG outputs would be the smoking gun.

Touches: just `z53.c`.

Rank: #5.  No action item today; tracked for awareness.

---

## Summary table

| #  | Change                                       | Complexity   | Impact category | Notes                              |
|----|----------------------------------------------|--------------|-----------------|------------------------------------|
| 1  | Symbol-font glyph table extension            | small-medium | correctness     | Single biggest user-visible gap    |
| 2  | symbolsize tracking for @Graph paint procs   | small        | correctness     | Edge case; mechanism understood    |
| 3  | Real OTF/Type1 outline parsing -> charpath   | large        | completeness    | LANDED 2026-05-22 via `z53_glyph.c`|
| 4  | More aggressive raw-PS -> SVG translation    | medium       | completeness    | Closes XML-comment fallback        |
| 5  | SVG_NullBackEnd refinements                  | small        | correctness     | No action; tracked for awareness   |

## Not included (out of scope)

- Font-metric matching at the rasteriser (mitigation already landed
  via embedded URW++ Nimbus base-35 data URLs).
- Per-glyph absolute positioning via `<tspan>` arrays (deferred
  until a `--precise-text` flag is requested).
- Texture-procedure body interpretation (already gracefully
  degrades to named-texture identification; no user-reported pain).
- C Lout UTF-8 input layer, OpenType loader (TODO section 4).

---

## Archive: completed items (for history)

The following items were listed as pending in earlier revisions of
this file and have since landed.  Kept here for traceability; cross-
reference SVG_PERFORMANCE.md section 1.1 for the timing impacts.

- **FNV-1a hash for `svg_dict_lookup`** -- landed commit `24d76b4`
  ("SVG perf: open-address hash for dict lookup -- 2.3x faster",
  pre-dates this file's first revision).  Eliminated the 24%-of-
  CPU `svg_dict_stack_lookup` hotspot.
- **`filledsquare/...` strcmp guard hoist** -- landed commit
  `9ddc198` (item 1 of "SVG perf: filledsquare guard, setvbuf,
  @Graphic token memo").  Replaced the 20-strcmp allowlist with a
  first-character `switch` + short strcmp.  ~3.7% wall.
- **`setvbuf(out_fp, NULL, _IOFBF, 128 KiB)`** -- landed commit
  `9ddc198` (item 2).  ~2.9% wall, -19% sys.
- **@Graphic token-stream memoization** -- landed commit `9ddc198`
  (item 3).  ~5.3% wall.
- **`op_id` hash dispatch for `svg_ps_exec_op`** -- landed commit
  `2a33e3d`.  ~77 s -> ~32 s on User's Guide build (~30-45% wall).
- **`charpath` bbox approximation** -- landed commit `2a33e3d`
  (item 2 of that commit).  Per-character axis-aligned bbox
  (`fs*0.5` wide, `fs*0.8` ascent, `fs*0.2` descent) sufficient
  for `include/coltex`'s `charpath flattenpath pathbbox`.  Real
  outline parsing tracked as queue item #3 above.
- **`xdecr`/`ydecr` BOOL coercion in @Graph C-shortcut** -- absorbed
  into the broader @Graph axes fix (commit `3e26007`,
  "Fix @Graph axes: tighten eq/ne to require same-kind operands")
  which addresses the same family of NUM/BOOL type-confusion bugs
  from the opposite side.  Verified by 30/30 regression snippets.
