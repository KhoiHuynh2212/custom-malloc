# Session Context — Custom `malloc` Implementation (continued)

**Project:** `my-malloc.c` / `my-malloc.h` / `list.h` — sbrk+mmap allocator, moving
from a single flat free list toward: segregated bins + a dedicated `top_chunk`
(wilderness) pattern.

This supersedes the earlier `malloc-session-context.md` — read this one first;
it includes everything from before plus what happened after.

---

## Bugs found and fixed in the earlier part of this project (already delivered)

| # | Bug | Status |
|---|---|---|
| 1 | `extend_heap`'s exact-fit branch mislabeled "never reach this branch possible" — reachable on every `[CHUNK_SIZE, MMAP_THRESHOLD)` request | Fixed: `allocate_size = block_chunk + MIN_FREE_BLOCK`. Test added. |
| 2 | Integer overflow in `my_malloc`'s mmap path (`ALIGN_HEADER_FOOTER + request_size` could wrap near `SIZE_MAX`, silently mmap'ing a tiny buffer while reporting success) | Fixed: added `SIZE_MAX - ALIGN_HEADER_FOOTER` guard. Test added. |
| 3 | `my_realloc`'s in-place sbrk-extend gated on `allocated_size` (increment) instead of `request_size` (result) — near-threshold blocks could grow well past `MMAP_THRESHOLD` while staying sbrk-backed | Fixed: gate changed to `request_size < MMAP_THRESHOLD`. Test added. |
| 4 | `rover` (next-fit cursor) never actually advanced past a mid-scan match — degenerated to first-fit | Fixed: `rover = curr->next;` added at the successful-match return site in `find_suitable_block`. Verified: repeat search dropped from 11 scan steps to 1. |
| 5 | `heap_init()`: `initialized = true` set (and mutex unlocked) **before** `heap_start`/`heap_end`/`bins[]`/`top_chunk` are actually written — concurrent caller can see `initialized == true` and use a null/uninitialized `top_chunk` | **Fix proposed, NOT yet applied to the real file.** Move `initialized = true` to the last statement before unlock. Confirmed via ThreadSanitizer + a plain repro (up to 7/8 worker threads observed `top_chunk == NULL`). |

Full suite as of last delivery: 138/138 passing (does not yet cover bug #5, or anything below this line).

---

## New this session

### Bug 6 — `grow_top()` has two stacked bugs, one of them a crash

User pasted:
```c
Block *grow_top(size_t size)
{
    size_t block_chunk = size + HEADER_SIZE + FOOTER_SIZE;
    size_t allocate_size = (size < CHUNK_SIZE) ? CHUNK_SIZE : block_chunk + MINBLOCKSIZE;
    void *request = sbrk(allocate_size);
    g_sbrk_calls++;
    if (request == (void *)-1) return NULL;
    top_chunk = (Block *)request;
    top_chunk->payload += allocate_size;   // BUG A
    top_chunk->free = 0;
    set_footer(top_chunk);
    SET_FREE(top_chunk);
    list_init(&top_chunk->list);
    heap_end = (char *)request + allocate_size;
    return top_chunk;
}
```

- **Bug A (crash):** `top_chunk->payload += allocate_size` doesn't subtract
  `HEADER_SIZE + FOOTER_SIZE`, so `set_footer()` writes 32 bytes past the
  actual sbrk'd region. **Reproduced: SEGV on the very first call**, confirmed
  via ASan (`SEGV on unknown address ... #0 set_footer #1 grow_top`).
- **Bug B (silent leak):** `top_chunk = (Block *)request;` creates a **new**,
  disconnected block at the fresh sbrk address instead of extending the
  **existing** `top_chunk` in place. The old `top_chunk` becomes permanently
  unreachable (not in any bin, no longer pointed to) — every call after the
  first leaks its predecessor's entire allocation. Reproduced: isolated Bug A,
  called `grow_top` twice, confirmed `first != second` (orphaned).
- **Fix proposed (not yet applied to the real file):**
  ```c
  Block *grow_top(size_t size)
  {
      size_t block_chunk = size + HEADER_SIZE + FOOTER_SIZE;
      size_t allocate_size = (size < CHUNK_SIZE) ? CHUNK_SIZE : block_chunk + MINBLOCKSIZE;
      void *request = sbrk(allocate_size);
      g_sbrk_calls++;
      if (request == (void *)-1) return NULL;

      // request == old heap_end == right after the existing top_chunk,
      // by construction -- this is genuine in-place extension.
      top_chunk->payload += allocate_size;
      set_footer(top_chunk);
      heap_end = (char *)request + allocate_size;
      return top_chunk;
  }
  ```
  Removed: the `top_chunk = (Block*)request` reassignment and the
  re-initialization lines (`free`, `SET_FREE`, `list_init`) — none of that
  should re-run on an already-initialized block.
  **Precondition this now depends on:** `top_chunk` must already exist
  (created by `heap_init`) before `grow_top` is ever called.

### `top_chunk` footer — resolved
Discussed whether `top_chunk` (always flush against `heap_end`, nothing ever
sits to its right) needs a footer at all. Conclusion: not functionally
required (no block will ever do a backward-coalesce lookup into it), but
**keep it anyway** — avoids a special-case branch everywhere a block gets
created/resized, and keeps a future heap-consistency-checker simple.
(This is different from the researched repo's epilogue, which *must* skip
its footer because its size is fake/unbacked memory — `top_chunk` is always
real, backed memory, so writing its footer is just slightly wasteful, not
dangerous, once Bug A above is fixed.)

### `INITIAL_HEAP_SIZE` decoupled from `MMAP_THRESHOLD`
Chosen value: `INITIAL_HEAP_SIZE = CHUNK_SIZE` (64 KB) — ties the constant to
the existing "one growth unit," rather than reusing `MMAP_THRESHOLD` for two
unrelated purposes (startup size vs. mmap cutoff).

### Bin-index algorithm — finalized and verified
Researched the real glibc mechanism via Azeria Labs' heap-exploitation
series (`https://azeria-labs.com/heap-exploitation-part-2-glibc-heap-free-bins/`):
62 small bins (exact-size, `< 1024` bytes), power-of-two large bins above
that, small bins auto-sorted by construction (O(1) insert), large bins need
manual sort + traversal (slower).

Final single-function design, tailored to this codebase's actual
`MMAP_THRESHOLD` (128 KB) rather than copying a range built for glibc's
different threshold:

```c
#define SMALL_BIN_MAX      1024
#define NUM_SMALL_BINS     64     // indices 0..63 -- index 0 permanently unused
#define LARGE_BIN_MIN_EXP  10     // 2^10 = 1024
#define LARGE_BIN_MAX_EXP  16     // 2^16 = 65536 -- last bin reaches up to MMAP_THRESHOLD
#define NUM_LARGE_BINS     (LARGE_BIN_MAX_EXP - LARGE_BIN_MIN_EXP + 1)  // 7
#define NUM_BINS           (NUM_SMALL_BINS + NUM_LARGE_BINS)            // 71

int size_to_bin(size_t payload)
{
    if (payload < SMALL_BIN_MAX)
        return payload >> 4;              // exact-size bin: payload / ALIGN

    int msb = 63 - __builtin_clzl((unsigned long)payload);
    if (msb < LARGE_BIN_MIN_EXP) msb = LARGE_BIN_MIN_EXP;  // defensive
    if (msb > LARGE_BIN_MAX_EXP) msb = LARGE_BIN_MAX_EXP;  // clamp, catch-all

    return NUM_SMALL_BINS + (msb - LARGE_BIN_MIN_EXP);
}
```

Verified via probe: all 7 large-bin range-pairs map to matching bins, all
adjacent boundaries differ correctly, defensive clamp holds even for
`SIZE_MAX`. **63 usable small bins** (index 0 dead — `ALIGN_UP` never
produces a payload under 16), **7 large bins** — 71-slot array total.

**Rationale settled (for the diary entry the user wrote):** bin density
should track *frequency × relative fit-waste*, not raw bin count. Small
sizes are frequent and a bad match wastes a high percentage of a small
request → need exact bins. Large sizes are rare and a bad match wastes a
low percentage of a large request → power-of-two bins are cheap enough.
Matching small-bin precision up to `MMAP_THRESHOLD` would cost ~8191 bins
(~128 KB of sentinels) for precision that mostly never gets reused, since
large-allocation sizes rarely repeat exactly.

One self-correction logged during this session: initially claimed glibc
"keeps large bins sorted for something closer to best-fit" with more
confidence than the source (Azeria Labs article) directly supports — the
article confirms sorted insertion + traversal-required lookup, but doesn't
spell out best-fit-as-a-goal in those terms. Flagged as inference, not
direct quote.

---

## Repos referenced this session (for the diary / future reading)
- `vmaksimovski/Malloc-Implementation` — segregated bins + epilogue-in-last-bin trick (fake huge size, unbacked memory, must skip its footer)
- `dlmalloc` (ARMmbed/ennorehling mirrors) — canonical top-chunk/wilderness pattern; `aradzie/dlmalloc` is a more readable rewrite
- `lattera/glibc` — `malloc/malloc.c`, authoritative `sysmalloc()`/`_int_malloc()` top-chunk logic
- `shellphish/how2heap` — minimal isolated top-chunk field-manipulation examples (security-education framing, but useful in isolation for seeing the mechanics)
- Azeria Labs heap-exploitation series (part 2) — grounded the 62-small-bin / large-bin / bin-index-array structure discussion

---

## Not yet applied to the actual file (action items for next session)
1. `heap_init()` race fix (Bug #5) — reorder `initialized = true` to after all setup, before unlock.
2. `grow_top()` fix (Bug #6) — stop reassigning `top_chunk`, extend the existing block's payload in place instead; remove the redundant re-init lines.
3. Wire `size_to_bin()` into `find_suitable_block`, `split`, `coalesce`, `try_expand`, `my_free` — replace all `&head.list` usage with `&bins[size_to_bin(payload)]`.
4. Decide the fate of `rover` now that bins exist (likely: drop it, since narrow bins shouldn't need a persistent next-fit cursor) — not yet decided or implemented.
5. Wire the `top_chunk` fallback tier into `my_malloc` (search bins → top_chunk → grow_top) and the coalesce-into-top rule into `my_free`/`coalesce` (guard against `list_unlink`-ing something that was never linked).
6. Add a regression test exercising concurrent `heap_init()` calls specifically (existing concurrency test only calls `heap_init()` once, before spawning threads — never exercised bug #5).