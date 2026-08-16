# Custom Allocator (`my-malloc`) — Session 7 Context

Continues directly from Session 6. Session 6 ended with `find_suitable_block()`
fully done (bin-climbing + early-exit, both verified and measured), two new
regression tests passing, and the non-deterministic mmap/shrink crash (open
item since Session 2/3) still undiagnosed with no test written. **This session
found, fixed, and regression-tested that crash.**

---

## Non-deterministic crash — diagnosed, fixed, and regression-tested

**Root cause found:** in `my_malloc()`'s top-chunk bump path (`my-malloc.c`,
around line 299-304), `gm.topchunkptr` was bumped forward and written to
(`gm.topchunkptr->payload = ...`) without first checking whether the new
position would land past `gm.heap_end`. Under enough random alloc/free
pressure, `gm.topsize` could go too small to satisfy a request without the
existing checks catching it, so the bump walked `topchunkptr` off the end of
mapped memory — the write there is what ASan caught as a SEGV.

- **Test written**: `test_non_deterministic_crash()` in `test_bugs.c`.
  Loops 1000 allocations per run (mix of a large mmap-triggering size every
  10th iteration, plus random small sizes via `rand()`), with random frees
  mixed in (not LIFO) to exercise coalesce/shrink paths. Runs 20 times with
  varied seeds (`time(NULL) + run`) since the bug is non-deterministic and a
  single seed isn't reliable enough to catch it.
- User found the bug themselves by reading the ASan trace (`SEGV ... WRITE
  memory access ... my-malloc.c:302`) and correctly reasoning from the
  surrounding code (`topsize -=`, `topchunkptr = BLOCK_NEXT_HEADER(...)`,
  then writes to the new `topchunkptr`) that no bounds check against
  `heap_end` existed before the write.
- Fix applied by user directly in their environment; confirmed via 15
  consecutive clean runs of `make bug` after the fix (previously crashed
  reliably).
- **Invariant now asserted in the test**, not just "didn't crash": added
  `assert((uint8_t *)gm.topchunkptr <= (uint8_t *)gm.heap_end)` inside the
  allocation loop, so future regressions fail loudly with a line number
  instead of relying on ASan/SEGV to notice.

**This closes Session 2/3's open item #3 (non-deterministic mmap/shrink
crash).**

## Flagged for follow-up: possible second instance of same bug pattern

During the audit (`grep -n "topchunkptr" src/my-malloc.c`), a structurally
similar top-chunk bump appears again in the realloc path around
`my-malloc.c:368-376` (`gm.topsize -= needed; gm.topchunkptr = np;
gm.topchunkptr->payload = gm.topsize; ...`) — **not yet confirmed whether
this spot has the same missing-bounds-check gap**, since it wasn't the one
that crashed this session. Worth a targeted check before considering the
top-chunk-bump pattern fully audited.

---

## Current status (end of session 7)

- Non-deterministic mmap/shrink crash: **diagnosed and fixed**. Root cause
  was an unchecked `topchunkptr` bump past `heap_end` in `my_malloc()`'s
  top-chunk path (~line 299-304).
- New regression test `test_non_deterministic_crash()` in `test_bugs.c`:
  1000-alloc randomized loop × 20 seeded runs, with an `assert()` on the
  `topchunkptr <= heap_end` invariant. Passing after the fix (was reliably
  crashing before).
- Realloc path (~line 368-376) has a similar-looking top-chunk bump —
  flagged, not yet audited for the same bug.
- No other changes made this session; `find_suitable_block()` work from
  Session 6 untouched and still verified working.

## Open items / next steps

1. **Audit realloc's top-chunk bump (~line 368-376)** for the same
   missing-bounds-check pattern found in `my_malloc()` this session.
2. **`ensure_state()`/`state` visibility fix in `debug.c`** — carried over
   from Session 4, still untouched. Also carried: whether `state` should be
   exposed differently, and the `fork()`-based test isolation pattern
   mentioned in Session 5 notes.
3. **`test_bugs.c` cleanup** — 1 of 3 known issues fixed (Session 6:
   `malloc.h` → `my-malloc.h`). Remaining:
   - `ensure_state()`/`state` visibility in `debug.c` (see #2 above).
   - One harmless leftover warning (redefined `DEBUG`) needs a one-line fix.
4. **Session 5's general probes** (`probe_sortbin3.c`, `probe_order.c`,
   `probe_dup.c`, `probe_scan_cost.c`, `my-malloc-instrumented.c`) still
   exist only in sandbox scratch space, not ported/deleted — superseded in
   spirit by Session 6's two tests plus this session's crash test, but not
   formally cleaned up.
5. **Decide a permanent home for scratch/test files** — carried over,
   still open (likely `test/`, per Session 5 notes).
6. Once the realloc audit (#1) and remaining `test_bugs.c`/`debug.c` items
   (#2, #3) are resolved, produce a corrected `my-malloc.c` + `test_bugs.c`
   pair in `/mnt/user-data/outputs/` — none generated this session (fix and
   test were written directly by the user in their own environment via
   `gcc`/`make`, not authored/output by Claude).