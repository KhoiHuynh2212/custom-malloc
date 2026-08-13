# Custom Allocator (`my-malloc`) — Session 3 Context

Continues directly from Session 2's context (bugs 1–9 fixed, bug 10 —
floor-vs-pad — left explicitly unresolved as the first item for this
session). This session resolved it, but along the way surfaced a new,
real, unfixed bug in the same shrink block. **Nothing in this session's
findings has been applied to code yet** — `/mnt/user-data/uploads/my-malloc.c`
already contains the floor-removal redesign the user wrote independently;
the sbrk-ordering fix discussed below is not yet in that file.

---

## Session 3 starting point

User uploaded a new `my-malloc.c` with an independent redesign of the
shrink block, plus a field rename (`gm.top_chunk` → `gm.topchunkptr`,
applied consistently across `heap_init`, `grow_top`, `coalesce`,
`my_malloc`, `try_expand`, `my_free`). Diffed against the project copy to
confirm scope — rename only, no other unrelated drift.

---

## Bug 10 (floor-vs-pad) — resolved this session

**User's own diagnosis, stated directly**: the floor (absolute address
anchored to `heap_start`, from Session 2) doesn't belong in this design.
Reasoning: `top_chunk` already moves continuously as normal allocation
carves from it, so pinning a shrink boundary to a *fixed* address
(`heap_start + INITIAL_TOP_SIZE`) is a mismatch — real trim logic should
be purely relative to wherever `top_chunk` currently sits.

**Verified against real source** (not just accepted on reasoning):
searched glibc's `systrim()`. Its trim math —
`top_area = top_size - MINSIZE - 1; extra = ALIGN_DOWN(top_area - pad, pagesize)`
— references only the top chunk's own size and the `pad` parameter. No
reference to the original/initial break anywhere. This confirms the
Session 2 floor was an invented constraint, not something grounded in how
real allocators trim.

**User's fix** (already in the uploaded file): drop the floor clamp
entirely, always shrink to `topchunkptr + HEADER_SIZE + TOP_PAD_SIZE`, and
assign `payload = TOP_PAD_SIZE` directly instead of deriving it from an
address subtraction.

**This is not just "acceptable," it's algebraically identical to the old
formula in the case where the floor clamp never fired.** Worked through
the substitution live:

```
new_break (unclamped) = heap_end - topsize + SHRINK_KEEP
payload_new            = new_break - top_chunk - HEADER_SIZE
                        = (heap_end - top_chunk - HEADER_SIZE - topsize) + SHRINK_KEEP
                        = 0 + SHRINK_KEEP                  (since topsize IS that difference)
                        = SHRINK_KEEP  ==  TOP_PAD_SIZE
```

So the new code is a simplification that exploits this identity, not a
behavior change in the common case — and removes the case-split (clamped
vs. unclamped) entirely, which is the correct simplification once the
floor's rationale doesn't hold. `check_top_chunk()`'s invariant
(`(top+1) + topsize == heap_end`) still holds exactly under the new code.

**Follow-up recommended, not yet applied**: add
`static_assert(TOP_PAD_SIZE < SHRINK_THRESHOLD, ...)` in `my-malloc.h`.
Previously this relationship held only because `SHRINK_KEEP = CHUNK_SIZE/4`
and `SHRINK_THRESHOLD = CHUNK_SIZE*2` happened to satisfy it; the floor
clamp used to be the thing implicitly protecting the subtraction
`heap_end - topchunkptr_after_shrink` from underflow if that ever drifted.
With the floor gone, nothing enforces it anymore — should be a
compile-time assertion, not an assumption.

**Status: bug 10 is now considered correctly resolved, verified via real
source (glibc), not just code review.** No longer an open item.

---

## New bug found this session (not yet fixed): unchecked `sbrk()` return in shrink block

While reviewing the new shrink block, found:

```c
gm.topchunkptr->payload = TOP_PAD_SIZE;
gm.topsize = gm.topchunkptr->payload;
gm.heap_end = (char *)topchunkptr_after_shrink;
sbrk(-(intptr_t)actual_shrink_amt);   // return value discarded
```

**Problem**: allocator state (`gm.topsize`, `gm.heap_end`, `payload`) is
committed *before* confirming the kernel actually moved the break. If
`sbrk()` fails (returns `(void*)-1`), the real break hasn't moved, but
`gm.heap_end` now claims a lower address than reality — every future
address computation derived from `gm.heap_end` is now silently wrong.
Classic fail-silent bug, same fingerprint as bug 6 from Session 2 (state
diverging from reality, symptom shows up several calls later in an
unrelated place).

**This is a real, still-open bug — not fixed in the uploaded file.**

**Correct pattern discussed** (not yet written into the file): compute
values into locals, call the fallible syscall, check its result, *only
then* mutate `gm.*`. This is the same shape already used correctly in
`grow_top()` (`sbrk()` first, check `== (void*)-1`, bail before touching
`gm` on failure, commit only after success). The fix is to reorder the
shrink block to match:

```c
if (sbrk(-(intptr_t)actual_shrink_amt) != (void *)-1)
{
    gm.topchunkptr->payload = TOP_PAD_SIZE;
    gm.topsize = gm.topchunkptr->payload;
    gm.heap_end = (char *)topchunkptr_after_shrink;
}
// else: sbrk failed, gm untouched — no rollback machinery needed,
// because nothing was committed yet
```

General principle established for future review: "rollback" is the wrong
mental model for `void*`-returning syscalls with no undo — the real
pattern is **compute → confirm → commit**, only ever mutating shared
state after the fallible call succeeds.

**Status: identified and the fix is agreed, but not yet applied to
`my-malloc.c`.** Next session should apply this reorder before anything
else touches the shrink block.

---

## Testing this bug — approach discussed, not yet written

Normal test runs can't reach the `sbrk` failure branch — `sbrk(negative)`
essentially never fails on a single-threaded dev box. This needs **fault
injection** via the linker, not a normal test.

Planned approach (not yet implemented as a file):

- Link with `-Wl,--wrap=sbrk`, which redirects unresolved calls to `sbrk`
  inside the linked object files to a test-defined `__wrap_sbrk`, with
  `__real_sbrk` available to call the genuine libc version.
- `__wrap_sbrk` uses a test-controlled flag (e.g. `force_fail`) to return
  `(void*)-1` on demand for negative increments only, forwarding
  everything else (including the initial `heap_init()` growth) to
  `__real_sbrk`.
- Drive the allocator into the shrink branch normally, flip the fail flag
  immediately before the triggering `free()`, then assert on
  `debug_get_state()`'s fields to confirm `gm.heap_end` / `gm.topsize`
  were **not** mutated when `sbrk` reports failure.

Flagged pitfalls to remember when this gets written: calling `sbrk(...)`
instead of `__real_sbrk(...)` inside `__wrap_sbrk` self-recurses into a
stack overflow; the fail-flag approach isn't thread-safe (fine for now,
not fine if concurrency tests reuse this harness); failing every `sbrk`
call including the one inside `heap_init()` breaks test setup before the
interesting case is even reached — the wrapper must be selective.

**Status: no test file created yet.** User has not started writing it —
correctly recognized as a fault-injection problem rather than a normal
unit test before attempting it. This is the right next step to pick up.

---

---

## New gap found this session (not yet fixed): `find_suitable_block()` không climb bin / does not climb to the next bin

**VI**: User tự phát hiện — hiện tại `find_suitable_block(request_size)`
chỉ nhìn đúng `gm.bins[idx]` với `idx = get_bin_bucket(request_size)`. Nếu
bin đó rỗng, hàm `return NULL` ngay (dòng 80-84), dù một bin *lớn hơn* có
thể đang có chunk free hoàn toàn dùng được. Kết quả: allocator carve từ
top chunk / gọi `grow_top()` một cách không cần thiết, dù bộ nhớ free phù
hợp đã tồn tại — gây phình heap (heap bloat) và tốn `sbrk()` oan.

**EN**: `find_suitable_block()` only ever looks at the single exact bin
`gm.bins[idx]`. If that bin is empty it gives up immediately (lines
80-84) instead of checking whether a *larger* bin has a free block that
would satisfy the request. This causes unnecessary top-chunk carving /
`grow_top()` calls even when usable free memory exists elsewhere —
classic segregated-fit gap, not a crash bug, a design/efficiency gap.

**Grounded via real source (verified this session, not recited from
memory)**: searched dlmalloc/ptmalloc's `malloc.c`. It solves this with a
**binmap** — a bitmap tracking which bins are non-empty — so on a miss it
jumps to the next non-empty bin in O(1) via `least_bit(leftbits)` /
`idx2bit` bit-scan tricks, rather than scanning bin-by-bin. Relevant
defines confirmed in source: `idx2bit`, `mark_bin`, `unmark_bin`,
`get_binmap`, `BINMAPSHIFT`.

**Quyết định phạm vi / scope decision**: dùng vòng lặp tuyến tính đơn
giản (`for idx..NUM_BINS-1`, dừng ở bin non-empty đầu tiên) cho bước này,
**không** làm bitmap ngay — file đã tự ghi chú
`// THIS IS STILL O(N) - can be optimize if use tree O(log n)` ngay tại
chỗ scan trong 1 bin, nên bitmap là tối ưu hoá cho *sau này*, không trộn
vào cùng lúc với fix correctness/coverage này.

**Vì sao climb lên là an toàn (correctness)**: theo cách `get_bin_bucket()`
được thiết kế — small bin idx tăng đúng theo size (mỗi bậc 16 byte), large
bin idx tăng theo dải luỹ thừa 2 — nên bin có idx **cao hơn** luôn chứa
chunk `payload >= request_size`. Không cần lo chunk lấy được bị nhỏ hơn
yêu cầu. Because of how `get_bin_bucket()` assigns bucket indices,
any bin above `idx` is guaranteed to hold chunks large enough.

**Không cần sửa gì thêm ở `my_malloc()`**: bước `split()` sau khi
`find_suitable_block()` trả về đã tự cắt phần dư nếu đủ lớn
(`payload >= request_size + MINBLOCKSIZE`), nên chunk lấy từ bin cao hơn
tự động được xử lý đúng, không cần logic riêng.

**Status: đã thống nhất hướng, chưa viết code.** User sẽ tự viết vòng lặp
climb-bin sau (`for (int i = idx; i < NUM_BINS; i++) { ... }`, tái dùng
logic scan-trong-1-bin đã có). Not implemented yet — next session or a
later pass should pick this up alongside the other open items below.

---

## Current status (end of session 3)

- `/mnt/user-data/uploads/my-malloc.c`: `topchunkptr` rename applied
  everywhere; floor-vs-pad redesign applied and verified correct
  (algebraically and against real glibc behavior). **Not yet copied to
  `/mnt/user-data/outputs/` as a corrected file** — no output file was
  generated this session, only review/discussion.
- New unfixed bug identified in the same shrink block (unchecked `sbrk`
  return, commit-before-confirm ordering). Fix agreed but not applied.
- `my-malloc.h` still needs the `TOP_PAD_SIZE < SHRINK_THRESHOLD`
  static_assert added (carried over recommendation, not yet done).
- No test file written this session for either the floor-vs-pad change or
  the new sbrk-ordering bug.

## Open items / next steps

1. **Apply the sbrk-ordering fix** to the shrink block in `my_free()` —
   compute → confirm → commit, matching `grow_top()`'s existing pattern.
2. Add `static_assert(TOP_PAD_SIZE < SHRINK_THRESHOLD, ...)` to
   `my-malloc.h`.
3. Write the `--wrap=sbrk` fault-injection test harness for the shrink
   failure path (see approach above) — user's next actual task.
4. Everything still open from Session 2, unchanged:
   - Diagnose the non-deterministic crash (persistent mmap'd allocation +
     shrink combo) — `gdb` or `-fsanitize=undefined`, not printf
     bisection. Still flagged as "before anything else touches
     `my_free()`" — this session did touch `my_free()`'s shrink block
     again, so this should move back to top priority next time.
   - Stress-test `insert_large_chunk()`'s sorted-insert-in-the-middle
     path (only the empty-bin fallback has been exercised so far).
   - Decide a permanent home for scratch test files
     (`test_sawtooth.c`, `test_sawtooth_debug2.c`, `test_floor.c`) —
     likely `test/`.
   - Test isolation strategy (`heap_reset_for_test()` vs.
     one-process-per-test) still undecided.
5. Once the sbrk-ordering fix and its test are both in, produce a
   corrected `my-malloc.c` in `/mnt/user-data/outputs/` — none was
   generated this session since no code changes were actually applied.
6. Implement bin-climbing in `find_suitable_block()` (see section above)
   — linear scan from `idx` to `NUM_BINS - 1`, first non-empty bin wins.
   Correctness-safe by construction of `get_bin_bucket()`; no changes
   needed elsewhere (`split()` already handles the leftover). User to
   write it; discuss/review next time it comes up.