# SVG Back-End Performance Analysis

Read-only audit of `z53.c` (SVG back end). Goal: explain why `lout -G` is
~2-3x slower than the PostScript back end, identify the hottest paths, and
rank concrete optimizations by expected payoff.

## 1. Wall-clock measurements

User guide build (`doc/user/all`), `O3` build, this WSL2 box:

| Mode | Flags                         | real    | user    | sys    |
|------|-------------------------------|---------|---------|--------|
| PS   | `lout -r3 all -o user.ps`     | 1m 16s  | 0m 46s  | 0m 2s  |
| SVG  | `lout -r3 -G all -o user.svg` | 2m 10s  | 1m 05s  | 0m 3s  |

### 1.1 Cumulative speedup tracking (single-pass `lout -G all`)

Stable, warm-cache, single-pass timings on the user-guide build
(`cd doc/user && rm -f *.li && time ../../lout -I ../../include -I . -G all
> /tmp/user.svg`).  Each row is layered on top of the previous:

| Stage                                           | real    | user    | sys    | delta vs base |
|-------------------------------------------------|---------|---------|--------|---------------|
| baseline (`24d76b4`, dict-hash already shipped) | 37.6 s  | 31.9 s  | 0.42 s | -             |
| + #1 hoist `filledsquare/...` strcmp guard      | 36.2 s  | 30.8 s  | 0.41 s | real -3.7%    |
| + #2 `setvbuf(out_fp, 128 KiB)` + flush         | 36.5 s  | 30.8 s  | 0.41 s | real -2.9%    |
| + #3 memoize parsed @Graphic token streams      | 35.6 s  | 30.2 s  | 0.34 s | real -5.3%    |

Cumulative wall-time delta over the dict-hash baseline: **real -5.3%**,
**user -5.3%**, **sys -19%**.  Below the 25-40% goal -- once
`svg_dict_stack_lookup` was hashed (commit `24d76b4`), the remaining
strcmp clusters and stdio costs are individually small.  Item #4 (hash
dispatch for `svg_ps_exec_op`'s 150-case chain) is the largest remaining
opportunity but requires a much heavier refactor; skipped this round
(see "Item 4 status" below).

### 1.2 Item 4 status (deferred)

The 150-case strcmp chain inside `svg_ps_exec_op` (z53.c ~2645-4150) is
the largest single remaining strcmp cluster.  A safe rewrite to a hashed
dispatch table (FNV-1a + linear probing, like `svg_dict_lookup`) would
need either (a) a per-op `enum op_id` + 150-case `switch`, or (b) a
shared `name_hash` precompute at the function top plus a length-gated
`MATCH(s, h_s)` macro at every site -- both 150+ line mechanical edits.
Deferred to a future session.  Expected residual gain: 2-4% wall.


SVG is **~1.7x slower wall, ~1.4x slower user** on `-r3`. The 3x figure on
the full 7-pass build is consistent with this once xref churn is included:
SVG output (`10.4 MB`) is also ~10x larger than PS, so I/O matters at the
upper passes. There are ~640 `@Graphic`/`@Diag`/`@LDiag`/`@LFig` invocations
across the user guide, each driving one full `svg_ps_run`.

## 2. Profiler results (gprof, `-O2 -pg -no-pie`, `-r2 -G`)

73.78s of profiled samples. Flat profile, top entries:

```
 23.99%  17.70s   svg_dict_stack_lookup        <-- SVG
 23.65%  17.45s   CopyObject                       (general Lout)
  8.70%   6.42s   DisposeObject                    (general Lout)
  6.79%   5.01s   Manifest                         (general Lout)
  3.98%   2.94s   SVG_LinkURL                  <-- SVG
  3.62%   2.67s   Constrained                      (general Lout)
  3.09%   2.28s   SVG_PrintBetweenPages        <-- SVG
  2.94%   2.17s   SearchEnv                        (general Lout)
  2.52%   1.86s   SearchSym                        (general Lout)
  1.34%   0.99s   SVG_LinkDest                 <-- SVG
  0.43%   0.32s   svg_ps_run                   <-- SVG
  0.27%   0.20s   svg_graphic_concat           <-- SVG
  0.20%   0.15s   SVG_CoordRotate              <-- SVG
  0.15%   0.11s   svg_ps_call / svg_ps_exec_proc
  0.14%   0.10s   SVG_PrintWord
  0.14%   0.10s   svg_ps_pop
```

SVG-attributable total: ~35% of CPU. The biggest single offender by a wide
margin is `svg_dict_stack_lookup` at **24%** of profiled CPU time.

Note: many other SVG statics (`svg_ps_exec_value`, `svg_ps_exec_op`,
`svg_ps_tokenise`, `svg_dict_lookup`) are heavily inlined at `-O3` and fold
into the caller they end up in. gprof can't see them. Static analysis shows
they live on the same hot path as `svg_dict_stack_lookup`.

## 3. Hot-path static analysis

### 3.1 `svg_dict_stack_lookup` (24% of CPU)

```c
static int svg_dict_stack_lookup(const char *name, svg_value *out) {
  for (i = svg_dict_top; i >= 0; i--)
    if (svg_dict_lookup(svg_dict_stack[i], name, out)) return 1;
  return 0;
}
static int svg_dict_lookup(int did, const char *name, svg_value *out) {
  for (i = 0; i < SVG_PS_DICT_ENTRIES; i++)        /* 256 */
    if (d->entries[i].used && d->entries[i].name &&
        strcmp(d->entries[i].name, name) == 0) { ... }
}
```

Worst case per name reference: 32 dicts on the stack * 256 entries *
strcmp = **8192 strcmps per name lookup**. Even with the prologue dict
sparsely populated, scanning all 256 slots is unconditional. Every PS name
in every `@Graphic` body triggers this; for typical `ldiagsetarc` /
`graphf.lpg` code there are dozens of name lookups per shape.

### 3.2 `svg_ps_exec_op` strcmp chain (z53.c ~2550-4070)

A linear `if (strcmp(name, "<op>")==0)` chain with **~150 cases** for every
operator. Reached only when dict lookup fails (i.e. for built-ins) plus
20 hardcoded names that are pre-checked in `svg_ps_exec_value`
(`filledsquare`, `dofilledsquare`, ...) -- each of those costs 20 strcmps
on every name dereference, even when the name is something else entirely.
That guard alone is a measurable tax on the dict-lookup-miss path.

### 3.3 `svg_ps_tokenise` (re-run on every `svg_ps_run`)

`svg_ps_run` re-tokenises the entire `@Graphic` buffer character-by-character
on every invocation (line 4211), then `svg_parse_tokens` rebuilds the typed
value array. The same prologue procedures (defined in `graphf.lpg` etc.)
are tokenised every time they appear, even though their token streams never
change. SVG_PS_MAX_TOKENS is 65536 -- arrays this size live on the stack
frame of `svg_ps_run` and incur a stack reset each call.

### 3.4 `svg_dict_gc_sweep` (per-`svg_ps_run` mark-and-sweep)

```c
svg_dict_gc_sweep(s);    /* runs at the end of every svg_ps_run */
```

Walks all 1024 dict slots * 256 entries on every `@Graphic` block. With
~640 graphic invocations across the user guide and 7 passes, that's ~4.5
million slot-scans. Most of them mark nothing (the anonymous-dict pattern
is rare). This is bounded by gprof <0.4% as part of svg_ps_run -- minor.

### 3.5 Path emission: `snprintf` + `fputc/fputs` (z53.c 1681-1799)

Each `moveto`/`lineto`/`curveto` does `sprintf(buf, "L %.3f %.3f ", ...)`,
then `svg_ps_path_append` copies into an `s->path` buffer with strlen() +
memcpy. The final `svg_ps_emit_path` writes via `fputs`/`fprintf` to
`out_fp`. Not currently a major hotspot (gprof shows `svg_ps_lineto` at
0.03%), but `_init`/glibc stdio appears at 3.78%, much of which is fprintf
formatting. The 10 MB SVG output goes through unbuffered-ish fprintf calls.

### 3.6 Link emission

`SVG_LinkURL` (4%) + `SVG_LinkDest` (1.3%) + `SVG_PrintBetweenPages` (3.1%)
together account for **~8% of CPU**. These are called per cross-reference
target/source in `FixAndPrintObject`. The user guide is rich with internal
links; each link emits an `<a id=...><rect .../></a>` block.

## 4. Optimization candidates (ranked)

| # | Change                                                          | Complexity | Expected speedup | Justification |
|---|-----------------------------------------------------------------|------------|------------------|---------------|
| 1 | **Hash the dict (replace linear strcmp scan in `svg_dict_lookup`)** | small      | **~20% total**   | Direct attack on 24% hotspot. Open-addressed hash keyed by FNV/DJB2; 256-entry table; lookup drops from O(N) to ~O(1). Single function change. |
| 2 | **Intern names; dict keys use interned pointer compare**         | medium     | additional 3-5%  | Strdup names through a hash-interner once at parse time; lookups become pointer == pointer. Removes the strcmp inside #1's hashed lookup, plus speeds the strcmp chain in `svg_ps_exec_op` once converted to pointer dispatch. |
| 3 | **Hash-table dispatch in `svg_ps_exec_op`**                      | medium     | 2-4%             | 150-case linear strcmp chain reachable for every built-in operator. perfect-hash (gperf) on operator names, dispatch via small table of function pointers. |
| 4 | **Hoist the 20-strcmp `filledsquare/...` guard** in `svg_ps_exec_value` | small | 1-2%      | Today every NAME lookup pays 20 strcmps before consulting the dict. Move those names into a small static hash, or check after the dict lookup so the common path is unaffected. |
| 5 | **Cache parsed token streams per `@Graphic` body**               | medium     | 3-5%             | `svg_ps_run` re-tokenises and re-parses identical prologue procedures on every call. Memoize by source-buffer pointer or by a hash of the buffer. Saves `svg_ps_tokenise` + `svg_parse_tokens` work each time. (Less impactful than #1 because both stages are already cheap relative to dict lookup.) |
| 6 | **Buffered `fwrite` for path emission**                          | small      | 1-3%             | Replace the many `fputs("M ", ...) ; fprintf("%.3f", ...)` calls with a single `fwrite` of a pre-formatted line buffer. Also use `snprintf` once per line, not three times. Helps wall time more than user CPU because of fewer syscalls. |
| 7 | **Reduce `svg_dict_gc_sweep` frequency**                         | small      | <1%              | Run sweep every Nth call, or only when `svg_dict_pool_used` is close to the pool size. Marginal; current cost is below 0.4%. |
| 8 | **Batch `<a>` rect emission / dedupe consecutive link wrappers** | small      | 1-2%             | Most page emits dozens of zero-area `<a>` markers for cross-refs. fputs is fine; the fprintf formatting is the cost. Inline the small fixed-format string. |

### Likely **not** worth doing

- Increasing dict pool size: already at 1024, not the bottleneck.
- Switching arena allocator: heap pressure is small; `Manifest` /
  `CopyObject` (general Lout) dominate non-SVG memory churn.
- Reworking texture-proc inspection (`svg_tex_*`): negligible in profile.

## 5. Recommendation

**Do #1 first.** It is the smallest, lowest-risk change with the largest
projected speedup. Replacing the linear scan in `svg_dict_lookup` with an
open-addressed hash table (separate index per `svg_dict` slot, sized like
the existing 256 entries) should claw back the bulk of the 24% that
`svg_dict_stack_lookup` is currently spending on string comparisons.

Re-profile after #1; if the hot spot is still in name resolution, do #2
(string interning) which then makes #3 (operator dispatch) and #4 (proc-name
guard) essentially free to layer on top.

Expected combined effect of #1-4: SVG mode running close to PS-mode user
CPU on the user guide build (~20-25% total wall-time reduction).

## Appendix: how to reproduce

```bash
cd lout
make clean
CFLAGS="-std=c99 -Wall -O2 -pg -no-pie" LDFLAGS="-pg -no-pie" make lout
# manually relink as non-PIE so gmon.out gets emitted:
rm -f lout && gcc -pg -no-pie -o lout *.o -lm

cd doc/user
rm -f -- *.ld *.ldx *.li user.svg gmon.out
time ../../lout -r2 -G all -o user.svg 2>/dev/null
gprof -b ../../lout gmon.out | head -80
```

Profiled with gcc-13's gprof on Linux 5.15 (WSL2). `-pg` instrumentation
adds ~30% wall-time overhead; profile percentages are otherwise
representative.
