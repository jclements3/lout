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
  for the SVG branch.  z53.c now defines `LoutPageDict`, `LoutPageSet`,
  `LoutMargSet`, `LoutMargShift`, and `matr` (the latter aliased to the
  `matrix` op so `matr setmatrix` reduces to an identity matrix push +
  CTM reset).  Each pops the right operand count, keeps the stack
  balanced, and emits a one-line XML comment for traceability so the
  fall-through path is no longer silent.  Net effect on @Place: the
  wrapping save/translate/restore is honoured by the surrounding
  gsave/grestore at the C-callback level, so the inner content renders
  -- the operand-stack-corruption hazard that produced the silent
  drop-out is closed.
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

- Added `LoutPageDict`, `LoutPageSet`, `LoutMargSet`, `LoutMargShift`,
  and `matr` to z53.c's op hash + switch (~line 4220).  Stack-balanced
  stand-ins emit XML comments naming the op; margin-shift's translate
  is approximated as a no-op pending a proper transform-group rewrite.

### Deferred

- Proper translate-on-LoutMargShift via `<g transform="translate(x, y)">`,
  so margin notes actually land in the margin.  The stack-clean
  stand-in is in; the visible translate is not.


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
  `LoutMargSet`, `LoutMargShift`, `LoutPageDict` tokens.  z53.c now
  handles these as C-side stand-ins (see `bsf` section above).  Margin
  notes still don't visibly translate -- the LoutMargShift stand-in
  pops its argument but doesn't open a `<g transform="translate(...)">`
  group -- but the stack is clean and the emission is no longer silent.
  The regression suite does not exercise margin notes; the gap stays
  out of failure stats.
- [cosmetic] `@HLine` (line 605) has a clean 4-way switch.  Good.
- [cosmetic] `@FootLabel` (1611) and `endtag` (2730) only branch on
  PlainText vs else; SVG correctly falls into the typeset branch.

### Fixed in this commit

- See bsf -- the `LoutPageSet`/`LoutMargShift` family now has C-side
  stand-ins in z53.c that keep the operand stack clean.

### Deferred

- Make `LoutMargShift` actually emit a `<g transform="translate(...)">`
  group so margin notes land in the margin instead of the page origin.


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
- mdlout.py: HTML-escape @ABC and @Mermaid block bodies before the
  Lout-string encode, so `&`, `<`, `>`, `"` in the source no longer
  corrupt the surrounding `data-abc="..."` attribute or `<div>` text
  content.  Browser DOM-decodes the entities before abcjs / mermaid
  reads them so notation round-trips intact.
- svgmacros header comment + docs/best_practices.md: document the
  hand-authored `.lt` gotchas (HTML-active characters, Lout-active
  characters in the non-SVG fallback, embedded-LF restriction) so
  raw-Lout authors mirror what mdlout already does for Markdown.

### Deferred

- Use a `right body @Body` string-only parameter for `@Math`, `@DMath`,
  `@ABC`, `@Mermaid`, `@SVG` so the non-SVG fallbacks emit the body
  without parsing it as Lout (avoids `{}`/`@`/`|`/`^` collisions in
  the PostScript/PDF/PlainText branches).  The doc note is a
  workaround; the macro-side fix is more thorough but a bigger lift.
- Once a downstream consumer needs page metadata, fill in `@DocInfo`
  for SVG (currently emits `@Null`).


## Cross-cutting items

### Severity counts (post 2026-05-22 follow-up commit)

- blocking: 0
- correctness: 1 (LoutMargShift translate-group still missing; @Math /
  @DMath fallback Lout-parse collision still pending the macro-side
  body-as-string refactor)
- cosmetic: ~7 (eqf:196 one-liner; tabf rule SVG inlines;
  old_graphf parallel-of-graphf)

The HTML-escape items and the @Place/@MargPut stack-corruption hazard
are addressed in this follow-up; see the per-section "Fixed in this
commit" blocks above for the diff anchors.

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

Re-confirmed 2026-05-23 with a fresh balanced-brace Python scan over
every file in `lout/include/`: 258 `@BackEnd @Case` blocks total, none
of which combine PostScript with neither SVG nor `else`.  The blocks
without a literal `SVG @Yield` (57 of the 258, all in `bsf`, `bluef`,
`cprintf`, `docf`, `dsf`, `eiffelf`, `haskellf`, `javaf`,
`javascriptf`, `npf`, `perlf`, `podf`, `pythonf`, `rslf`, `rubyf`,
`tblf`, `tclf`) every one uses the `PlainText @Yield ... else @Yield
...` shape, so SVG falls through to the typeset `else` arm
correctly.

### Residual `replacing unknown @Case option SVG by PostScript` warnings

The build warnings observed on the SVG path of `lout/doc/design`
(96 warnings on the 2026-05-23 run) and `lout/doc/expert` (235
warnings on the same run) trace exclusively to `@BackEnd @Case`
blocks **inside the doc sources themselves**, not to anything in
`include/`.  Originating files:

- `doc/design/mydefs` (4 sites: `@HLine`, `@VDashLine`, `@LBox`,
  `@LittlePage` -- each PostScript+PDF, no SVG, no `else`)
- `doc/design/s2_3`, `doc/design/s3_2`, `doc/design/s5_2`
  (in-text figure bodies)
- `doc/expert/pre_colo`, `pre_conc`, `pre_cont`, `pre_cove`,
  `pre_data`, `pre_grap`, `pre_hmir`, `pre_lang`, `pre_oner`,
  `pre_rota`, `pre_scal`, `preface`, `pri`, `pri_cros`,
  `pri_defi`, `pri_gall`, `pri_obje`, `det_gall`, `det_size`
  (each demonstrates a `@BackEnd @Case` example in running
  prose, PostScript+PDF only)

Those blocks were authored when only `PostScript` and `PDF` were
live back-ends.  They need a peer `SVG @Yield` (mirroring
PostScript -- z53.c's embedded interpreter runs the PS body) to
stop firing the warning.  Fixing them is out of scope for the
include/-only audit; they are tracked by the doc-rendering work
stream.

## Verification

- `bash tests/run_all.sh` baseline: 57 PASS-EXCELLENT, 0 FAIL.
- `bash tests/run_all.sh` after the initial audit: identical -- the
  single fix was a comment-only change in svgmacros.
- `bash tests/run_all.sh` after the 2026-05-22 follow-up
  (LoutPageDict/Set/MargSet/MargShift + matr stand-ins in z53.c;
  HTML-escape in mdlout.py's ABC/Mermaid emitters; hand-author
  gotcha docs in best_practices.md + svgmacros header): 65 PASS-
  EXCELLENT, 0 FAIL (the snippet corpus grew from 57 to 65 between
  the two runs; new entries cover hashed text, the AFM kerning
  path, the mermaid passthrough, and the per-page reset asserts).
- `bash tests/run_all.sh` after the 2026-05-23 re-confirmation
  pass: 87 PASS-EXCELLENT, 0 FAIL.  This pass touched no
  `lout/include/*` source file because the rescan found no
  remaining PS-without-SVG-and-without-else gap in include/.  The
  surviving SVG-case warnings live in `lout/doc/{design,expert}/*`
  and are tracked by the doc-rendering work stream, not by this
  audit.  Design-build warning count before this pass: 96; after:
  96 (unchanged -- no include/ change can affect them).  Expert
  build warning count before: 235; after: 235 (same reason).
