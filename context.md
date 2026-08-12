# Custom Allocator (`my-malloc`) — Session 2 Context

Handoff notes from a mentoring/debugging session focused on **top-chunk
shrink-to-OS logic** and **`free()` correctness**. Continues directly from
the prior session's context (bugs 1–5, `debug.c` wiring, testing strategy).

Corrected file from this session: `/mnt/user-data/outputs/my-malloc.c`
(passes the full regression suite described below — **this is the version
to commit**, not any file re-uploaded mid-session, which repeatedly reverted
to a pre-fix state).

---

## Design decision made this session

Compared **fixed shrink threshold** (dlmalloc-style, static) vs **dynamic/
adaptive threshold** (glibc-style, ratchet-up-only, ties to `mmap_threshold`
history). Grounded via real source: dlmalloc's `M_TRIM_THRESHOLD` default
(2 MB, static) and glibc's dynamic-threshold patch (ratchets up on large
frees, `trim_threshold = 2 × mmap_threshold`, capped at
`DEFAULT_MMAP_THRESHOLD_MAX`).

**Decision: keep fixed threshold.** User reasoned through the tradeoff
independently — a ratchet-up-only dynamic threshold would over-retain memory
after a setup-phase burst allocation followed by small-object churn, which
matches this project's likely workload shape. Correct call, not revisited.

---

## Bugs found and fixed this session

All fixes verified with a real test suite (see below), not just code review.

| # | Location | Description | Status |
|---|---|---|---|
| **6** | `my_malloc()` top-carve, growth check | `if (request_size >= gm.topsize)` compared raw payload against `topsize`, ignoring that the actual footprint needed is `request_size + ALIGN_HEADER_FOOTER`. When `topsize` sat between those two values, the allocator wrongly skipped `grow_top()`, then `gm.topsize -= needed` underflowed (`size_t`), sending `gm.top_chunk` walking past `heap_end` into unmapped memory. Reproduced live via a sawtooth test (`i=31` in a 50-alloc loop). | ✅ Fixed — `if (request_size + ALIGN_HEADER_FOOTER >= gm.topsize)`. |
| **7** | `insert_large_chunk()` | Confirmed bug 2 from session 1 is a **real crash bug**, not just a silent leak: chunks carved from top never had `list_init()` called on their `->list` member. Combined with the missing fallback insert, an "orphaned" chunk kept `list.next/prev == NULL` (zero-filled fresh `sbrk` pages). The next `free()` that coalesced with it as `prev` called `list_unlink()` on a non-self-linked NULL node → segfault. Reproduced live, traced to exact `free(ptrs[1])` call via `check_heap()`/`check_bins()` bisection. | ✅ Fixed two ways: (a) added fallback tail-insert (`list_add_before(head, ...)`) after the sorted-insert loop in `insert_large_chunk()`; (b) added `list_init(&block->list)` in `my_free()` right after `SET_FREE(block)`, before `coalesce()`, so any freed block is always in a valid detached-or-linked state. |
| **8** | `my_free()`, shrink-check placement | `if (survivor == gm.top_chunk) { unlock; return; }` returned **before** reaching the `SHRINK_THRESHOLD` check. Since freeing memory that coalesces directly into the top chunk is the most common case (ascending-address frees, typical LIFO-ish patterns), shrink almost never fired in practice — this is what looked like "shrink keeps topsize at default" but was actually "shrink rarely runs at all." Verified via `/proc/self/statm`: `heap_end` didn't move after freeing 800 KB in one test run. | ✅ Fixed — restructured so the bin-insert step is skipped when `survivor == top_chunk` (correct, top isn't binned), but the shrink-threshold check always runs afterward regardless of which branch was taken. |
| **9** | `my_free()`, shrink payload recompute | Original: `gm.topsize = new_break - survivor - HEADER_SIZE - FOOTER_SIZE` (bug 3 from session 1, now confirmed as a real defect, not "unfinished code"). Two problems: (a) used `survivor` (whichever chunk triggered the free) instead of `gm.top_chunk`/`gm.topsize`, wrong reference point when `survivor != top_chunk`; (b) subtracted `FOOTER_SIZE`, which reintroduces bug 5's mistake — **the top chunk never has a footer**, so the correct invariant is `(top_chunk+1) + topsize == heap_end`, no `-FOOTER_SIZE`. This bug was invisible to `check_heap()`/`check_bins()` since neither validates the top-chunk invariant; only caught once `check_top_chunk()` was called after a shrink event. | ✅ Fixed — `gm.top_chunk->payload = new_break - (uintptr_t)gm.top_chunk - HEADER_SIZE;` then `gm.topsize = gm.top_chunk->payload;`. Always derived from the actual `new_break`, never hardcoded. |
| **10** (rejected variant) | Same shrink block | User proposed `gm.top_chunk->payload = INITIAL_TOP_SIZE; // simply reset?` on the reasoning that shrink "resets to default." Disproved with two tests: (a) even in the all-freed case, the correct value is off from `INITIAL_TOP_SIZE` by `HEADER_SIZE` bytes (units mismatch — `INITIAL_TOP_SIZE` is an address offset, `payload` is a size net of header); (b) with a live allocation present below top, the natural (unclamped) `new_break` sits **above** the floor entirely, so the floor is never touched and the real payload is nowhere near `INITIAL_TOP_SIZE`. Root confusion identified: conflating **floor** (absolute lower bound anchored to `heap_start`, safety net) with **pad** (relative slack anchored to `top_chunk`, the actual shrink target) — they only coincide in the special case `top_chunk == heap_start`. | ❌ Rejected, not applied. Diagrammed for the user to build intuition; user hit a wall mid-explanation and chose to stop here rather than push through confused. **Revisit this floor-vs-pad distinction first thing next session**, probably with a fresh, simpler diagram before touching code again. |

### Naming cleanup (also this session)
- Replaced the shrink floor's reuse of `MMAP_THRESHOLD` (coincidental,
  unrelated constant) with a new, purpose-built `INITIAL_TOP_SIZE` macro in
  `my-malloc.h`, defined as `INITIAL_HEAP_SIZE` (so `heap_start +
  INITIAL_TOP_SIZE` exactly equals the original post-`heap_init()`
  `heap_end`). Good improvement, orthogonal to bug 10 above — the *name* is
  fixed, the *usage* (floor vs pad conflation) is not yet resolved.

---

## Test infrastructure built this session (not yet copied into the project's `test/` dir)

All ad hoc so far, written and run in a scratch dir against copies of the
project's real sources. **Not yet persisted to the actual project** — next
session should decide where these live permanently (likely `test/`).

1. **`test_sawtooth.c`** — perf-oriented: allocs/frees a batch of same-size
   chunks in a loop, tracks `g_sbrk_calls` (existing counter) and peak RSS
   via `getrusage()`. Revealed that same-size batch frees fully coalesce
   into one reusable block, so `sbrk_calls` stays at 1 — this pattern does
   **not** actually exercise the shrink path repeatedly. Useful baseline,
   not sufficient alone.
2. **`test_sawtooth_debug2.c`** — correctness-oriented: same shape, but
   calls `check_heap()`/`check_bins()` after every single `my_malloc`/
   `my_free` call, built with `-DDEBUG -fsanitize=address`. This is the
   workhorse that caught bugs 6 and 7 — bisected by adding per-call print
   tracing (`i=%d ptr=%p`) until the exact failing call was isolated.
3. **`test_floor.c`** — allocates a big batch, frees it all, checks
   `/proc/self/statm` RSS and `heap_end` before/after, plus
   `check_top_chunk()`. Caught bug 9 (the missing/wrong invariant after
   shrink) — this was the first test in the whole project to call
   `check_top_chunk()` after anything other than `heap_init()`.
4. **`test_reset_claim2/3.c`** — persistent-allocation variants used to
   disprove the "shrink resets to default" claim (bug 10). Also stumbled
   into an **unresolved, non-deterministic crash** (see below) — found by
   accident while building these, not fully diagnosed.

**Key testing lesson reinforced**: `check_heap()` and `check_bins()` do not
validate the top-chunk invariant at all (they stop iterating once they
reach `top_chunk`, and never inspect its own payload against `heap_end`).
Every code path that touches `gm.topsize` or `gm.top_chunk` — `grow_top()`,
top-carve in `my_malloc()`, and the shrink block in `my_free()` — needs
`check_top_chunk()` run against it specifically, not just the general
heap/bin checks.

---

## Open item: unresolved non-deterministic crash (found, not fixed)

While testing bug 10 with a persistent 200 KB allocation kept alive across
a shrink event: a segfault occurred in some builds/runs and not others
(same source, same test, differing only by presence of
`-fsanitize=address` or minor unrelated code changes in the test harness
around it — classic UB fingerprint, not flakiness). Isolated so far:

- The 200 KB "persistent" allocation actually goes through the **mmap
  path** (`>= MMAP_THRESHOLD` = 128 KB), not the sbrk heap, which
  invalidated the original test design (intended to keep a live block
  *inside* the sbrk heap, below `top_chunk`).
- Crash trace pointed at `my_free(persistent)` (the `munmap()` call) or
  immediately around it — not yet confirmed which.
- ASan builds did **not** reproduce it; plain `-O0 -DDEBUG` builds did,
  consistently across multiple runs. This suggests something ASan's
  instrumentation masks or shifts (stack layout, possibly), rather than
  true nondeterminism — worth a `gdb` session or `-fsanitize=undefined` run
  next time, not more ad hoc printf bisection.

**Next session: dedicate focused time to this before anything else touches
`my_free()` again** — don't layer more changes on top of a codebase with a
known-but-uncharacterized crash.

---

## Current status (end of session 2)

- `my-malloc.c` in `/mnt/user-data/outputs/` has bugs 6, 7, 8, 9 fixed and
  verified (sawtooth regression × 500 rounds with ASan, `check_top_chunk`
  passing after shrink). **This is the version to commit.**
- Bug 2/7's fallback-insert fix means `insert_large_chunk()` is now
  believed correct, but has not been stress-tested with a workload that
  actually populates a large bin with 2+ entries and forces sort-position
  insertion in the middle (all repro tests so far only exercised the
  empty-bin fallback path).
- The floor-vs-pad conceptual conflation (bug 10 area) is understood by
  Claude and diagrammed, but **not yet internalized by the user** — flagged
  explicitly as unfinished, not swept under the rug.
- Test files exist only in a scratch environment this session, not
  committed to the project's `test/` directory.

## Open items / next steps

1. **Resume the floor-vs-pad distinction** from a clean state next session
   — the diagram exists in this conversation's history if needed, but
   probably better to re-derive it fresh once rested.
2. Diagnose the non-deterministic crash (persistent mmap'd block + shrink)
   properly — `gdb`, or `-fsanitize=undefined`, not printf bisection.
3. Decide on a permanent home for the test files built this session
   (`test_sawtooth.c`, `test_sawtooth_debug2.c`, `test_floor.c`) — likely
   `test/`, per the project's existing `src/`/`include/`/`test/` layout.
4. Stress-test `insert_large_chunk()`'s sorted-insert-in-the-middle path
   specifically (not just the empty-bin fallback).
5. Everything from session 1 that was still open remains open: test
   isolation strategy (`heap_reset_for_test()` vs one-process-per-test),
   and Bug 2's original repro sketch (now largely superseded by the
   sawtooth/debug tests built this session).

---

## Suggested commit for today

```
fix: correct top-chunk growth check, orphaned-chunk crash, and shrink logic

- my_malloc(): compare full chunk footprint (payload + header/footer),
  not raw payload, against topsize before deciding to grow_top().
  Prevents unsigned underflow that walked top_chunk past heap_end.
- insert_large_chunk(): add missing fallback tail-insert when a freed
  chunk is >= every existing entry in its bin (was previously silently
  dropped, orphaning the chunk).
- my_free(): list_init() a block's list node before coalesce/insert,
  so a subsequent coalesce() against it as `prev` doesn't dereference
  an uninitialized (non-self-linked) node.
- my_free(): shrink-threshold check no longer skipped when the freed
  chunk merges directly into top_chunk (previously an early return
  bypassed shrink entirely in the most common coalescing case).
- my_free(): shrink recompute now derives topsize from the actual
  new_break and top_chunk address instead of a stale/wrong reference
  chunk, and no longer over-subtracts FOOTER_SIZE (top chunk has no
  footer).
- my-malloc.h: rename shrink floor constant from a reused MMAP_THRESHOLD
  to a purpose-built INITIAL_TOP_SIZE.

Verified: 500-round sawtooth regression with check_heap()/check_bins()
after every op under ASan; check_top_chunk() passes after shrink events
at and away from the floor.

Known open issue (not in this commit): non-deterministic crash under a
persistent mmap'd allocation + shrink combo, not yet root-caused.
```