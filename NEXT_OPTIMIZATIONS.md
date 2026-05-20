# Next SVG Back-End Optimizations

Read-only survey closing 2026-05-20.  After commit `24d76b4` ("SVG perf:
open-address hash for dict lookup -- 2.3x faster") the headline 24% CPU
hotspot in `svg_dict_stack_lookup` is gone.  This file lists the next 3-5
quick-wins, ranked by impact-per-effort.  Out-of-scope items called out
elsewhere (font-metric matching at the rasteriser, UTF-8 input, OpenType
loader, PDF colour completion) are skipped.

Each entry: description / impact / complexity / observable / files touched.

---

## 1. Hoist the 20-strcmp `filledsquare/...` guard out of the NAME hot path

Description: `svg_ps_exec_value` (z53.c:4198-4222) runs an unconditional
linear chain of 20 `strcmp` calls against `v->name` before consulting the
dictionary, just to intercept the @Graph plot-symbol shortcut ops.  Every
single PS-NAME execution -- and there are hundreds per `@Graphic` body --
pays this tax even when the name is `moveto` or `xcurr`.  Move the 20
names into a tiny static perfect-hash (or a single `switch` on first char
+ short strcmp) so the common path is one cheap test.

Impact: performance.  SVG_PERFORMANCE.md item #4 estimates 1-2% wall-time,
but with #1 (the dict-hash) already landed, this guard is now the single
densest cluster of strcmps on the per-name path and the relative cost has
risen.  Realistic expectation: 2-4%.

Complexity: small (<50 LOC; a 20-entry static array and one lookup loop,
or a one-character-dispatch `switch`).

Observable: build the user guide, `time ../../lout -r3 -G all -o user.svg`,
compare to current 2m10s baseline; profile with `gprof` and verify
`svg_ps_exec_value` no longer shows in the top 10.  Grep
`grep -c "strcmp(v->name," lout/z53.c` -- should drop from 20 to 0.

Touches: just `z53.c`.

Rank: #1 (top dispatch).  Lowest-risk, smallest, directly attacks the
remaining hot path identified in SVG_PERFORMANCE.md section 3.2.

---

## 2. Set block-buffered output on `out_fp` (setvbuf 64-256 KB)

Description: z53.c never calls `setvbuf` on `out_fp`.  When lout is
invoked with `-o user.svg`, glibc picks the default 4 KB block buffer for
regular files; on the user-guide build that means ~2700 buffer flushes
for the 10.4 MB output.  Bumping to 64-256 KB cuts kernel-mode syscall
overhead.  Compounds with item 6 in SVG_PERFORMANCE.md (path emission via
many small `fputs`/`fprintf` calls).

Impact: performance, and slightly more so on WSL2 where syscall overhead
is elevated (gprof's 3.78% in glibc `_init`/stdio is partly this).
Expect 2-4% wall-time reduction; little to no user-CPU change.

Complexity: small (<10 LOC -- one `setvbuf(out_fp, NULL, _IOFBF, 1<<17)`
call inside `SVG_PrintInitialize`, plus matching `fflush` at
`SVG_PrintAfterLastPage` for safety).

Observable: `strace -c -e write ./lout -r3 -G doc/user/all -o user.svg`
write count drops by ~2 orders of magnitude.  Wall-time delta visible on
`time` runs of the user-guide build, especially when stdout is piped.

Touches: just `z53.c`.

Rank: #2.  Trivial change; clear measurable win that also makes
benchmarking the *other* optimizations less noisy.

---

## 3. Cache parsed token streams per `@Graphic` body pointer

Description: `svg_ps_run` (z53.c:4284) re-tokenises and re-parses the
entire raw `@Graphic` buffer character-by-character on every single call.
The user guide invokes ~640 graphic bodies x 3 passes (cross-ref
resolution), and many are the *exact same prologue* (`graphf.lpg`,
`diagf.lpg`, `figf.lpg`, `coltex` snippets) emitted verbatim each time.
Memoize: keep a small fixed-size (e.g. 32-slot LRU) cache keyed by the
WORD object identity or by an FNV hash of `(buf, n)`, storing the parsed
`svg_value *vals` array.  On hit, skip straight to the execute loop at
z53.c:4299-4307.

Impact: performance.  SVG_PERFORMANCE.md item #5 estimates 3-5%.  Slightly
under-counts because at `-O3` both `svg_ps_tokenise` and
`svg_parse_tokens` fold into their callers, so gprof understates them.

Complexity: medium (50-100 LOC).  The wrinkle is that the parsed `vals`
array refers into `s->`-owned storage in a few places (string operand
backing); careful audit needed to ensure cached entries are immutable
across runs, or to copy mutable bits out on the way in.  Otherwise pure
addition next to `svg_ps_run`.

Observable: instrument `svg_ps_run` with a one-line `static long
tok_hit, tok_miss;` increment and dump at `SVG_PrintAfterLastPage`; on
the user guide expect hits >> misses (most bodies are prologue boilerplate).
Wall-time check: 1m 2s -> ~58s on the `-r3 user.svg` build.

Touches: just `z53.c`.

Rank: #3.  Mid-impact, mid-risk.  Worth doing once the lower-risk items
have landed and verified.

---

## 4. Replace the 150-case `svg_ps_exec_op` strcmp chain with a hash table

Description: `svg_ps_exec_op` (z53.c:2622-~4145) is one linear chain of
~150 `if (strcmp(name, "<op>")==0)` cases.  For a name like `pop` (the
last few cases) the dispatch costs ~150 string comparisons.  Reachable
for every PS built-in invocation that misses the dict (i.e., almost all
of them after recent prologue-shadowing patches).  Replace the chain
with an open-addressed hash table of `(name, fn_ptr)` -- the same
approach now used inside `svg_dict_lookup`, but for built-ins.

Impact: performance.  SVG_PERFORMANCE.md item #3 estimates 2-4%.  Layers
multiplicatively with item 1 above: once #1 hoists the NAME-path guard,
this chain is the next dominant strcmp cluster on every operator call.

Complexity: medium (100-200 LOC).  Touch points: declare a static struct
table of ~150 entries, write a small dispatch helper, refactor the chain
into either function-pointer callees or a `switch (op_id)` block.  Lowest-
friction refactor is to assign integer ids during the hash lookup and
keep one giant `switch` -- minimal logic churn, just dispatch speedup.

Observable: gprof flat profile -- `svg_ps_exec_op`'s share drops from
its current sub-percent baseline (most current cost is fold-in from
inlined callers) but the *whole-program* `strcmp` time shrinks.  Easier
check: `perf stat -e instructions ./lout -r3 -G ...` should drop ~3%.

Touches: just `z53.c`.

Rank: #4.  Larger refactor, but mechanically straightforward.  Sequence
after #1 because the latter sets up the pattern (perfect/open-addressed
hash dispatch on operator names) cleanly.

---

## 5. Fix `xdecr`/`ydecr` boolean lookup in @Graph C-shortcut

Description: SVG_PORTING.md section 10 ("@Graph plot -- symbol/curve
alignment") flags that the C-side `trpoint` shortcut at z53.c:3138-3142
looks up `xdecr`/`ydecr` via `vv.kind == SVG_VK_NUM`, but graphf.lpg
defines those via `/xdecr exch def` from a *boolean* argument, so the
lookup silently fails and the shortcut always treats the axis as
ascending.  Harmless for ascending data, visible misalignment on
descending axes.  One-line fix: also accept `SVG_VK_BOOL` and convert.

Impact: correctness.  Resolves a known-but-deferred bug noted in
SVG_PORTING.md.  No performance change.

Complexity: small (<20 LOC including the BOOL->NUM coercion helper).

Observable: build a 4-line `@Graph` test doc with a descending x-axis
(e.g. `xdecr { yes }`) plotting a `square` symbol curve.  Before: symbols
are mirrored relative to the curve.  After: symbols sit on the curve.
The author of SVG_PORTING.md flagged this as a "one-line patch worth
doing on principle even if it isn't the dominant offender" for the larger
@Graph misalignment.

Touches: just `z53.c`.

Rank: #5.  Lowest performance impact (none) but a documented correctness
gap with a trivial fix.  Schedule when next touching @Graph code.

---

## Summary table

| #  | Change                                       | Complexity | Impact category | Est. wall-time |
|----|----------------------------------------------|------------|-----------------|----------------|
| 1  | Hoist `filledsquare/...` guard               | small      | performance     | 2-4%           |
| 2  | `setvbuf` on out_fp                          | small      | performance     | 2-4%           |
| 3  | Cache parsed token streams per @Graphic body | medium     | performance     | 3-5%           |
| 4  | Hash-dispatch `svg_ps_exec_op`               | medium     | performance     | 2-4%           |
| 5  | Accept BOOL for xdecr/ydecr in @Graph        | small      | correctness     | n/a            |

Combined #1-4 should bring SVG-mode wall-time within ~10% of PS mode on
the user guide build, completing the trajectory begun by `24d76b4`.

## Not included (out of scope)

- Font-metric matching at the rasteriser (TODO.md "Partial / lower
  priority" -- mitigation already landed via embedded URW++ Nimbus base-35
  data URLs).
- Per-glyph absolute positioning via `<tspan>` arrays (deferred until a
  `--precise-text` flag is requested).
- Texture-procedure body interpretation (already gracefully degrades to
  named-texture identification; no user-reported pain).
- C Lout UTF-8 input layer, OpenType loader (TODO section 4).
