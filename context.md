# my-malloc — Session Context

*Drop this at the top of a new chat to pick up exactly where this session left off.*

## What this project is

A custom heap allocator (`my-malloc.c` / `my-malloc.h`) built on an intrusive
circular doubly-linked free list (`list.h`) plus direct `mmap()` for large
allocations. Every allocation is prefixed with a `Block` header (`payload`,
`free` bitflags, `list` node) and followed by a footer (copy of `payload`)
used to walk backward for coalescing.

```c
typedef struct Block {
    size_t payload;   // usable size
    int    free;      // bit 0 = FREE_BIT, bit 1 = MMAP_BIT
    list   list;       // intrusive free-list node
} Block;
```

Key constants: `ALIGN=16`, `HEADER_SIZE=32`, `FOOTER_SIZE=16`,
`MIN_FREE_BLOCK=64`, `CHUNK_SIZE=4096`, `MMAP_THRESHOLD=128KB`.

## Status: all known bugs fixed, correctness suites passing

Current state (37/37 basic, 49/49 edge-case checks, 0 suites failed,
clean under ASan/UBSan) — see `test-basic.c` / `test-edge-cases.c`.

## Bugs found and fixed this session (chronological)

1. **Uninitialized `Block.free`** — `SET_FREE`/`SET_ALLOCATED` are
   read-modify-write macros; they only guarantee the bit they target.
   Fresh sbrk-carved blocks (`heap_init`, `request_block`, `split`'s
   remainder) never had `->free` zeroed first, leaving other bits
   undefined. **Fix:** `block->free = 0;` before the first macro touches
   each new header. Found via Valgrind.

2. **Heap shrink underflow** — `my_free()`'s shrink-to-OS logic had no
   floor, so it could shrink a coalesced free block at `heap_end` past
   the point where measurement started, producing negative "heap grew"
   deltas. **Fix:** clamp shrink to `heap_start + MMAP_THRESHOLD`.

3. **Stale `rover` → double free (serious)** — after adding a `rover`
   next-fit pointer to `find_suitable_block()`, `split()` correctly
   invalidated `rover` before unlinking a block, but the *sibling* path
   in `my_malloc()` (reusing a found block without splitting) did not.
   A self-linked, newly-allocated block left `rover` pointing at itself;
   the next search treated it as free again, silently aliasing two live
   pointers to the same memory. **Fix:** same `if (rover == &block->list)
   rover = &head.list;` guard added to that path. **Lesson:** whenever a
   new invariant like `rover` is threaded through the code, grep every
   `list_unlink()` call site — don't trust that the common path was the
   only one that needed the guard.

4. **mmap page-rounding lost, then restored** — `mmap()` always returns
   whole pages; recording only the requested size (not the rounded-up
   page size) in `block->payload` threw away free headroom, which
   silently defeated the mmap fast-path in `my_realloc`. Round the mmap
   request up to `sysconf(_SC_PAGESIZE)` and record the *actual* mapped
   capacity.

5. **Wilderness extension: reassignment bug (severe)** — first attempt
   at "if this block is last in the heap, just grow it via sbrk"
   reassigned the local `block` pointer to the *new* raw sbrk memory
   instead of extending the *existing* block's payload — orphaning the
   original block's real size metadata and creating an untracked,
   uninitialized phantom block. **Fix:** never reassign `block`; grow
   `block->payload` in place, `set_footer()` again, extend `heap_end`.

6. **Wilderness extension: fired on shrinks + size math double-counted**
   — the `next_block == heap_end` check ran *before* checking whether
   growth was even needed, so pure shrinks triggered a real `sbrk()`
   call. Separately, `allocated_size` was computed from `new_payload`
   (the *target total*) and then added *on top of* the existing
   payload — a ~2x overshoot minimum, worse for large targets. **Fix:**
   moved the wilderness check to only run after the "already fits" and
   `try_expand()` checks fail (so growth is guaranteed needed by
   construction), and compute `shortfall = new_payload - block->payload`
   as the actual amount to request, floored at `CHUNK_SIZE` for batching.

## Performance work (realloc-heavy benchmark: ~6,000 ops/s → ~5.5M ops/s)

Order of investigation matters here — first hypothesis was wrong and
worth remembering as a process lesson:

- **First theory (partially wrong):** assumed the free-list linear scan
  in `find_suitable_block()` was the bottleneck under repeated realloc
  growth. Instrumented it directly — turned out to be near-irrelevant
  (8 calls, 5 total scan steps across 100k ops).
- **Actual bottleneck, found via instrumentation:** ~83,000 of ~83,274
  relocations were hitting the `mmap`/`munmap` path (large sizes cross
  `MMAP_THRESHOLD` quickly in a growth pattern). Fixed via:
  - **mmap page-rounding** (see bug #4 above) — gives real headroom.
  - **Extending the "already fits" fast path to mmap blocks** — this
    check previously only existed for sbrk blocks.
  - **`mremap()` instead of mmap-new+memcpy+munmap-old** for mmap
    relocations — kernel resizes via page-table updates instead of a
    full userspace copy.
- **`rover` next-fit search** added to `find_suitable_block()` — search
  resumes from the last successful match instead of restarting at head
  every call, spreading search cost as the free list grows.
- **Wilderness extension** (see bugs #5/#6) — top-of-heap blocks grow via
  `sbrk()` directly instead of falling through to a full relocate.

Current gap to glibc on the realloc-heavy benchmark: ~2.8x (down from
~2,200x at the start of this investigation). Remaining gap is
architectural — glibc has segregated size-class bins vs. this
allocator's single free list (rover next-fit narrows but doesn't close
that gap).

## Testing infrastructure (all three files use the same pattern)

- **`test-basic.c`** / **`test-edge-cases.c`** / **`benchmark.c`** — all
  registry-driven (`TestCase`/`PhaseCase` tables instead of hand-called
  functions in `main()`) and fork-isolated (each test/phase runs in its
  own child process via `fork()`, results aggregated through
  `mmap(MAP_SHARED)` counters, so one crash doesn't kill the whole run).
- `test-edge-cases.c` additionally nests a second fork inside
  `test_double_free()` to catch an *intentional* `SIGABRT` — outer fork
  protects against surprise crashes, inner fork confirms an expected one.
- `benchmark.c`: cold-start subprocess per phase, percentile stats
  (p50/p90/p99/max not just mean), `escape()` compiler barrier to
  prevent `-O2` dead-code elimination, `sbrk(0)`-based heap growth
  accounting, glibc side-by-side comparison.

## Known gotchas / things to double check next session

- **`LINUX_PAGE` macro** (used for page-rounding) was referenced in the
  user's local `my-malloc.c` but not present in the version of
  `my-malloc.h` synced to this session's project files — verify it's
  actually defined as `((size_t)sysconf(_SC_PAGESIZE))` somewhere before
  assuming a fresh checkout compiles.
- **`#include <stdint.h>`** for `uintptr_t` has been added/dropped a
  couple of times across uploads in this session — double check it's
  present in whatever version you start from next.
- **Benchmark noise:** a single run's throughput numbers can vary ~2x
  run to run from system noise. Always compare medians of 3+ runs before
  concluding a change helped or hurt (this caught us once — see the
  first growth-factor attempt that looked like a regression but was
  actually a no-op due to a threshold guard).
- **Fragmentation-overhead metric blind spot:** `(grown - requested) /
  requested * 100` reports exactly `-100%` whenever `grown == 0` — true
  whenever a workload fits entirely inside the initial 128KB heap
  reservation. Use `--ops 50000`+ before trusting that number.

## Not yet done / good next steps

- **Bidirectional `try_expand()`** — currently forward-only (absorbs the
  *next* neighbor if free). Backward absorption is possible but requires
  changing the return type to `Block *` (survivor may not be `curr`) and
  a `memmove()` of live payload data if the backward direction is used
  (unlike `coalesce()`, which never needs to preserve data since it's
  only ever called on already-free blocks).
- **mremap growth path doesn't page-round** — `nb->payload = new_payload`
  after a successful `mremap()` doesn't get the same page-rounding
  headroom treatment the initial mmap path has. Lower priority than the
  bugs above, but same pattern to apply.
- Thread safety was never in scope this session — nothing here is safe
  for concurrent access.