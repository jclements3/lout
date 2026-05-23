# SVG Back-End Porting Guide (z49.c -> z53.c)

Working document for adding an SVG back-end to Lout that mirrors the
existing PostScript back-end (`lout/z49.c`) function-for-function. The
PS back-end is FROZEN; this is purely additive work. The output target
is byte-for-byte feature parity at the drawing level: every PS drawing
operation must have an SVG equivalent.

## 1. Overview

`lout/z49.c` (2274 lines) defines `PS_BackEnd`, a pointer to a
`struct back_end_rec` of ~26 function pointers plus 10 capability
booleans. The struct is declared in `lout/externs.h:2073-2116`. Lout's
galley layout engine (`z18.c`-`z22.c`) walks the formatted document
tree and dispatches drawing operations to `BackEnd->Foo(...)`. The
back-end emits the actual file (PostScript, PDF, or plain text).

Back-end selection happens in `lout/z01.c:236-240` (the `BE_TYPE`
enum: `BE_PLAIN`, `BE_PS`, `BE_PDF`) and `lout/z01.c:689-705` (the
dispatch block that assigns `BackEnd = PS_BackEnd` or `Plain_BackEnd`
or `PDF_BackEnd`). On non-final runs (Lout iterates up to 3 times for
cross-reference resolution) the engine uses a `*_NullBackEnd` whose
callbacks are no-ops.

`PS_BackEnd` is defined at `z49.c:2097-2137`; `PS_NullBackEnd` at
`z49.c:2148-2244`. The `code` field is `POSTSCRIPT` (=0), defined in
`externs.h:2447`. We will mirror this layout in a new `z53.c` that
defines `SVG_BackEnd` and `SVG_NullBackEnd` with `code = SVG` (=3).

The PS back-end leans heavily on a runtime PostScript prologue
(`z49.c:793-1149`) that defines PS procedures `LoutGraphic`, `LoutFont`,
`LoutRecode`, `LoutStartEPSF`, `LoutSetRGBColor`, etc. The drawing
callbacks then emit short PS commands that invoke those procedures.
SVG has no equivalent run-time scripting; the SVG back-end must emit
fully-resolved geometry inline. This means more state lives in the C
code on the SVG side.

## 2. State variables

All static variables in z49.c that the SVG back-end must mirror,
adapt, or drop. Line numbers refer to `lout/z49.c`.

| Variable | z49.c line | Purpose | SVG equivalent |
|---|---|---|---|
| `out_fp` | 59 | output FILE* | KEEP - same role |
| `encapsulated` | 52 | TRUE if EPS output requested (-EPS) | DROP - no EPS mode in SVG; if user passes -EPS with SVG, error |
| `wordcount` | 53 | atoms since last newline (PS line-length cosmetic) | DROP - SVG is XML; pretty-print at element granularity instead |
| `pagecount` | 54 | total pages emitted | KEEP - used for page numbering and as the suffix on per-page filenames or `<g id="page-N">` ids |
| `prologue_done` | 55 | TRUE after header prologue printed | KEEP - guards `SVG_PrintAfterLastPage` cleanup |
| `needs` | 56 | OBJECT list of DSC resource needs from included EPSFs | DROP - SVG has no DSC; resources are inline `<defs>` |
| `supplied` | 57 | OBJECT list of DSC resources supplied | DROP |
| `incg_files` | 58 | OBJECT list of `@IncludeGraphicRepeated` files (PS Form dict) | REPLACE - keep a list to deduplicate, but emit each as a `<symbol>` in `<defs>` and reference via `<use>` |
| `gs_stack` / `gs_stack_top` | 79-80 | C-side mirror of PS graphics state stack (font, colour, texture, currentpoint y, xheight); `MAX_GS = 50` | KEEP - SVG needs the same C-side stack because SVG has no implicit graphics state; we also need to track the open `<g>` element nesting depth |
| `currentfont` | 82 | font of most recent atom | KEEP - used to elide redundant font emission; in SVG it becomes the last-emitted `font-family`/`font-size` attribute pair |
| `currentbaselinemark` | 83 | baseline mark flag | KEEP - affects y offset of next word |
| `currentcolour` | 84 | colour of most recent atom | KEEP - elides redundant `fill=` |
| `currenttexture` | 85 | texture of most recent atom | KEEP - SVG renders textures as `<pattern>` in `<defs>` |
| `currentxheight2` | 86 | half xheight of current font | KEEP - used by `PS_PrintWord` to vertically center text on baseline |
| `cpexists` | 87 | TRUE if a PS "current point" exists | DROP - SVG `<text>` is absolutely positioned; there is no implicit current point. The y-coordinate elision optimization in `PS_PrintWord` (`z49.c:1394`) does not apply. |
| `currenty` | 88 | y coord of current point, if `cpexists` | DROP for the same reason |
| `link_dest_tab` | 305 | hash table of link dest names | KEEP - same dedup logic, but emit `<a>` / `id=` instead of pdfmark |
| `link_source_list` | 306 | list of link source names | KEEP |

New state the SVG back-end needs that PS does not:

- `current_page_g_open` (BOOLEAN): whether a `<g class="page">` is
  currently open (closed by `SVG_PrintBetweenPages` and
  `SVG_PrintAfterLastPage`).
- `page_width_pt`, `page_height_pt` (FULL_LENGTH): cached from the
  first `SVG_PrintBeforeFirstPage` call for use in viewBox and in
  per-coordinate y-flip if that strategy is chosen.
- `defs_buffer` (OBJECT or FILE\*): patterns, symbols, raster images
  must be emitted into `<defs>` near the start of the SVG document.
  Since we emit linearly, either (a) buffer the entire body to a
  temp file and flush after `<defs>`, or (b) emit `<defs>` lazily at
  the very end inside `<svg>` (legal but unusual). Recommend (a) for
  cleanliness.

## 3. Function-by-function port table

### Status digest (2026-05-21)

All 32 callbacks below are now **implemented** in `z53.c` and exercised by
the 53-snippet regression suite (`tests/run_all.sh` -> 53 Pass-Excellent,
0 Fail).  The "Complexity" column below was the porting estimate; the
table is kept for historical reference and for the few items where the
SVG strategy still has known gaps (see section 10 "Remaining known
issues" for the live list).

Recently completed (since the previous SVG_PORTING.md revision):

- `svg_ps_exec_op` strcmp ladder -> FNV-1a + open-addressed hash table
  feeding a `switch (op_id)` (commit `2a33e3d`, 2026-05-21).  Built
  lazily from a static seed table of ~175 `(name, op_id)` pairs; aliases
  (`setrgbcolor`/`LoutSetRGBColor`, `fill`/`eofill`,
  `userdict`/`systemdict`/`globaldict`/`errordict`/`statusdict`/`$error`,
  `save_cp`/`restore_cp`, `setlinecap`/`setlinejoin`/`setmiterlimit`,
  `currentmatrix`/`defaultmatrix`) collapse onto one op_id; pairs that
  need to discriminate (`arc`/`arcn`, `transform`/`dtransform`,
  `eq`/`ne`, `lt`/`gt`/`le`/`ge`, `and`/`or`/`xor`, ...) keep distinct
  op_ids and dispatch on op_id inside the case.  The 20 @Graph plot-
  symbol names route to a new helper `svg_ps_exec_symbol`.  See
  SVG_PERFORMANCE.md for the timing impact (~77 s -> ~32 s on the
  User's Guide build).
- `SVG_OP_CHARPATH` now lays a per-character axis-aligned bbox
  (`fs*0.5` wide, `fs*0.8` ascent, `fs*0.2` descent) into the current
  path and advances the current point to the string's end.  Sufficient
  for `include/coltex`'s `charpath flattenpath pathbbox` sequence
  (the only in-tree consumer); real Type1/OTF outline parsing through
  `z37.c` deferred (see NEXT_OPTIMIZATIONS.md).
- `show` + font operators -- axis tick labels now render
  (commit `67f29dd`).
- `@DocInfo` SVG branch -- now emits nothing instead of stray pdfmark
  gibberish (commit `63c247a`).
- @Graph plot-symbol sizing (`cur_gr_loutf` now tracks @Graph's
  `font { -2p }` argument; pages 248 / 262 fixed, commit `421c0dc`).
- @Graph axes: tightened `eq`/`ne` so that NUM vs BOOL no longer
  compares as equal (commit `3e26007`).

All functions taken from `grep -n "^static.*PS_\|^void.*PS_" z49.c`.
Functions are listed in source order. Line numbers are `z49.c`.

| # | PS function | Line | What it emits | SVG strategy | Complexity |
|---|---|---|---|---|---|
| 1 | `PS_PrintInitialize` | 333 | Resets all static state; opens link tables. No output. | Same: reset SVG state, open link tables. No XML emitted yet. | TRIVIAL |
| 2 | `PS_PrintLength` | 363 | Formats a length as cm string (debug only). | Direct copy; not output-format dependent. | TRIVIAL |
| 3 | `PS_IncGRepeated` | 378 | Registers an `@IncludeGraphicRepeated` file in `incg_files` list. Not output. | Direct port: same list, used by `SVG_PrintGraphicInclude` to emit a single `<symbol>` per repeated file. | TRIVIAL |
| 4 | `PS_FindIncGRepeated` | 397 | Looks up file in `incg_files`; warns on type mismatch. | Direct copy. | TRIVIAL |
| 5 | `PS_PrintPageSetupForFont` | 442 | Emits `%%IncludeResource` DSC comment and a PS `/short_name { /font_name LoutFont } def` macro. | NO-OP for SVG: fonts are not preloaded; each `<text>` element carries its own `font-family` and `font-size` attributes. | TRIVIAL |
| 6 | `PS_PrintPageResourceForFont` | 466 | Emits `%%PageResources: font NAME` DSC comment. | NO-OP. | TRIVIAL |
| 7 | `PS_PrintMapping` | 480 | Emits a 256-entry PS encoding vector for a glyph mapping. | NO-OP for v1. SVG uses Unicode `<text>` content directly. If non-Latin glyphs needed later, emit a comment for now and revisit. | TRIVIAL (later: MEDIUM if we honor Lout's character mappings) |
| 8 | `PS_BtoI` | 502 | Little-endian byte decoder for EPS preview headers. | NOT NEEDED - EPS support is out of scope for SVG mode. | TRIVIAL (drop) |
| 9 | `PS_FindEPSSegment` | 525 | Skips MS EPSF preview header. | NOT NEEDED. | TRIVIAL (drop) |
| 10 | `PS_PrintEPSFile` | 604 | Filters EPS content into PS output, diverting DSC resource lines. | NOT NEEDED. | TRIVIAL (drop) |
| 11 | `PS_PrintBeforeFirstPage` | 773 | Largest function in file (~460 lines). Emits PS DSC header, the huge `LoutGraphic`, `LoutRecode`, `LoutStartEPSF`, `LoutSetRGBColor`, texture/pattern setup PS procedures, then `BeginSetup`, all included EPSF resources as PS Forms, then `BeginPageSetup` for page 1. | Emit `<?xml version="1.0" encoding="UTF-8"?>`; emit `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="..pt" height="..pt" viewBox="0 0 W H">` with width/height from the `h, v` args (units: PT). Open `<defs>` (will close before body). Open first `<g class="page" id="page-1">`. Set `prologue_done = TRUE`. The PS prologue procedures have NO SVG equivalent - they are dead weight. | MEDIUM (lots of PS to skip; only ~20-30 LOC of SVG emission) |
| 12 | `PS_PrintAfterLastPage` | 1233 | Emits `pgsave restore showpage`, `%%Trailer`, then DSC resource summaries. | Close last `<g class="page">`. Close `<svg>`. If multi-file-per-page strategy: close the per-page file. | SIMPLE |
| 13 | `PS_PrintBetweenPages` | 1281 | Resets graphics state vars; emits `pgsave restore showpage %%Page: N` then page setup commands. | Close current `<g class="page">`. Reset state vars (font, colour, texture, gs_stack_top=0). Open new `<g class="page" id="page-N">` with optional y-flip transform if not using viewBox. If using one-file-per-page strategy: close current file and open the next. | SIMPLE (single-svg) or MEDIUM (per-page-file) |
| 14 | `PS_PrintWord` | 1346 | Emits text. Performs ligature substitution from `lig_table`, kerning lookups, and composite-character expansion. Emits compact PS like `100 200 (hello)m` (move-show) or `(world)s` (show at current y). | Emit `<text x="HPOS" y="VPOS_FLIPPED" font-family="FONTNAME" font-size="SIZE" fill="COLOUR">ESCAPED_TEXT</text>`. XML-escape `<`, `>`, `&`, `"`, `'`. Keep the ligature substitution loop (1359-1387) since it modifies the string before emission. Kerning: in v1, do not honor per-pair kerning (let the browser kern), or emit per-character `<tspan dx="...">` if matching width is critical. Composite chars: emit each component as a separate `<tspan>` with `dx`/`dy`. | MEDIUM (HARD if pixel-identical width required) |
| 15 | `PS_PrintPlainGraphic` | 1461 | Asserts FALSE - never called by PS back-end. | Same: assert FALSE. The `plaingraphic_avail` capability flag is FALSE for both back-ends. | TRIVIAL |
| 16 | `PS_PrintUnderline` | 1477 | Emits `xstart xstop ymk thick ul` invoking PS `/ul` procedure (gsave; setlinewidth; moveto; lineto; stroke; grestore). | Emit `<line x1="xstart" y1="ymk-offset" x2="xstop" y2="ymk-offset" stroke="COLOUR" stroke-width="THICK"/>`. y must be flipped. | SIMPLE |
| 17 | `PS_CoordTranslate` | 1500 | Emits `XDIST YDIST translate`. | Emit `<g transform="translate(XDIST YDIST)">`. **Open** a new `<g>`. Must be paired with a `</g>` close in `RestoreGraphicState`. This means CoordTranslate becomes a stack-pushing op on the SVG side, OR we accumulate transforms in C and apply at the next `<g>` boundary. Recommend: accumulate; emit a single `<g transform="...">` only when geometry is actually about to be drawn. | MEDIUM (design choice required) |
| 18 | `PS_CoordRotate` | 1517 | Emits `AMOUNT rotate` (amount in degrees). | `<g transform="rotate(AMOUNT)">`. SVG rotates clockwise around origin by default; PS rotates counterclockwise. After the viewBox y-flip, the handedness lines up. Same accumulation strategy as translate. | MEDIUM |
| 19 | `PS_CoordScale` | 1533 | Emits `HFACTOR VFACTOR scale`. | `<g transform="scale(HFACTOR VFACTOR)">`. Same accumulation strategy. | SIMPLE |
| 20 | `PS_CoordHMirror` | 1554 | Emits `[-1 0 0 1 0 0] concat`. | `<g transform="matrix(-1 0 0 1 0 0)">` or `scale(-1 1)`. | SIMPLE |
| 21 | `PS_CoordVMirror` | 1571 | Emits `[1 0 0 -1 0 0] concat`. | `<g transform="scale(1 -1)">`. | SIMPLE |
| 22 | `PS_SaveGraphicState` | 1589 | Emits `gsave`; pushes C-side state (font, colour, etc.) onto `gs_stack`. | Open a `<g>` element (no attributes if no transform pending; else with accumulated transform). Push C-side state. Increment `gs_stack_top`. The element close happens in `RestoreGraphicState`. | SIMPLE |
| 23 | `PS_RestoreGraphicState` | 1618 | Emits `grestore`; pops C-side state. | Emit `</g>`. Pop C-side state. | SIMPLE |
| 24 | `PS_PrintGraphicObject` | 1642 | Walks an OBJECT tree of WORD/ACAT and emits the strings verbatim (whitespace at GAP_OBJ). Used to splice raw PS in `@Graphic { ... }` into the output. | **THE BIG ONE.** See section 5. The OBJECT tree contains user-authored PostScript drawing commands; we must translate them to SVG path commands. Plan: tokenize the PS string, maintain a small PS stack interpreter for the common ops (moveto/lineto/curveto/closepath/stroke/fill/setrgbcolor/setlinewidth/setdash/translate/rotate/scale/gsave/grestore/show/newpath), accumulate into an SVG `<path d="..."/>` element. Unknown ops are emitted as an XML comment so output stays valid. | HARD |
| 25 | `PS_DefineGraphicNames` | 1701 | Emits seven numbers followed by `LoutGraphic`, defining PS variables `xsize`, `ysize`, `xmark`, `ymark`, `loutf`, `loutv`, `louts` (font size, line gap, space gap, etc.) so that the user's `@Graphic { ... PS code ... }` can reference them. | We must mirror the same bindings inside the PS-to-SVG translator. The translator's stack interpreter is initialized with these named values before running the user's PS code. No XML emitted directly. | MEDIUM |
| 26 | `PS_SaveTranslateDefineSave` | 1734 | Optimized fused version of Save+Translate+DefineGraphicNames+Save. | Implement as the literal sequence of the four corresponding SVG operations; do not bother with the PS optimization since SVG output size is not driven by `LoutGr2`-style fusion. | SIMPLE |
| 27 | `PS_FindBoundingBox` | 1795 | (Utility, not in the back_end_rec struct.) Reads bounding box from an EPS file. | NOT NEEDED in SVG mode (no EPS). Keep as PS-only utility. | TRIVIAL (skip) |
| 28 | `PS_PrintGraphicInclude` | 1880 | Splices an external `.eps` file into PS output, with `LoutStartEPSF`/`LoutEPSFCleanUp` wrappers and translate/scale. | In SVG mode the file must be `.svg`, `.png`, or `.jpg`. For SVG: read the file, strip its XML declaration, inline its root `<svg>` content as a `<g>` (with transform for position/scale). For raster: emit `<image x=".." y=".." width=".." height=".." xlink:href="path"/>` or with base64 data URI. EPS in SVG mode: emit a warning and a placeholder rect. | HARD |
| 29 | `PS_LinkSource` | 1983 | Emits a `pdfmark` link-source annotation with rectangle and target name. | Emit `<a xlink:href="#PDFNAME"><rect x=llx y=lly width=urx-llx height=ury-lly fill="transparent"/></a>` around the appropriate region. Or, if the link wraps text: defer and wrap the next `PS_PrintWord` output. Simpler v1: emit a transparent rect as the link region. Remember source in `link_source_list` for cross-check. | SIMPLE |
| 30 | `PS_LinkDest` | 2010 | Emits a `/DEST pdfmark` naming an anchor. Errors on duplicate. | Emit a zero-size `<g id="PDFNAME"/>` (or attach the id to whatever element is being drawn). Same dedup via `link_dest_tab`. | SIMPLE |
| 31 | `PS_LinkURL` | 2042 | Emits an external-URL pdfmark. | Same as LinkSource but href is the raw URL: `<a xlink:href="URL"><rect .../></a>`. | SIMPLE |
| 32 | `PS_LinkCheck` | 2072 | Walks `link_source_list`, warns on any source without a matching dest. | Direct copy - same warning logic. No output. | TRIVIAL |

Null back-end (`PS_NullBackEnd`, `z49.c:2148-2244`): every callback
is a no-op. The SVG null back-end is a verbatim copy with `SVG_Null*`
names; complexity TRIVIAL.

## 4. Coordinate system

SVG origin is top-left, +y down. PostScript origin is bottom-left, +y
up. Lout speaks PS coordinates internally; the page is `h x v` points
with `(0,0)` at the bottom-left corner.

Three viable strategies:

1. **viewBox flip (recommended)** - emit
   `<svg viewBox="0 -PAGE_H PAGE_W PAGE_H" transform="scale(1 -1)">`
   or wrap the entire page body in `<g transform="matrix(1 0 0 -1 0 PAGE_H)">`.
   All x/y coordinates from Lout are then emitted unchanged.
   **Gotcha:** text glyphs will also be vertically flipped. Counter
   with an inverse `transform="scale(1 -1)"` on every `<text>`
   element, OR keep the body in PS coords but apply the flip only at
   the outermost `<g>` and re-flip text individually.

2. **Per-coordinate flip in C** - emit `y_svg = PAGE_H - y_lout` for
   every coordinate touched by `PS_PrintWord` (`z49.c:1402`),
   `PS_PrintUnderline` (`z49.c:1486`), `PS_LinkSource` (`z49.c:1991`),
   `PS_LinkDest`, `PS_LinkURL`, `PS_PrintGraphicInclude`
   (`z49.c:1914`). Requires `page_height_pt` cached at
   `SVG_PrintBeforeFirstPage`/`SVG_PrintBetweenPages` time. Simple
   per-call but introduces a centralized `flip_y()` helper.

3. **Negate y at every emission** - emit `<text y="-VPOS">` and rely
   on a single outer `<g transform="scale(1 -1)">`. Same flip-text
   gotcha as (1).

Recommend **strategy 2** (per-coordinate flip). Pros: text is not
flipped, no nested counter-transforms, output reads naturally in a
browser dev tools inspector. Cons: every coordinate-emitting
callsite must call `flip_y(v)`. The list of callsites is short
(under 10) and they are easy to identify because they all already
emit `v`/`ymk`/`vpos`/`lly`/`ury`.

Coordinate-emission lines in `z49.c` (for grep verification when
porting):

- `z49.c:1395-1402` - `PS_PrintWord` printnum(hpos) / printnum(currenty)
- `z49.c:1486` - `PS_PrintUnderline` `xstop ymk-... ul`
- `z49.c:1503` - `PS_CoordTranslate` `xdist ydist translate`
- `z49.c:1710-1713` - `PS_DefineGraphicNames` LoutGraphic args
- `z49.c:1776-1780` - `PS_SaveTranslateDefineSave` LoutGr2 args
- `z49.c:1900,1914` - `PS_PrintGraphicInclude` translate calls
- `z49.c:1991, 2021, 2051` - link rectangles

## 5. The HARD bits

### 5.1 PS_PrintGraphicObject and the PS-to-SVG translator

`@Graphic { @Box paint { red } { Hello } }` is harmless - the `@Box`
macro library expands to a sequence of PS drawing commands like:

    gsave
    0 0 moveto
    100 0 lineto
    100 50 lineto
    0 50 lineto
    closepath
    1 0 0 setrgbcolor fill
    grestore

This raw PS source becomes an OBJECT (WORD or ACAT tree) which
`PS_PrintGraphicObject` (`z49.c:1642`) splices verbatim into the PS
output. For SVG we must **execute** the PS in a tiny interpreter and
emit equivalent SVG.

Minimum operator set to support (covers Lout's standard library
`include/dg`, `include/diag`, `include/fig`, `include/graph`,
`include/tab`):

| PS operator | Pops | Pushes | SVG mapping |
|---|---|---|---|
| `moveto` | x y | - | start a new path subpath: `d += "M x y "` |
| `lineto` | x y | - | `d += "L x y "` |
| `rlineto` | dx dy | - | `d += "l dx dy "` |
| `curveto` | x1 y1 x2 y2 x3 y3 | - | `d += "C x1 y1 x2 y2 x3 y3 "` |
| `rcurveto` | dx1 dy1 dx2 dy2 dx3 dy3 | - | `d += "c ... "` |
| `arc` | cx cy r a1 a2 | - | translate to SVG arc command `A`; non-trivial because SVG arc takes endpoint + flags, not center+angle |
| `closepath` | - | - | `d += "Z "` |
| `newpath` | - | - | flush current path; start fresh |
| `stroke` | - | - | flush `<path d="..." fill="none" stroke="CURRENTCOLOUR" stroke-width="CURRENTWIDTH"/>` |
| `fill` | - | - | flush `<path d="..." fill="CURRENTCOLOUR" stroke="none"/>` |
| `gsave` | - | - | push interpreter state; open `<g>` |
| `grestore` | - | - | pop interpreter state; close `</g>` |
| `translate` | tx ty | - | accumulate into current `<g>` transform |
| `rotate` | a | - | accumulate `rotate(a)` |
| `scale` | sx sy | - | accumulate `scale(sx sy)` |
| `setrgbcolor` | r g b | - | set interpreter's `current_colour = "rgb(255*r,255*g,255*b)"` |
| `setgray` | g | - | set `current_colour = "rgb(255g,255g,255g)"` |
| `setlinewidth` | w | - | set `current_linewidth = w` |
| `setdash` | array offset | - | parse array, set `current_dasharray` |
| `setlinecap` | n | - | map 0/1/2 -> butt/round/square; set `current_linecap` |
| `setlinejoin` | n | - | map 0/1/2 -> miter/round/bevel |
| `show` | string | - | emit `<text x=CP_X y=CP_Y>STRING</text>`; advance CP_X by approximate width |
| `stringwidth` | string | w h | look up via FontKernLength etc.; push width estimate |
| `concat` | matrix | - | accumulate transform matrix |
| `clip` | - | - | wrap subsequent ops in `<clipPath>` then `<g clip-path="...">` |
| Numeric literals | - | n | push number |
| `def`, `bind`, dictionary ops | varies | varies | best-effort no-op; warn |

The interpreter needs:

- An operand stack (numbers, strings, arrays).
- A graphics state stack (transform matrix, line width, line cap,
  join, dash, colour, current path).
- A current path accumulator (string buffer).
- Knowledge of Lout's pre-defined names: `xsize`, `ysize`, `xmark`,
  `ymark`, `loutf`, `loutv`, `louts`, `in`, `cm`, `pt`, `em`, `sp`,
  `vs`, `ft`, `dg` (defined by `PS_DefineGraphicNames` and the
  prologue at `z49.c:827-834`).
- A dictionary stack with the user's own `def`s so that macros in
  `include/dg` expand correctly.

**Operators with no clean SVG mapping** (warn and emit an XML
comment placeholder):

- `image`, `imagemask` - raster images in PS Level 1 form. Lout's
  standard library does not use these from `@Graphic`; only EPS
  includes do, and EPS is out of scope.
- `colorimage` - same.
- `eofill` - rare; map to `<path fill-rule="evenodd"/>` if needed.
- `flattenpath`, `reversepath`, `pathbbox` - path introspection; not
  needed for any standard Lout macro.
- `charpath` plus `clip` - PS-specific text-as-clip; not used by
  standard macros.
- `setoverprint`, `setstrokeadjust` - print-engine-specific; ignore.
- `where`, `dup`, `exch`, `pop`, `roll`, `index`, `def`, `cvx`,
  `if`/`ifelse`, `for`, `repeat` - stack and control flow operators.
  These ARE needed because Lout's `include/dg` macros use them
  extensively. The interpreter must implement them properly, not
  warn.

Recommend writing the translator as a new `z53_psinterp.c` companion
file (sibling header included from `z53.c`), or as a static block
inside `z53.c`. Estimate: 600-1200 LOC for a workable subset, more
for full robustness. This is the single biggest item in the port.

**Status (2026-05-21).**  Landed in `z53.c` as a static block: an
operand stack, a graphics-state stack, a mark-and-sweep-collected dict
pool, an open-addressed `svg_dict_lookup` hash, and (since commit
`2a33e3d`) an FNV-1a hashed dispatch table over ~175 built-in op
names feeding a `switch (op_id)` (`svg_ps_exec_op` at z53.c ~2645-4150).
The named-value bindings (`xsize`, `ysize`, `xmark`, `ymark`, `loutf`,
`loutv`, `louts`, `in`, `cm`, `pt`, ...) are seeded by
`SVG_DefineGraphicNames` / `svg_ps_init_run`.  Total interp footprint:
~3000 LOC of z53.c.

### 5.2 PS_PrintGraphicInclude

PS version (`z49.c:1880-1935`): opens the file (`OpenIncGraphicFile`
at `z49.c:1909`), wraps in `LoutStartEPSF`/`LoutEPSFCleanUp`,
translates/scales to fit Lout's mark coordinates, then splices the
EPS file contents through `PS_PrintEPSFile`. The `@IncludeGraphicRepeated`
fast-path uses a PS `Form` dictionary with `execform`.

SVG version:

- Switch on file extension (`.svg`, `.png`, `.jpg`, `.jpeg`, `.gif`,
  `.webp`).
- `.svg`: open file, skip XML declaration and DOCTYPE if present,
  extract root `<svg>` element's attributes (width, height, viewBox)
  for sizing, then emit:

      <g transform="translate(colmark-back ymk-fwd) scale(PT PT) translate(-back -back)">
        <svg ...inlined attrs except outer xmlns... >...contents...</svg>
      </g>

  Or, for repeated includes: register the file as a `<symbol id="incg-N">`
  in `<defs>` on first encounter, then emit `<use href="#incg-N" .../>`
  for each instance (mirrors PS `Form` + `execform`).
- `.png`, `.jpg`, etc.: emit `<image x=".." y=".." width=".."
  height=".." xlink:href="file:relative-path"/>`. For
  self-contained output, base64-encode and use a `data:` URI.
- `.eps`: emit an XML comment warning ("EPS not supported in SVG
  mode") plus a `<rect>` placeholder sized to the EPS bounding box,
  so the surrounding layout is undisturbed. Use `PS_FindBoundingBox`
  to size the placeholder.

Resource declaration logic (DSC `%%DocumentNeededResources`) at
`z49.c:1249-1255` has no SVG equivalent: drop entirely.

## 6. CLI wiring

Five files need edits to register the new back-end. Line numbers are
current at time of writing.

### externs.h

- `externs.h:459-489` - flag letters table. Free letters audit (case
  matters): currently used: a, c, C, d, D, e, E, F, h, H, i, I, k,
  l, L, m, M, o, p, P, r, s, S, t, u, U, V, x, w, Z. **Recommend
  `-G`** (mnemonic: G for graphic/SVG). Add:
  ```c
  #define CH_FLAG_SVG        'G'    /* the -G command line flag */
  ```
  Place alphabetically between `CH_FLAG_FNTPATH` (F) and
  `CH_FLAG_HYPHEN` (h).

- `externs.h:582-584` - back-end string names. Add:
  ```c
  #define STR_SVG            AsciiToFull("SVG")
  ```

- `externs.h:2447-2449` - back-end code numbers. Add:
  ```c
  #define SVG                3        /* SVG back end */
  ```

- `externs.h:3754-3755` (after PS externs). Add:
  ```c
  /*****  z53.c   SVG back end    **************************************/
  extern BACK_END SVG_BackEnd;
  extern BACK_END SVG_NullBackEnd;
  ```

### z01.c

- `z01.c:236-240` - extend the enum:
  ```c
  typedef enum {
    BE_PLAIN,
    BE_PS,
    BE_PDF,
    BE_SVG
  } BE_TYPE;
  ```

- `z01.c:689-705` - extend the dispatch:
  ```c
  if( be_type == BE_PLAIN )      BackEnd = Plain_BackEnd;
  else if( be_type == BE_PS )    BackEnd = PS_BackEnd;
  else if( be_type == BE_PDF )   BackEnd = PDF_BackEnd;
  else                           BackEnd = SVG_BackEnd;
  ```
  and the null branch:
  ```c
  if( be_type == BE_PLAIN )      BackEnd = Plain_NullBackEnd;
  else if( be_type == BE_SVG )   BackEnd = SVG_NullBackEnd;
  else                           BackEnd = PS_NullBackEnd;
  ```
  (PDF reuses PS_NullBackEnd in current code.)

- Earlier in `run()`, where command-line flags are parsed (look for
  the loop that switches on `argv[i][1]`), add a `case CH_FLAG_SVG:`
  arm that sets `be_type = BE_SVG`. Reference behavior: existing
  `CH_FLAG_PDF` / `-Z` handling.

### makefile

- `makefile:369-375` - add `z53.o` to OBJS:
  ```make
  OBJS = z01.o z02.o ... z51.o z52.o z53.o
  ```

### z53.c (new file)

- Mirrors the structure of z49.c: file header comment, includes
  `externs.h`, static state, helper functions, callback definitions,
  the `svg_back` struct initializer, public `SVG_BackEnd` pointer,
  null callback set, `svg_null_back` struct, public `SVG_NullBackEnd`
  pointer.

## 7. Implementation phases

### Phase 1 (this task's scope)

Goal: empty stub that compiles cleanly, is selectable, and emits a
valid (empty) SVG document.

- Create `lout/z53.c` with stub bodies for every callback. Most are
  no-op `{}`. The minimum emission for an end-to-end smoke test:
  - `SVG_PrintInitialize`: reset statics, store `out_fp`.
  - `SVG_PrintBeforeFirstPage`: emit
    `<?xml version="1.0" encoding="UTF-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" width="Wpt" height="Hpt" viewBox="0 0 W H">\n`.
  - `SVG_PrintBetweenPages`: emit a comment marker `<!-- page break -->`.
  - `SVG_PrintAfterLastPage`: emit `</svg>\n`.
- Wire externs, z01 enum, z01 dispatch, makefile OBJS, externs.h
  declarations, CH_FLAG_SVG, STR_SVG, SVG=3 code, as in section 6.
- Run `make all`; fix warnings.
- Run `./lout -G doc/user/sample.lt > out.svg` and verify the result
  opens in a browser (it will be empty but well-formed).

Maps to TODO items: 1.1 (all checkboxes).

### Phase 2: text, rules, basic graphics

- Implement `SVG_PrintWord` (the big one for content).
- Implement `SVG_PrintUnderline`.
- Implement `SVG_CoordTranslate`, `Scale`, `Rotate`, `HMirror`,
  `VMirror`, `SaveGraphicState`, `RestoreGraphicState`.
- Implement `SVG_DefineGraphicNames`, `SVG_SaveTranslateDefineSave`
  with simple (un-fused) bodies.
- Verify against 3-5 small text-only docs from `lout/doc/user/`.

Covers ~70% of typical document content. Maps to TODO items 1.2, 1.3,
1.4 (rules portion only), 1.5 (basic colour).

### Phase 3: the PS interpreter

- Write the PS-to-SVG translator (section 5.1).
- Implement `SVG_PrintGraphicObject` using it.
- Verify against `lout/doc/user/all` examples that exercise `@Graphic`,
  `@Diag`, `@Fig`, `@Tab`.

Maps to TODO item 1.4 (path emission).

### Phase 4: include, links, polish

- Implement `SVG_PrintGraphicInclude` (section 5.2).
- Implement `SVG_LinkSource`, `SVG_LinkDest`, `SVG_LinkURL`,
  `SVG_LinkCheck`.
- Texture pattern support (emit `<pattern>` in `<defs>` for each used
  texture from `z52.c`).
- Regression corpus and pixel-diff harness (TODO 1.8).

Maps to TODO items 1.5 (full colour), 1.6 (raster), 1.7 (links), 1.8.

## 8. Risk inventory

### Font metric mismatch (HIGH RISK)

Lout computes line breaks, column widths, and table column widths
ahead of emission using the AFM metrics in `lout/font/` (loaded by
`z37.c`). The text width Lout believes the word `Hello` will occupy
is based on those AFM tables. A browser rendering `<text font-family="Times-Roman" font-size="10pt">Hello</text>`
will use whatever Times-Roman the user's system has, which may not be
metric-identical to Lout's AFM.

Concrete consequences:
- Justified paragraphs will look ragged or have inter-word gaps that
  don't match what Lout expected.
- Right-aligned tables will be off by a few px.
- Table column widths laid out by Lout may not align with the
  browser-rendered cell contents.

Mitigations, ordered from cheap to expensive:
1. Emit explicit `textLength` on every `<text>` element using
   Lout's computed width: `<text x=".." y=".." textLength="W" lengthAdjust="spacingAndGlyphs">`.
   This forces the browser to scale glyphs to match Lout's width.
   Cheap, mostly works. Glyph distortion is usually invisible at body
   font sizes.
2. Emit per-glyph absolute positioning via `<tspan>` arrays with `x="..."`
   listing each glyph's x position from Lout's `FontKernLength`-based
   computation. Expensive in output bytes but pixel-faithful to PS
   geometry.
3. Embed actual font files (woff2) in `<defs>` via `@font-face` data
   URIs. Bulky output. Necessary if perfect rendering is required.

Recommend (1) for v1, (2) as a `--precise-text` flag later.

### @Graphic with obscure PS (MEDIUM RISK)

User-authored `@Graphic { ... }` blocks may contain idiomatic PS
that uses operators outside the supported subset in section 5.1.
Mitigation: emit a clearly marked XML comment with the offending op
and the source position; document the supported subset; provide a
`@SVG` passthrough macro (TODO item 2.3) for users who need to write
SVG directly.

### Multi-page strategy (DESIGN RISK)

Options:
1. **One SVG per page**, separate files: `out-page-1.svg`,
   `out-page-2.svg`, etc. Clean but requires the parent wrapper
   (mdlout) to know how many pages were produced. Lout's normal mode
   writes to one file via `-o`.
2. **One `<svg>` per page** concatenated into one file: each
   `SVG_PrintBetweenPages` closes `</svg>` and opens a new `<svg>`.
   Browsers render each as a separate inline image. Works for HTML
   embedding.
3. **One outer `<svg>` with `<g class="page">` per page**, each
   translated downward by `page_index * page_height`. Renders as one
   long scrollable strip. Good for screen reading, bad for paged
   print CSS.
4. **One outer `<svg>` with `<switch>` and CSS media queries** to
   show one page at a time. Complex.

Recommend (2) for HTML embedding (mdlout's primary use case),
because the HTML wrapper can place each `<svg>` in its own
`<section>` with print-css `page-break-after: always`. Decide
**before** writing `SVG_PrintBetweenPages`.

### XML escaping bugs (LOW-MEDIUM RISK)

Lout produces text via the `EightBitToPrintForm[]` table
(`z49.c:1302, 1421, 1442`) which is PS-escape-friendly (octal
escapes for high-bit chars). For SVG we need XML escaping (`<`, `>`,
`&`, optionally `'` and `"`). Build a dedicated `XMLEscape` helper;
do not reuse `EightBitToPrintForm`.

### Coordinate-flip bugs (LOW RISK)

Easy to miss a y-coord. Add a single `flip_y(v)` macro and grep the
final z53.c for any raw `vpos`, `ymk`, `lly`, `ury`, `currenty`
references that escaped the macro before considering Phase 2 done.

### Cross-reference resolution (LOW RISK)

Lout runs 1-3 times to resolve `@PageMark` / `@PageOf`. The null
back-end is used on non-final runs (`z01.c:701-705`). Make sure
`SVG_NullBackEnd` is a true no-op set; do not accidentally emit XML
on intermediate runs (that would corrupt the final output if the
file is opened in append mode).

### Texture patterns (LOW RISK)

`z42.c`'s `TextureCommand()` returns a PS string. Same call from the
SVG back-end will return PS, not SVG, and pasting it into `fill="..."`
will produce garbage. Either (a) extend z42.c with a `TextureSVG()`
sibling, or (b) parse the well-known PS texture commands inside z53.c
and translate. Recommend (a) for cleanliness; small change.

## 9. Reference

- `lout/z49.c` - PostScript back-end (this guide's source).
- `lout/externs.h:2073-2116` - BACK_END struct.
- `lout/externs.h:2447-2449` - back-end code constants.
- `lout/externs.h:459-489` - CLI flag letters.
- `lout/externs.h:3753-3768` - back-end externs section.
- `lout/z01.c:236-240` - BE_TYPE enum.
- `lout/z01.c:689-705` - back-end dispatch.
- `lout/z42.c:294` - `ColourCommand()` - returns the PS string we
  must intercept and translate to SVG.
- `lout/z52.c` - texture service (analogous).
- `lout/z37.c` - font service; provides `FontSize`, `FontName`,
  `FontHalfXHeight`, `FontKernLength`, `finfo[]` table.
- `lout/z51.c` - plain text back-end; smaller reference for stub
  shapes if z49.c is overwhelming.
- `TODO.md` section 1 - parent task list this guide implements.

## 10. User-guide regression notes

Two fixes applied during user-guide review (2026-05-20):

1. **Colour propagation into @Graphic** — `SVG_DefineGraphicNames` now
   copies `colour(save_style(x))` into the PS interpreter's per-gstate
   `fill_rgb`/`stroke_rgb`.  PS mode achieves this implicitly because
   `SetColourAndTexture` writes `... setrgbcolor` into the PS stream
   before `LoutGraphic`; SVG has no implicit current point, so the
   colour has to be plumbed into the C-side gstate explicitly.  Without
   the fix, paths inside `@Graphic` that did not issue their own
   `setrgbcolor` fell back to `fill="currentColor"` which inherits to
   black at the document level.  This made every page that exercised
   `@Box paint{darkred}` / `@CurveBox paint{...}` / `@Colour ... @FilledBox`
   render those swatches as solid black.  The big visible win is the
   chapter 8 colour gallery (user-guide page 165): all 25 colour boxes
   now display their correct hues instead of being uniform black.

2. **System dict pushes** — `errordict`, `systemdict`, `globaldict`,
   `statusdict`, `$error` (previously unknown ops) now push the
   userdict slot as a stand-in so that the `<dict> begin ... end`
   stanzas at the top of every `*.lpg` prologue stay balanced.  Before
   the fix, `errordict` warned and emitted nothing, then the trailing
   `begin` popped whatever happened to be left on the operand stack
   (typically a literal name from earlier setup), corrupting later
   operations.  Suppresses the recurring "unknown PostScript operator
   'errordict'" warning and removes a class of subtle state-drift bugs.

Both fixes pass 26/26 of the regression suite.

### @Diag connector dropout (fixed)

The thin connector strokes (`stroke-width=0.48`) emitted by
`ldiagdosegpath`/`ldiaglinkend` were silently dropping out across late
pages of the user-guide build.  Root cause was two-fold:

1. **`LoutSetTexture` and `LoutMakeTexture` were complete no-ops** but
   PS-side they consume operands (1 and 11 respectively, with
   `LoutMakeTexture` also producing 1).  Each `@Diag` arrowhead body
   invokes `null LoutSetTexture` as part of its paint procedure --
   leaving the `null` argument stranded on the operand stack.  Across
   ~2700 @Graphic invocations the leftover operands accumulated and
   eventually drifted the stack enough that `ldiagnodeend`/`ldiaglinkend`
   were popping the wrong values for `<linewidth>` and `<linestyle>`,
   causing the connector path to be malformed or its `setlinewidth` to
   pick up the wrong number.

2. **Dict pool leak via tag-dicts**.  `ldiagpushtagdict`/`ldiagpoptagdict`
   create a fresh dict per node/link/label.  When `ldiagpoptagdict` pops
   the dict via the `currentdict end dup /ldiagtagdict known { exit } if`
   loop, the dict briefly sits on the operand stack via `dup` before
   being discarded with `pop` after the loop.  At the moment of the
   `end`, `svg_dict_try_free_anonymous` saw the operand-stack reference
   and (correctly) refused to reclaim the slot -- but it never gets a
   second chance, so each tag-dict permanently consumed a pool slot.
   After ~60 @Diag instances the 1024-slot pool was exhausted and
   subsequent `dict` allocations failed, so any code path needing a
   fresh dict (notably `ldiagdosegpath`'s `12 dict begin`) silently
   degraded into a state where the connector path was never built.

Fix: added a proper mark-and-sweep `svg_dict_gc_sweep` invoked at the
end of every `svg_ps_run`.  Roots are the dict stack and the operand
stack; unreachable in-use pool slots get reclaimed.  Combined with
making `LoutSetTexture`/`LoutMakeTexture` pop their operands, the
user-guide build now emits ~2200 `stroke-width=0.48` paths (vs. ~80
before the fix) and pool_used stays under 20 indefinitely.

### Remaining known issues

- Texture patterns (`LoutMakeTexture`/`LoutSetTexture` now pop operands
  but still ignore the texture body): `@Box paint{black} texture{brickwork}`
  collapses to a solid black rectangle in SVG.  Documented design
  choice; needs `<pattern>` emission in `<defs>` to fix.  The texture
  body is itself a PostScript procedure passed to `LoutMakeTexture` so
  a clean implementation would either special-case the @TextureCommand
  names recognised by `coltex` (`brickwork`, `honeycomb`, `striped`,
  `grid`, `dotted`, `chessboard`, `triangular`, `string`) and emit
  prebuilt `<pattern>` elements, or run the texture procedure through
  the interpreter while redirecting its draw ops into a per-pattern
  buffer.

  **2026-05-20 update.**  `svg_tex_identify` now hardened against custom
  (non-coltex) paintprocs.  Each named-texture branch in z53.c requires
  positive corroborating signals (e.g. `dotted` needs `arc + fill` AND
  the absence of setdash/stroke/closepath/lineto; `chessboard` needs
  `>=2 moveto`, `0 lineto`, `>=4 rlineto`).  Custom procs that match
  none of these patterns fall through to `SVG_TEX_SOLID`, which the
  caller interprets as "no <pattern> fill" -- so the surface keeps its
  base colour rather than crashing or being mis-mapped to an unrelated
  named texture.  All 8 named textures continue to round-trip through
  `/tmp/tex.lt` (the small per-texture box document used for spot
  checking).

- **@Graph axes missing on `style { axes }` graphs with `xorigin { 0 }` /
  `yorigin { 0 }`** (user-guide pp 248, 262) -- **fixed 2026-05-20**.

  Root cause was in the `eq` operator, not in any of the symbol-drawing
  arithmetic that the previous round was hunting through.  graphf.lpg's
  `axesstyle` dispatches on
      `xaxis false eq yaxis false eq or { framestyle } { ... } ifelse`
  to choose between drawing a full frame (when `xaxis` / `yaxis` was
  literally `false`, meaning "no origin given") or drawing crossed
  axes (when both have numeric values).  Our `eq` implementation
  treated `SVG_VK_NUM` and `SVG_VK_BOOL` interchangeably -- both have
  `vv.num` holding the value, with `false` represented as `0.0` -- so
  `0 false eq` returned `true` and the dispatch always took the
  framestyle branch.  Every `xorigin { 0 }` / `yorigin { 0 }` graph
  (the common case) rendered as a frame with inward tick stubs and no
  axis lines instead of the proper crossed axes with outward ticks.

  Fix: `eq` / `ne` now require both operands to be the same kind
  (NUM-vs-NUM or BOOL-vs-BOOL) before comparing `vv.num`.  Cross-type
  comparisons fall through to the `kind == kind` fallback, which is
  false for NUM vs BOOL.  String and name comparisons are unchanged.

  After the fix the axesstyle dispatch correctly takes the axes
  branch, emitting the two crossed lines `M 0 0 L xsize 0` and
  `M 0 0 L 0 ysize`, and the tick procs draw their tick marks
  centred on the axis (outward) rather than below the (absent) frame.
  Tick numeric labels are still missing because `show` is still a
  stub in @Graphic context; that is a separate text-emission issue.
  All 30 regression snippets continue to pass.

- **@Sym / @Char Symbol-font glyph gap in SVG output** -- **audited
  2026-05-21**, **not fixable from the include/ layer**.

  Audit.  Both `bsf`'s `@Sym` (`{Symbol Base} @Font @Char x`, glyph
  name argument) and `eqf`'s `@Sym` (`{Symbol Base} @Font x`, octal
  byte argument) ultimately produce a byte that flows through
  `SVG_PrintWord` -> `svg_emit_word_text` -> Symbol-font LCM lookup
  (`maps/Symb.LCM`) -> `svg_glyph_to_unicode` (z53.c, line 451).  The
  current `svg_glyph_table` in z53.c covers Latin-1 (ISO-8859-1) glyph
  names plus a handful of typographic punctuation, but contains **none
  of the 138 Symbol-font glyph names** that `Symb.LCM` actually emits.

  Concretely, of the 188 named entries in `Symb.LCM`, 138 fall through
  to the `cp = c` (raw byte -> Latin-1 -> UTF-8) fallback in
  `svg_emit_word_text`.  This means `@Sym "alpha"` renders as the
  Latin letter `a`, `@Sym "minute"` renders as U+00A2 (cent sign),
  `@Sym "integral"` renders as U+00F2 (`o-with-grave`), etc.

  Missing Symbol-font glyph names include:

  - All Greek lower-case and capital letters (alpha..omega,
    Alpha..Omega), plus the variant forms `theta1`, `phi1`, `sigma1`,
    `omega1`, `Upsilon1`
  - Math operators: `integral`, `integraltp/ex/bt`, `summation`,
    `product`, `radical`, `radicalex`, `partialdiff`, `gradient`,
    `dotmath`, `asteriskmath`, `proportional`, `infinity`,
    `lessequal`, `greaterequal`, `notequal`, `approxequal`,
    `equivalence`, `congruent`, `similar`, `perpendicular`, `angle`,
    `therefore`, `existential`, `universal`, `suchthat`
  - Set operators: `element`, `notelement`, `propersubset`,
    `propersuperset`, `notsubset`, `reflexsubset`, `reflexsuperset`,
    `union`, `intersection`, `emptyset`, `aleph`
  - Logic: `logicaland`, `logicalor`, `logicalnot`
  - Geometry: `circleplus`, `circlemultiply`, `lozenge`, `weierstrass`
  - Arrows: `arrowleft/right/up/down`, `arrowboth`, `arrowdblleft`
    etc., plus the extension pieces `arrowvertex`, `arrowhorizex`,
    `carriagereturn`
  - Fences (used by eq for big brackets): `bracketlefttp/ex/bt`,
    `bracketrighttp/ex/bt`, `parenlefttp/ex/bt`, `parenrighttp/ex/bt`,
    `bracelefttp/mid/bt`, `bracerighttp/mid/bt`, `braceex`
  - Suits: `club`, `diamond`, `heart`, `spade`
  - Misc: `minute`, `second`, `degree`, `Ifraktur`, `Rfraktur`,
    `angleleft`, `angleright`

  Observation in the User's Guide build (`lout/doc/user/all`):
  `grep -cE '[<greek-range>]' /tmp/user.svg` -> 0 hits, despite 1182
  references to `font-family="Symbol"` in the same file.  Every Greek
  letter and math operator in the User's Guide currently renders
  silently as the wrong glyph in SVG output.

  Why the include/-layer workaround in the brief does not apply.
  The task brief proposed switching `@Sym` to emit `@Char "name"`
  inside `@BackEnd @Case { SVG @Yield { ... } }`.  This was based on
  the premise that `@Char` would route through a different code path
  than raw bytes in SVG mode.  It does not: `@Char "alpha"` in Lout's
  parser resolves to **the same byte** (0x61 with Symbol-font
  encoding) that `@Sym "\141"` produces, and from `SVG_PrintWord`'s
  perspective the two are indistinguishable.  Both land at the same
  `svg_glyph_to_unicode("alpha")` call that returns 0.

  There is no Lout-layer mechanism that bypasses `svg_emit_word_text`
  for ordinary text runs.  The only passthroughs are `@Graphic` and
  the PS-prologue paths, neither of which is appropriate for inline
  symbols inside galleys / equations.

  Conclusion.  The fix must live in `z53.c`: extend `svg_glyph_table`
  with the full Symbol-font glyph -> Unicode set (and, for full
  coverage, also the `Ding.LCM` Zapf Dingbats names a1..a999).  A
  reasonable mapping for the Symbol font is the one in Adobe's
  `glyphlist.txt` cross-referenced with Unicode's Mathematical
  Operators (U+2200..U+22FF), Miscellaneous Mathematical Symbols
  (U+27C0..U+27EF), Arrows (U+2190..U+21FF), and Greek and Coptic
  (U+0370..U+03FF) blocks.  This is a ~150-entry static-array
  extension; deferred this round per the no-touch on z53.c.

  Verification this round.  All 53 regression snippets still pass
  (`tests/run_all.sh` -> 53/0, Pass-Excellent 24/24).  No
  `include/` files were changed; the audit is documentation-only.

### Rotated `show` inside @Graphic (fixed 2026-05-22)

`svg_ps_show` was emitting text without honouring the CTM-rotation
component of the surrounding PS state.  PS prologue idioms of the form
`translate N rotate offset moveto show` (e.g. `ldiagshowtags` in
`diagf.lpg`, plus any user `@Graphic` body that rotates the frame
before showing text) ended up with the text positioned at the rotated
origin but rendered upright in the SVG output.  In the User's Guide
this hit pages 205-207 (the @Diag "compass-point labels" demo on
labels.) — the named-direction labels around the box rendered upright
in SVG but rotated 40 degrees in PS.

Fix: in `svg_ps_show`, probe the path-delta's x-axis direction by
transforming `(0,0)` and `(PT,0)` and computing
`atan2(dy, dx) * 180/PI`.  When that angle is non-trivial
(`|angle| > 0.01`), the emitted text wrapper becomes
`<g transform="translate(x,y) rotate(angle) scale(1,-1)">`.  The
rotate sits before the scale(1,-1) so it is interpreted in the bottom-
left frame inside the page-level Y-flip group (CCW positive, matching
PS).  Adds ~22 LOC to z53.c.  Regression snippet
`graphic_rotated_show.lt` (12 labels evenly placed around a circle,
each rotated 30 degrees further than the last) was added to the test
corpus; it passes Pass-Excellent (AE=8749, SSIM=0.9927).

### Known remaining @Fig / @Diag limitations

The following come from a 2026-05-22 audit of user-guide pages with
diff_ratio > 7% that lie in the @Diag chapter (pages ~193-225) or in
@Fig-using sections.  None are real layout bugs; all are either
already-tracked irreducibles or fall outside the no-touch budget for
this round.

- **Pagination drift in dense prose pages adjacent to @Diag examples**
  (e.g. pages 205, 207, 213, 219).  Same paragraphs, half-line vertical
  offset between PS and SVG.  Caused by the Ghostscript vs librsvg
  text-antialiasing-at-the-pixel-level disagreement compounding into
  Lout line-break decisions over a long page.  Documented at length
  in `tests/user_guide_diff/README.md` "SSIM vs AE".  No back-end fix.

- **Faux-italic body text inside @Diag node bodies** (e.g. page 207's
  `label` placeholder, drawn as `1.0 fnt5  0.5 0.5 0.5 LoutSetRGBColor
  ...(label)m`).  This is regular Lout text, not PS-prologue
  text, so it goes through `SVG_PrintWord` and not `svg_ps_show`.
  It renders correctly modulo the same antialiasing floor.

- **@Graphic-emitted text inside non-uniform-scale CTM**.  The new
  rotation-recovery code uses `atan2(dy, dx)` on the path-delta x-
  basis; it correctly captures rotation but loses any non-uniform
  scale or shear.  In the User's Guide the only PS-prologue paths that
  build a sheared CTM around a `show` are inside `coltex` (texture
  preview swatches), which already routes through `<pattern>` and not
  through `svg_ps_show`.  No live page in the User's Guide exercises
  sheared `show`.  Deferred: emit a full `matrix(a b c d e f)` transform
  in `svg_ps_show` when `|a*d - b*c - 1| > epsilon` (would also need
  to undo any non-uniform scale baked into `font_size`).

- **`@Fig` per-figure caption layout**.  `@Fig` itself is just a thin
  wrapper that emits its body through @Graphic; layout differences
  between PS and SVG on @Fig pages are dominated by the surrounding
  Lout galley (caption text, figure number).  The fig_multi and
  fig_numbering snippets exercise this path and pass.  No real bug.

  Verification.  All 63 regression snippets pass
  (`tests/run_all.sh` -> 63/0, Pass-Excellent 63/63).  The new
  `graphic_rotated_show.lt` snippet guards the rotated-show fix
  against future regressions.

- **OpenType GSUB features (smcp / onum), phase 1**.  The CFF/OTF
  font loader in `z53_glyph.c` now also parses the `GSUB` table for
  the small-caps (`smcp`) and old-style-figures (`onum`) features.
  The parser walks Script -> default-LangSys -> Feature -> Lookup
  lists, applies any Lookup Type 1 (Single Substitution, formats 1
  and 2) subtables, and projects the resulting GID->GID map through
  Adobe StandardEncoding into a Latin-1 codepoint -> GID table
  stored on the `svg_glyph_font` record.  Public API:
  `svg_glyph_font_smcp_substitute`, `svg_glyph_font_onum_substitute`,
  `svg_glyph_font_has_feature`.  A new `font_features` bitmask field
  on `svg_gstate` (declared in z53.c) anticipates the consumer side
  but is not yet wired through: the current `<text>` emission path
  in `svg_emit_word_text` writes Unicode codepoints, and small-caps
  glyphs have no Unicode codepoint of their own, so applying the
  substitution at the codepoint level is a no-op.  The architectural
  follow-up is to switch body text on these features over to glyph-
  path emission (already supported by `svg_glyph_emit_outline` for
  charpath).  Other GSUB Lookup Types (2 multiple, 3 alternate, 4
  ligature, 5/6 contextual, 7 extension, 8 reverse-chained) and
  TrueType GSUB are out of phase 1 scope.  Tests:
  `tests/snippets/text_smallcaps.lt` and
  `tests/snippets/text_oldstyle_figures.lt` exercise the surrounding
  text path but currently render lining figures and lower-case in
  both back-ends (parser-only, consumer is the deferred work).
