# Custom Allocator (`my-malloc`) — Session 5 Context

Continues directly from Session 4. Session 4 ended with `insert_large_chunk()`
found broken (silently dropped chunks on empty-bin and tail-insert paths),
fix direction agreed but unapplied, and bin-climbing validated against real
source but unwritten. **This session, the user applied the
`insert_large_chunk()` fix themselves** (uploaded a new `my-malloc.c` with
it already in place) — Claude's role this session was verification,
measurement, and research, not writing the fix. No other code changes were
applied to `my-malloc.c` this session.

---

## `insert_large_chunk()` fix — user-applied, verified correct

User's fix: explicit `list_is_empty(head)` branch for the empty-bin case
(`list_add_after(head, ...)`, return), and for the non-empty case, a loop
that walks `curr` forward while `next_block->payload >= chunk->payload`,
breaking on the first strictly-smaller neighbor, followed by an
**unconditional** `list_add_after(curr, &chunk->list)` after the loop —
this is the key fix: every exit path from the function now reaches a real
insertion, unlike the old version where falling out of the loop (empty
bin, or chunk is the new smallest) skipped insertion entirely.

Verified empirically, directly against the user's uploaded file (not a
hand-retyped copy):

- `probe_sortbin3.c` (guard-block isolated, so frees can't coalesce into
  neighbors or the top chunk): empty-bin insert → `list_length` goes to
  `1`; a second, larger chunk inserted into a non-empty bin (the case that
  used to silently vanish) → `list_length` goes to `2`. Both previously
  broken paths now work.
- `test_shrink_correctness.c` and `test_shrink_sbrk_fail.c` (from Session
  4): still pass — no regressions from the fix.

**Bug is resolved. No longer an open item.**

---

## Bin ordering — confirmed descending, LRU tie-break (not the originally-intended ascending)

Traced by hand and confirmed with `probe_order.c`: the fixed algorithm
produces bins sorted **descending** by payload (largest chunk at head,
smallest near tail) — the opposite of the old (broken) code's intent,
which was ascending. Mechanism: `curr` starts at `head` (acts as a
"virtual infinity"), advances past any neighbor that's still `>=` the new
chunk, stops at the first strictly-smaller neighbor (or end of list), and
inserts right after `curr`. Bigger new chunks lose the size comparison
immediately at `head->next` and land at the head; smaller ones have to
walk past everything bigger and land near the tail.

Duplicate-size tie-breaking confirmed with `probe_dup.c`: because the
loop condition is strict `<` (not `<=`), a new chunk equal in size to an
existing one is walked *past* (not stopped at), so it always lands
**after** the existing same-size chunk — new chunks of a given size end
up further from the head than older ones of the same size. This matches
dlmalloc's documented "ties go to the least recently used chunk"
convention.

This ordering is not a correctness requirement for the current code
(`find_suitable_block()` doesn't rely on order — see below) but is a
real, deliberate, now-understood invariant worth documenting in a comment
near `insert_large_chunk()`, since the old inline comment ("insert before
to keep sorted list") no longer describes what the code does.

---

## Search performance investigated — sort order exists but isn't exploited yet

User asked directly: does the new order change how `find_suitable_block()`
performs? Answered by measurement, not guesswork — instrumented a scratch
copy (`my-malloc-instrumented.c`) using the **pre-existing but previously
dead** `g_scan_steps` counter (declared in `my-malloc.h`, defined in
`my-malloc.c`, never incremented anywhere in the real code before this).

`probe_scan_cost.c` results, 6-element bin, non-exact-match requests (to
avoid the O(1) head fast-path):

| request | true best-fit location | scan steps |
|---|---|---|
| 2850 | 2912, at the **head** | 6 (full scan) |
| 2450 | 2528, near the **tail** | 5 (near-full scan) |

**Conclusion, stated carefully to avoid overclaiming**: the fix improved
*correctness* (no more lost chunks) and produced a *valid, sortedness
invariant* — but did **not** improve search *performance*. The scan cost
is effectively O(n) regardless of where the answer sits, because
`find_suitable_block()`'s `else` branch does a full `list_for_each_entry`
with no early exit, exactly matching the pre-existing comment already in
that function:

```c
// THIS IS STILL O(N) - can be optimize if use tree O(log n)
...
// TODO: CHANGE AFTER WE CHANGE TO SORTED BINS
```

The sortedness now satisfies that TODO's precondition, but the early-exit
change itself hasn't been written. **User confirmed they anticipated
this while originally writing the scan loop and already left a comment
marking the spot for a future early-exit swap** — consistent with (or
possibly referring to) the TODO above.

---

## Research: what real allocators do, and what it implies for build order

Searched glibc source/analyses specifically to answer "should we build
the early-exit scan optimization or bin-climbing next?" Key finding,
confirmed across multiple independent sources describing `malloc.c`:
**"Binmap searches occur after an unsuccessful unsortedbin or largebin
search."** I.e., in glibc, bin-climbing (via the binmap bitmap) is a
fallback layer wrapped *around* the direct-bin search, not something that
changes how the direct-bin search itself works.

This directly implies bin-climbing and the early-exit scan optimization
are **architecturally independent** in this codebase too: climbing only
needs to decide *which bin index* gets fed into the existing
scan-and-filter loop in `find_suitable_block()`'s `else` branch — the
loop itself doesn't need to change for climbing to work.

**Recommendation given to the user (reasoned, not yet acted on)**:
implement bin-climbing next, ahead of the early-exit optimization,
because:

1. The two features don't block each other (per the glibc structure
   above), so order is a free choice — no reason not to pick the higher-
   impact one first.
2. Bin-climbing fixes a real **behavioral** bug still open since Session
   3: an empty target bin currently makes `find_suitable_block()` return
   `NULL` immediately even when a larger bin has a perfectly usable free
   chunk, causing unnecessary `grow_top()`/`sbrk()` calls and heap bloat.
   This is a resource-usage correctness issue, not just a speed one.
3. The early-exit scan optimization doesn't fix any wrong behavior —
   current results are always correct, just not maximally cheap to
   compute. Lower priority by construction.

**Neither is implemented yet.** Session ended here (user reported being
tired) with the decision made but no code written for either.

---

## Current status (end of session 5)

- `insert_large_chunk()`: **fixed and verified**, by the user, confirmed
  by Claude against the actual uploaded file (not a retyped copy).
- Bin ordering: confirmed descending with LRU-style tie-breaking;
  documented above, not yet reflected in an updated code comment.
- `find_suitable_block()`: confirmed via instrumentation to still be a
  full O(n) scan regardless of bin order — matches its own pre-existing
  TODO comment. Not yet changed.
- Decision made: **bin-climbing before early-exit optimization**,
  grounded in glibc's actual `malloc.c` structure. Neither implemented.
- User has already marked the future early-exit swap spot with their own
  comment while writing the scan loop — good forward-thinking, worth
  preserving/consolidating with the existing TODO when that work starts.
- Scratch probes from this session (`probe_sortbin3.c`, `probe_order.c`,
  `probe_dup.c`, `probe_scan_cost.c`, `my-malloc-instrumented.c`) exist
  only in the sandbox working directory — **not saved to outputs, not
  promoted to real regression tests** (still print-only, no assertions).
- `test_bugs.c` (user's own file, 3 issues from Session 4): still
  untouched this session — `ensure_state`/`state` visibility, `#define
  DEBUG` ordering, suspicious `#include "malloc.h"`, missing `fork()`
  isolation.

## Open items / next steps

1. **Implement bin-climbing** in `find_suitable_block()` — linear scan
   `idx..NUM_BINS-1`, reuse the existing scan-and-filter loop verbatim
   for whichever bin is landed on. Decided priority for next session.
2. **Wire `g_scan_steps` into the real `find_suitable_block()`** (not
   just a throwaway instrumented copy) so climbing's and the eventual
   early-exit's before/after cost can be measured for real and tracked
   over time.
3. **Early-exit in the best-fit scan** — deferred until after
   bin-climbing; spot already marked by the user's own comment plus the
   pre-existing TODO in the code.
4. **Promote this session's probes into real regression tests** —
   `probe_sortbin3.c` / `probe_order.c` / `probe_dup.c` /
   `probe_scan_cost.c` are all print-only right now; should gain
   assertions (e.g. `assert(list_length(...) == expected)`,
   `assert(g_scan_steps <= expected_bound)` once early-exit lands).
5. **Fix `test_bugs.c`** (carried over from Session 4, still untouched):
   reorder `#define DEBUG` before `#include "debug.h"` (or drop it in
   favor of the `-DDEBUG` build flag); replace `ensure_state()`/`state`
   with `debug_get_state()`; resolve the `#include "malloc.h"` line;
   adopt the `fork()`-based test isolation pattern.
6. Carried over from Session 2/3, still untouched:
   - Diagnose the non-deterministic crash (persistent mmap'd allocation +
     shrink combo) — `gdb` or `-fsanitize=undefined`, not printf
     bisection.
   - Decide a permanent home for scratch/test files (now includes this
     session's probes too) — likely `test/`.
7. Once bin-climbing and the early-exit optimization are both in and
   tested, produce a corrected `my-malloc.c` in `/mnt/user-data/outputs/`
   — none generated this session (verification/research only).