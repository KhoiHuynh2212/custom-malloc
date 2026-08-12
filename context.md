# Custom Allocator (`my-malloc`) — Session Context

Handoff notes from a mentoring/code-review session on a custom `malloc`/`free`
implementation. Architecture: **array-based bins with sorted linked lists**
(no tree — an intentional, fixed constraint of this project), loosely modeled
on dlmalloc's design but adapted throughout since the tree-bin approach isn't
available here.

Reference implementation studied alongside this project:
`https://github.com/KhoiHuynh2212/dlmalloc/` (a modularized fork of Doug Lea's
dlmalloc — used for grounding design comparisons, not copied directly).

---

## Architecture summary

- `bin_map_t`-style bitmap concept discussed (dlmalloc reference), but **not
  needed** in this project — bins here use `list_is_empty()` for O(1)
  "is this bin empty" checks, so no bitmap layer was added.
- Small bins: fixed-size buckets, O(1) insert (push front) and O(1) delete
  (splice via existing `fd`/`bk`, no search).
- Large bins: sorted linked list per bucket (`get_bin_bucket()` ranges).
  Insert is O(n) to find sort position; delete is still O(1) once you already
  have the chunk pointer (no search needed — this was a point of confusion
  earlier in the session, since it's a common misconception that delete is
  also O(n)).
- `top_chunk` / `topsize`: must always satisfy the invariant
  `top_chunk->payload == topsize`. Several bugs stemmed from this invariant
  being silently violated.
- `MIN_CHUNK_SIZE` in this project's header struct = 32 bytes, which is why
  `bins[0]` (and possibly `bins[1]`, depending on whether `small_index()` is
  fed payload-only or total chunk size) is structurally unreachable — this is
  expected/normal, matches the same phenomenon in dlmalloc's own small bin
  array, not a bug.

---

## Bugs found this session

| # | Location | Description | Status |
|---|---|---|---|
| **1** | `grow_top()` + `my_malloc()` top-carve path | `grow_top()` updated `top_chunk->payload` but never `gm.topsize`; `my_malloc()` then overwrote `payload` and subtracted `needed` from the **stale** `topsize` → unsigned integer underflow, `topsize` becomes a huge garbage value. Also: newly split-off top chunk's header (`payload`/`flags`/`SET_FREE`) was never initialized. | ✅ Fixed — `grow_top()` now does `gm.topsize = gm.top_chunk->payload;` after growing; `my_malloc()` now initializes the new top chunk's header fields explicitly. |
| **2** | `insert_large_chunk()` | Loop never `break`s/`return`s after insertion (risk of double-splice), and has **no fallback insert-at-tail** — if the freed chunk is larger than every existing entry in its bin, it's silently never inserted anywhere. Chunk becomes an unreachable "leak" in the allocator's own bookkeeping. | ⏳ Open — not yet fixed. |
| **3** | `my_free()` shrink-to-OS block | Uses `survivor` (an arbitrary just-coalesced chunk, guaranteed *not* to be `top_chunk` since that case returns early) instead of `gm.top_chunk`/`gm.topsize` to compute `new_break`. Only valid if `survivor` happens to be adjacent to `heap_end`, which isn't checked. | ⏳ **Not actually a bug** — user confirmed this section of `free()` is unfinished/mid-refactor, not a landed defect. Revisit once shrink logic is completed. |
| **4** | `my_malloc()` top-carve, line ~282 | `p->payload = needed` (where `needed = request_size + ALIGN_HEADER_FOOTER`, i.e. the chunk's *total footprint*) — should be `p->payload = request_size` (pure payload only), matching the convention used everywhere else in the file (`split()`, `try_expand()`, `heap_init()`). Causes `BLOCK_NEXT_HEADER` to compute the wrong location for the next chunk on every future heap walk touching this block (e.g. in `coalesce()` when it's later freed) — corrupts heap walking, not a rare edge case since it affects the main top-carve growth path. | ⏳ Open — one-line fix identified, not yet confirmed applied. |
| — | `try_expand()` top-chunk branch | Same missing-header-init issue as Bug 1's second half. | ✅ Fixed — confirmed matches the corrected `my_malloc()` pattern. |
| — | `coalesce()` | No bug — was already correct; it was only a victim of corrupted upstream state from Bugs 1/4. No changes needed. | ✅ Confirmed correct as-is. |
| **5** | `heap_init()`, line ~41 | `raw_payload = INITIAL_HEAP_SIZE - HEADER_SIZE - FOOTER_SIZE` over-reserves by `FOOTER_SIZE` (16 bytes). Top chunk never has a footer (no chunk follows it in memory, consistent with `grow_top()` and `coalesce()`'s top-absorb branch, neither of which account for a footer on top). Caused `check_top_chunk()`'s invariant `(top_chunk+1) + topsize == heap_end` to fail immediately on init — off by exactly `FOOTER_SIZE`. Confirmed with real build+test run: `test_bugs: src/debug.c:18: check_top_chunk: Assertion ... failed`. | ✅ Fixed — user confirmed applying the one-line fix (`raw_payload = INITIAL_HEAP_SIZE - HEADER_SIZE;`, drop the `- FOOTER_SIZE`). |

---

## `debug.c` / `debug.h` review

Existing checks (`check_top_chunk`, `check_bins`, `check_heap`) are a solid
foundation but have a coverage gap: **nothing cross-validates that every
free chunk found by walking the heap actually appears in some bin.** This
gap is exactly what would let Bug 2 (lost large chunk) pass silently.

Proposed addition (not yet added to the file):
```c
void check_heap_bin_consistency(void)
{
    // walk heap, count free chunks (excluding top)
    // walk all gm.bins[], count total via list_length()
    // assert the two counts match
}
```

Also flagged:
- `check_heap()`'s `next == gm.top_chunk` branch currently skips validation
  entirely; should instead assert `!IS_FREE(curr)` (a free chunk adjacent to
  top should never exist under aggressive coalescing).
- No bound-check that `BLOCK_NEXT_HEADER(...)` result stays `<= gm.heap_end`
  — a corrupted `payload` could walk past the end of the heap silently.

---

## Build/tooling notes (resolved this session)

- `-DDEBUG` needs no space (`gcc - DDEBUG ...` is parsed as "read from stdin" +
  a bogus input file named `DDEBUG` — a classic copy-paste spacing mistake).
- Header include path: headers live in `include/`, sources in `src/`, tests
  in `test/` — build needs `-Iinclude` regardless of which directory the
  `.c` file being compiled lives in (quote-includes resolve relative to the
  including file first, so `-I` is required for cross-directory access).
- `gm` in `my-malloc.c` is `static` (file-scope only) by design, so
  `debug.c` can't reference it directly. Resolved via a `pull + cache`
  pattern:
  - `my-malloc.c` exposes `const malloc_state *debug_get_state(void) { return &gm; }`
    under `#ifdef DEBUG`, keeping `gm` itself `static`/encapsulated.
  - `debug.c` caches the pointer once in a local `static const malloc_state *state`,
    via an `ensure_state()` helper called at the top of every `check_*()`
    function. Caching the *pointer* is safe since `gm`'s address never
    changes (it's a static struct) — only its field values change, which
    are read fresh through the pointer each time, not cached.
- On some environments, `-fsanitize=address` failed to link
  (`cannot find libasan.so.8.0.0`) — a missing/mismatched system package,
  not a code or flag issue. Fix is `sudo apt install libasan8` (or the
  matching major version for the installed `gcc`) / `sudo dnf install libasan`
  depending on distro; Valgrind was suggested as a fallback if the package
  can't be installed.

## Testing strategy discussed

- Test incrementally by layer, not all-at-once: `heap_init` → top-carve
  malloc (no bins) → free+coalesce (no bin insert) → bin insert/find →
  grow/shrink. Don't jump to testing later layers before earlier ones are
  verified independently.
- Use `check_*()` debug assertions **after every operation** in tests, not
  just at the end.
- Build with `-DDEBUG -fsanitize=address -g`; Valgrind as a secondary check
  for uninitialized-read bugs specifically.
- **Caught mid-session:** `heap_init()` has a `static bool initialized`
  guard that's correct for production but breaks multi-test-in-one-binary
  setups — second+ calls silently no-op, so a second test function
  unknowingly runs on the first test's leftover heap state. Two fixes
  discussed:
  1. One process per test binary (simplest, no product code changes).
  2. A `#ifdef DEBUG`-gated `heap_reset_for_test()` that resets the
     `initialized` flag — not yet added.
- Bug-specific repro test sketches were drafted for Bug 1 and Bug 4
  (malloc-only, no `free()` needed — safe to write now). Bug 2's repro
  (three increasing-size large-bin frees, biggest freed last) depends on
  `free()`/`insert_large_chunk()` being finished — **on hold**, user is
  still finishing `my_free()`.

---

## Design discussion: top-chunk shrinking strategies

Compared three real-world approaches (grounded via source/docs, not memory):

1. **Fixed threshold** (dlmalloc default, and what this project currently
   uses) — simple, no adaptation.
2. **Adaptive/dynamic threshold** (glibc) — ratchets `mmap_threshold` up to
   the size of the largest recently-freed block (capped at
   `DEFAULT_MMAP_THRESHOLD_MAX`), with `trim_threshold = 2 × mmap_threshold`.
   Real production bug found in glibc history (`BZ #17195`): inconsistent
   threshold application across per-thread heaps caused ~9000 `madvise()`
   calls/sec on a benchmark; fixing consistency dropped it to ~2 calls total
   and ~4x'd throughput. This project's single-arena design sidesteps that
   specific failure mode (no per-thread heaps), which is a genuine advantage
   if adaptive thresholding is added later.
3. **Decay-based purging** (jemalloc) — time-based (sigmoidal decay curve,
   default 10s), purging decoupled from `free()` via background threads.
   Requires infrastructure (background thread) this project doesn't have;
   not recommended as a near-term direction.

**Decision:** user is interested in adding a simplified version of strategy 2
(ratchet-up-only, no decay/EMA) mainly for portfolio appeal, but agreed to
finish and stabilize the four core bugs first before layering in adaptive
threshold logic, since it touches the same `my_free()`/`topsize` code paths.

---

## Current status (end of session)

- `debug.c` is fully wired up (`ensure_state()` pattern) and building
  successfully against `src/my-malloc.c` with the `-Iinclude` fix.
- `check_top_chunk()` ran and immediately caught **Bug 5** (heap_init footer
  over-reservation) — now fixed and confirmed by the user.
- Bugs 1, 4, and the `try_expand`/`coalesce` fixes from earlier were reviewed
  in code but **not yet re-verified against a passing test run** — the test
  binary aborted on Bug 5 before reaching later checks (`check_heap`,
  `check_bins`, `check_heap_bin_consistency`). Re-run the full test suite
  now that Bug 5 is fixed to see whether Bugs 1/4 truly hold up, or whether
  more init-layer issues are hiding behind them.
- Bug 2 (`insert_large_chunk` lost-chunk) is still unfixed in code.
- `check_heap_bin_consistency()` (the cross-check between heap-walk and
  bin contents, designed to catch Bug 2) has been written and added to
  `debug.c`, but hasn't caught anything yet since Bug 2 hasn't been
  exercised (needs `my_free()` finished first).

## Open items / next steps

1. Re-run the test suite now that Bug 5 is fixed — confirm `check_top_chunk`,
   `check_heap` all pass on a fresh `heap_init()` + a few `malloc()` calls.
2. Finish `my_free()` (currently in progress — shrink section explicitly
   unfinished).
3. Fix Bug 2 (`insert_large_chunk` missing `break` + tail-insert fallback),
   then exercise `check_heap_bin_consistency()` against it.
4. Confirm Bug 4 one-line fix (`p->payload = request_size;`) is applied and
   passes `check_heap()` on a top-carved-then-freed chunk.
5. Decide on test isolation approach (separate binaries vs. `heap_reset_for_test()`)
   — needed once more than one test function shares a binary.
6. After `my_free()` is stable: revisit Bug 3 (shrink logic), then consider
   the adaptive shrink-threshold feature (ratchet-up-only, glibc-style,
   deferred until core bugs are stable).