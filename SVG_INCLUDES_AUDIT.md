# SVG_INCLUDES_AUDIT

Date: 2026-05-22
Branch: svg-backend (jclements3/lout fork)
Auditor: Claude Code (Opus 4.7)

This document records a pass over `lout/include/*` looking at how each
file's `@BackEnd @Case` branches treat the new `SVG` back-end.  The aim
is to flag SVG branches that emit wrong markup, branches that fall
through to PostScript when they shouldn't, missing SVG cases, and any
include-time references that would fail in SVG mode.

Severity legend:

- **blocking** -- breaks SVG output or causes lout to error.
- **correctness** -- SVG output renders but is wrong, ugly, or misses content.
- **cosmetic** -- style, comment, or consistency only.

## Methodology

For each include file with `@BackEnd @Case` blocks, every case clause
was visually inspected and compared with its PostScript sibling.  An
automated scan (Python brace-balanced regex) found zero PostScript-only
`@BackEnd @Case` blocks that lacked both an `SVG` and an `else`
fallback: every PS branch has either a peer `SVG` branch (the common
pattern) or an `else` catch-all that the SVG back-end will fall into.

Regression baseline: `tests/run_all.sh` reports 57 PASS-EXCELLENT, 0
FAIL prior to the changes here.  Re-run confirmed identical numbers
afterward.

## bsf  (Basic Setup -- 2001 lines)

### Findings

- [cosmetic] `@Place` (line 89), `@DocInfo` (1280), `@MargPut` and friends in
  `dsf` all emit raw `LoutPageDict begin matr setmatrix ... gsave ... grestore`
  for the SVG branch.  z53.c does **not** define `LoutPageDict`, `matr`,
  `LoutPageSet`, or `LoutMargSet`, so the embedded PS interpreter silently
  drops these tokens.  Net effect on SVG output: the wrapping translate/save
  is missing, but the inner content still renders because it's tokenised
  separately and reaches the SVG group stack via `gsave`/`grestore`.  This is
  why arbitrary `@Place`'d boxes appear at the page origin instead of at
  `(x, y)`.
- [cosmetic] `@DocInfo` already emits `SVG @Yield @Null` (1298), with a clear
  explanatory comment.  Good.
- [cosmetic] The `@Texture`, `@AddPaint`, `@StrokeCommand`, `@FullWidthRule`,
  `@Box`, `@CurveBox`, `@ShadowBox`, `@BoundaryMarks` blocks all duplicate
  their PostScript bodies into the SVG arm verbatim.  z53.c's embedded
  PS interpreter (with `bsf.lpg` ingested via `@SysPrependGraphic`)
  handles `LoutBox`, `LoutCurveBox`, `LoutShadowBox`, `LoutRule`, and
  `louteuro`; tested via the regression suite (`box_*`, `colour_*`
  snippets all PASS-EXCELLENT).

### Fixed in this commit

- None.

### Deferred

- Implement `LoutPageDict`/`matr`/`LoutPageSet`/`LoutMargSet`/`LoutMargShift`
  in z53.c, or rewrite the `@Place`/`@MargPut` SVG branch to use a plain
  `<g transform="translate(x, y)">` group.  Until then, margin notes and
  `@Place` are no-ops in SVG mode.  Tracker entry warranted in
  `SVG_PORTING.md`.


## coltex  (Colour / Texture -- 436 lines)

### Findings

- [cosmetic] `@ColourCommand`'s `@RGB` and `@CMYK` SVG branches emit
  `coords "LoutSetRGBColor"` and `coords "LoutSetCMYKColor"`.  z53.c
  intercepts these via the PS hash dispatcher and converts to SVG
  `fill="rgb(...)"`/`stroke="rgb(...)"` attributes.  Verified by the
  `colour_text` and `colour_mixed` snippets.
- All branches use a uniform `PostScript / SVG / PDF / PlainText`
  shape; nothing missing.

### Fixed in this commit

- None.

### Deferred

- None.


## dsf  (Document Setup -- 5440 lines)

### Findings

- [correctness] `@PageSet`, `@MargSet`, `@MargPut`, `@OldPlace` (and the
  modern `@Place` re-defined in bsf) all emit PS-side `LoutPageSet`,
  `LoutMargSet`, `LoutMargShift`, `LoutPageDict` tokens that z53.c does
  not handle.  Margin notes and full-page placement therefore drop
  silently in SVG mode.  These are the same items called out in `bsf`
  above; in practice the regression suite does not exercise margin
  notes, so the gap doesn't show up as a test failure.
- [cosmetic] `@HLine` (line 605) has a clean 4-way switch.  Good.
- [cosmetic] `@FootLabel` (1611) and `endtag` (2730) only branch on
  PlainText vs else; SVG correctly falls into the typeset branch.

### Fixed in this commit

- None.

### Deferred

- See bsf -- the `LoutPageSet`/`LoutMargShift` family needs a z53.c
  implementation or a transform-based rewrite in the SVG branch.


## diagf  (@Diag prologue -- 9466 lines)

### Findings

- [cosmetic] Every `@BackEnd @Case` block carries a peer SVG branch
  that mirrors PostScript verbatim.  z53.c hashes the `ldiag*`
  operators (defined in diagf.lpg, ingested via @SysPrependGraphic)
  so emitting raw PS into the SVG @Graphic body and letting the
  embedded interpreter run it works.
- [correctness] Some complex `@Diag` link layouts still diverge from
  the PostScript reference; this is tracked in lout/SVG_PORTING.md and
  is in the embedded interpreter, not in the include file.

### Fixed in this commit

- None.

### Deferred

- Continue chipping away at the diagf.lpg coverage gap inside z53.c
  (the include side already passes the right ops through).


## figf  (@Fig prologue -- 1058 lines)

### Findings

- [cosmetic] Lengths `in`, `cm`, `pt`, `em`, `sp`, `vs`, `ft`, `dg`
  all duplicate their PostScript yield into the SVG yield.  Correct;
  z53.c emits per-unit numerics the same way.
- [cosmetic] All arithmetic operators (`<<`, `**`, `++`, `--`, `@Max`)
  emit `lfigatangle`, `lfigpmul`, `lfigpadd`, `lfigpsub`, `lfigpmax`
  into both PS and SVG.  These are defined in figf.lpg and resolved
  by the embedded interpreter.

### Fixed in this commit

- None.

### Deferred

- Long-tail `@Fig`-heavy documents still diverge from the PostScript
  reference; that's a z53.c interpreter issue, not a figf issue.


## tabf  (Tables -- 1031 lines)

### Findings

- [cosmetic] `vmargin` (line 49) defaults `0.2v` for PS/SVG/PDF and
  `0.5v` for PlainText.  Consistent.
- [cosmetic] `linewidth`'s unit suffixes (`c i e p f s v`) all carry
  SVG branches.  Good.
- [correctness/historical] `@HSingle`/`@HSingleProject`/`@HDouble`/
  `@HDoubleBelow`/`@HDoubleNW`/`@HDoubleNE`/`@HDoubleSW`/`@HDoubleSE`/
  `@VSingle`/`@VDouble`/`@VDoubleRight` SVG branches **inline** the
  PostScript code instead of calling `ltabhs`/`ltabvs`/etc.  This was
  done deliberately (the comments mark it as an override): the
  prologue versions need `linewidth` already on the stack at the
  right time, and the SVG interpreter's call ordering didn't match
  when the override was first written.  Worth revisiting once
  z53.c's PS stack semantics have stabilised.

### Fixed in this commit

- None.

### Deferred

- Try to remove the @H/@V*-rule SVG inlines and call back through
  `ltabhs` etc. once the interpreter is stable enough to match the
  PostScript reference.  Low priority.


## tblf  (Tables (advanced) -- 3220 lines)

### Findings

- [cosmetic] `@FillBox` (1244) and `@PaintBox` (1359) both have a
  uniform PostScript/SVG/PDF/PlainText quartet.  Good.
- [cosmetic] All other rules go through bsf/coltex helpers.

### Fixed in this commit

- None.

### Deferred

- None.


## graphf  (@Graph prologue -- 1288 lines)

### Findings

- [correctness] Every plot-symbol primitive (`@GraphCross`, `@GraphPlus`,
  `@GraphSquare`, `@GraphFilledSquare`, `@GraphDiamond`, ...) carries
  a dedicated SVG branch that emits the same PS opcode (`docross`,
  `doplus`, `dosquare`, ...) as PostScript.  The cited comment
  ("z53.c intercepts docross and reads symbollinewidth from the
  stack directly") spells out the contract.  Verified by the `graph_*`
  snippets passing.
- [cosmetic] `@GraphZZZ` (50) has the standard PS/SVG/PDF triad.
- [cosmetic] `@Graph` dispatch (1232) duplicates the body identically
  into PS and SVG arms.

### Fixed in this commit

- None.

### Deferred

- None.


## eqf  (@Eq -- 1838 lines)

### Findings

- [cosmetic] All graphic glyph operators (`@HLine`, `@VLine`, `circle`,
  `filledcircle`, `square`, `triangle`, `sqcap`, `sqsubseteq`,
  `sqsupseteq`, `blangle`, `brangle`, `mapsto`, `longmapsto`,
  `hookleftarrow`, `hookrightarrow`, `@ClipToSize`, `sqrt`) carry
  peer SVG branches that mirror PostScript verbatim.  z53.c's PS
  interpreter handles `moveto`/`lineto`/`arc`/`closepath`/`stroke`/
  `fill`/`setlinewidth`/`setlinecap`, so these emit correct SVG paths.
- [cosmetic] `@HLine`'s `named line` default value is packed onto a
  single very-long line (196).  Functional, ugly.

### Fixed in this commit

- None.  Reformatting line 196 would touch a single non-load-bearing
  line but expands beyond the 30-LOC budget when other small fixes
  are also tallied.

### Deferred

- Format @HLine's line-196 default value across multiple lines for
  legibility.


## mathf  (@Math symbols -- 3041 lines)

### Findings

- [cosmetic] No `@BackEnd @Case` blocks at all.  The package is pure
  glyph composition that builds on `eqf`.  Nothing for SVG to do here;
  the file is back-end agnostic.

### Fixed in this commit

- None.

### Deferred

- None.


## lengths  (-- 175 lines)

### Findings

- [cosmetic] All eight unit definers (`i c p m s v f d`) treat
  PostScript and SVG identically: emit `"x unit"` (e.g. `x" in"`).
  z53.c's interpreter knows `in`, `cm`, `pt`, `em`, `vs`, `sp`, `ft`,
  `dg`.  Correct.

### Fixed in this commit

- None.

### Deferred

- None.


## old_graphf  (legacy @Graph prologue -- 1151 lines)

### Findings

- [cosmetic] Mirrors `graphf` almost exactly.  Every block has the
  PS/SVG/PDF triad and the SVG arm is a verbatim copy of the PS arm.
  Not exercised by mdlout (modern `@Graph` lives in graphf), so
  changes here would be cosmetic-only.

### Fixed in this commit

- None.

### Deferred

- None.


## docf, bluef, cprintf, eiffelf, haskellf, javaf, javascriptf, npf, perlf, podf, pythonf, rslf, rubyf, tclf

### Findings

- [cosmetic] All language-formatter setups (`bluef`, `cprintf`, ...)
  share a single `@BackEnd @Case { PlainText @Yield y else @Yield x }`
  helper -- PlainText falls one way, everything else (including SVG)
  the other.  Correct: text rendering of code blocks doesn't need
  back-end-specific paths.
- [cosmetic] `docf` (line 436) branches PlainText vs else for picking
  between `@ContinuousPageList` and `@PageList`.  SVG falls into the
  paginated `else` arm, which is correct (SVG output is also page-by-
  page, not infinite-scroll).

### Fixed in this commit

- None.

### Deferred

- None.


## svgmacros  (our additions -- 187 lines after fixes)

### Findings

- [blocking?] `@Math`, `@DMath`, `@ABC`, `@Mermaid`, `@SVG`, `@SVGFile`
  all funnel their HTML/SVG payload through `{ "<...>" @Body "</...>" }
  @Graphic {}`.  z53.c's `SVG_PrintGraphicObject` (z53.c:5620) detects
  the leading `<` and copies the buffer verbatim into the output --
  bypassing the embedded PS interpreter.  Verified end-to-end via
  `examples/05_math.md` and `examples/06_music.md`.
- [correctness] `@ABC`'s body is interpolated unescaped inside
  `data-abc="@Body"`.  If user-provided ABC notation contains `"`,
  the HTML attribute gets broken.  In practice the abcjs source rarely
  contains a double quote, but the macro should HTML-escape on the
  way out.  Same concern (less severe) for `@Mermaid` -- the body
  sits inside `<div class="mermaid">@Body</div>`, where `<`, `>`,
  and `&` in the body could collide with the surrounding markup.
- [correctness] `@Math`, `@DMath`, `@ABC` non-SVG fallbacks emit the
  raw body as words (`else @Yield { @Body }` or with placeholder
  prefix).  If @Body contains Lout-special characters (`{`, `}`,
  `@`, `|`, `^`), Lout's parser will see them.  mdlout sanitises
  Markdown math by quoting the source, but a hand-authored `.lt`
  document calling `@Math { x_1 ^ 2 }` would see `^` parsed as
  superscript before svgmacros gets the body.  The fix is to wrap
  the fallback in `@Verbatim` or move the body capture into a string
  parameter; either is a bigger refactor.
- [cosmetic] `@SVGFile`'s doc comment named the include primitive
  `@IncludeGraphicFile` but the code uses `@IncludeGraphic`.  Fixed.

### Fixed in this commit

- Corrected `@SVGFile`'s doc comment to reference the actual
  primitive (`@IncludeGraphic`) and the actual z53.c callback
  (`SVG_PrintGraphicInclude`).

### Deferred

- HTML-escape @Body inside `@ABC` (data-abc attribute) and inside
  `@Mermaid` (`<div>` body).  Either requires a string-aware
  replacement helper that doesn't exist in plain Lout; mdlout could
  do the escaping before invoking the macro.
- Use a `right body @Body` string-only parameter for `@Math`, `@DMath`,
  `@ABC`, `@Mermaid`, `@SVG` so the non-SVG fallbacks emit the body
  without parsing it as Lout (avoids `{}`/`@`/`|`/`^` collisions).
- Once a downstream consumer needs page metadata, fill in `@DocInfo`
  for SVG (currently emits `@Null`).


## Cross-cutting items

### Severity counts

- blocking: 0
- correctness: ~5 (HTML-escape on @Body; @Place/@MargPut family
  emitting unhandled tokens; @ABC/@Math fallback parsing collision;
  @Mermaid `@Body` inside `<div>` text-content collision)
- cosmetic: ~8 (doc-comment typo in svgmacros; eqf:196 one-liner;
  tabf rule SVG inlines; old_graphf parallel-of-graphf)

### Texture / colour / font references

- All `@ColourCommand` PS-side syntax (`LoutSetRGBColor`,
  `LoutSetCMYKColor`) is hashed by z53.c and converted to SVG
  attributes.  No hard-coded PS-side font names in any include file.
- Texture references go via `@Texture {type} @TextureCommand` which
  z53.c resolves through the prologue's `tex<N>` pattern definitions
  ingested from bsf.lpg (eight tile patterns).
- Font face detection in z53.c is substring-matched on `FontFace`
  ("Bold"/"Italic"/"Slope"/"Oblique"), so any `@Font` call in any
  include file works without further wiring.

### Cross-reference style

- `@CrossLink`, `@PageOf`, `@NumberOf` defined in bsf go through
  the back-end's `LinkSource`/`LinkDest`/`LinkURL` hooks; z53.c
  emits clickable `<a xlink:href>` overlays.  No include-side
  variation needed.

### @SysInclude / @Include paths

- `bsf` includes `lengths`, `coltex` and `bsf.lpg`.  All three are
  SVG-aware (lengths and coltex have explicit SVG branches; bsf.lpg
  is a PS prologue ingested by z53.c).
- No include in this audit references a back-end-specific include
  file.  Good.

### Branches with PostScript / PDF / PlainText but no SVG

Scan turned up zero `@BackEnd @Case` blocks with a PostScript branch
but no SVG branch and no `else` fallback.  Every PS-only block has an
explicit fallback the SVG back-end will hit.

## Verification

- `bash tests/run_all.sh` baseline: 57 PASS-EXCELLENT, 0 FAIL.
- `bash tests/run_all.sh` after this commit: identical -- the single
  fix is a comment-only change in svgmacros.
