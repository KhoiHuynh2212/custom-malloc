# Context: my-malloc — refactor xong, đang lên kế hoạch viết test

Tóm tắt phiên làm việc (tiếp theo phiên trước). Vai trò AI: mentor senior
systems engineer, dạy theo 5-step (big picture -> diagram -> code tối
thiểu -> pitfall -> câu hỏi kiểm tra), grounded qua source thật, không
đoán từ trí nhớ khi chưa verify.

## Trạng thái refactor kiến trúc — ĐÃ ÁP DỤNG (khác với phiên trước, lúc đó
mới chỉ bàn, chưa làm)

Cấu trúc thật đã có (xác nhận qua `my-malloc.h`, `debug.h`, `internal.h`
mới upload):

```
include/
└── my-malloc.h     <-- CHỈ public API: my_malloc, my_realloc, my_calloc,
                         my_free, my_malloc_trim. Không còn leak struct/macro
                         nội bộ ra ngoài.
src/ (suy ra, chưa thấy lại my-malloc.c/debug.c bản mới)
├── internal.h      <-- MỚI, đã tạo thật. Chứa: struct mblockptr,
│                        malloc_state, mọi macro (ALIGN, SET_FREE, IS_FREE,
│                        BLOCK_NEXT_HEADER...), prototype của các hàm "nội
│                        bộ nhưng cross-file" — get_bin, split, coalesce,
│                        try_expand, find_suitable_block, insert_small_chunk,
│                        insert_large_chunk, trim_chunk, grow_top, heap_init,
│                        debug_get_state (dưới #ifdef DEBUG). internal.h tự
│                        include "../include/my-malloc.h".
├── debug.h         <-- include "internal.h", khai check_heap/check_bins/
│                        check_top_chunk/check_heap_bin_consistency/
│                        check_malloced_chunk/check_mmapped_chunk (mới thêm
│                        so với phiên trước) dưới #ifdef DEBUG, macro no-op
│                        khi không DEBUG.
├── my-malloc.c, debug.c, list.h  (nội dung mới chưa được xem lại trong
    phiên này — cần re-view trước khi sửa gì thêm)
```

## Quyết định kỹ thuật ĐÃ CHỐT — cách test gọi hàm static/internal

Câu hỏi để mở từ phiên trước ("test static functions bằng cách nào") đã
được cấu trúc mới **tự giải quyết**, không cần macro `STATIC` hay
`#include "*.c"` trong test file:

- Các hàm nội bộ (`coalesce`, `split`, `trim_chunk`, `insert_small_chunk`,
  `get_bin`, ...) khai trong `internal.h` là **non-static, external
  linkage**. Test file chỉ cần `#include "internal.h"` + link với
  `my-malloc.o` đã build là gọi được thẳng.
- "Private" ở đây là quy ước ẩn qua header visibility, KHÔNG phải static
  compiler-enforced — giống style glibc (`_int_malloc`, `_int_free` không
  static, chỉ không có trong public header). Nên có 1 dòng comment ở đầu
  `internal.h` ghi rõ đây là chủ ý đánh đổi để test được, không phải sơ sót.
- Test qua public API (`my-malloc.h` only) vẫn dùng được song song, chọn
  theo từng test file, không phải chọn 1 kiểu duy nhất cho cả project.
- Hệ quả cho build test: compile `my-malloc.c` + `debug.c` với `-DDEBUG`
  khi link test binary (để `check_heap()` v.v. là hàm thật, không phải
  no-op), test build cần `-Iinclude -Isrc`.

## Trạng thái 4 bug từ phiên trước

1. `get_bin_bucket` -> `get_bin` đồng bộ 3 chỗ — **ĐÃ FIX** (xác nhận ở
   phiên trước, chưa re-verify trong `internal.h` mới nhưng prototype
   `int get_bin(size_t payload);` khớp).
2. `insert_small_chunk`/`insert_large_chunk` static vs non-static mismatch
   — **ĐÃ FIX** (phiên trước xác nhận non-static, khớp header).
3. `malloc_trim` trùng tên libc, sai chữ ký — **ĐÃ FIX ở public header**:
   `my-malloc.h` giờ chỉ khai `size_t my_malloc_trim(void);`, không còn
   `malloc_trim` cũ. Prototype cũng thấy trong `internal.h`
   (`trim_chunk`). *Chưa xem lại `my-malloc.c` bản mới* để confirm định
   nghĩa hàm khớp tên — cần re-view trước khi tin tưởng 100%.
4. Biến `psm1` khai nhưng không dùng trong `my_malloc_trim`/`trim_chunk`
   — **CHƯA XÁC NHẬN LẠI**, vì chưa thấy `my-malloc.c` bản mới trong phiên
   này. Cần view lại file trước khi coi là đã fix hay chưa.

**Việc cần làm đầu phiên sau: xin/`view` lại `my-malloc.c` và `debug.c`
bản mới nhất để re-verify bug #3 và #4, trước khi bắt đầu viết code test
thật.**

## Test plan đã thống nhất — 4 lớp

1. **Internal mechanics (white-box, qua `internal.h`)**
   - `get_bin(payload)`: biên `0`, `SMALL_BIN_MAX - 1`, `SMALL_BIN_MAX`,
     `2^LARGE_BIN_MIN_EXP`, `2^LARGE_BIN_MAX_EXP`, và giá trị vượt max
     (phải clamp).
   - `split()`: payload đúng `request_size + MINBLOCKSIZE - 1` (KHÔNG
     split) vs đúng `MINBLOCKSIZE` (phải split); kiểm footer của phần dư.
   - `coalesce()`: 4 case — không neighbor free, prev free, next free, cả
     hai free; case absorb vào top chunk test riêng.
   - `trim_chunk()`: bảng test data-driven `{payload, expect_trimmed}`,
     dùng lại số đã tính tay phiên trước (payload=4096, page=4096), cả
     case payload_start đã align sẵn lẫn chưa align, và case payload nhỏ
     hơn 1 page (`expect_trimmed == 0`).

2. **Public API (black-box, chỉ `my-malloc.h`)**
   - size 0, size khổng lồ (`SIZE_MAX`), round-trip alloc/free/
     realloc-grow/realloc-shrink, overflow guard của `my_calloc`
     (`num * size` overflow), double-free (phải abort — chạy qua
     fork/subprocess để không giết luôn test runner).

3. **Invariant/property test (dùng lại `check_*` làm oracle)** — lớp đòn
   bẩy cao nhất vì tận dụng code debug đã có sẵn. Chạy chuỗi random
   `my_malloc`/`my_free`/`my_realloc`, sau mỗi N thao tác gọi
   `check_heap()`, `check_bins()`, `check_heap_bin_consistency()`,
   `check_top_chunk()`. Bug ở `coalesce`/bin-insertion sẽ lộ ra dù không
   có unit test nào target trực tiếp.
   - Lưu ý performance: `check_heap_bin_consistency()` là O(tổng số free
     chunk) qua `list_length()` từng bin (list_length tự nó O(n)) — với
     heap có nhiều free chunk (vd 10k), gọi sau MỌI thao tác trong vòng
     lặp random sẽ tốn — cân nhắc gọi sau mỗi N thao tác (vd N=50-100)
     thay vì mỗi thao tác một lần. (Câu hỏi đang để mở cho người dùng tự
     ước lượng, chưa chốt N cụ thể.)

4. **Regression test** — 1 test cho mỗi bug đã tìm+fix (tên hàm
   `get_bin`, `my_malloc_trim`, non-static insert_*...), để tránh
   regress lại.

## Đã học thêm trong phiên này (grounded)

- `check_heap_bin_consistency()`: đây là cross-structure invariant check
  — so sánh "ground truth" (đếm free chunk bằng cách duyệt vật lý cả
  heap) với "index" (tổng độ dài tất cả list trong `gm.bins[]`). CHỈ
  check số lượng khớp, KHÔNG check đúng bin (đó là việc của
  `check_bins()` qua `get_bin(curr->payload) == i`). Vi phạm invariant
  này là bug âm thầm (không crash ngay): free_chunk > binned_cnt nghĩa
  là có free block "vô hình" với allocator (leak logic — không bao giờ
  được tái sử dụng dù đã free đúng cách); binned_cnt > free_chunk nghĩa
  là có node trong bin trỏ tới vùng nhớ không còn là free chunk hợp lệ
  (dangling/corruption, sẽ crash ở lần alloc kế tiếp đụng phải node đó).
  Các nguyên nhân điển hình: quên `insert_*` sau `SET_FREE`, quên
  `list_unlink` trước khi merge 2 chunk trong `coalesce`, hoặc `split()`
  insert cả node gốc lẫn phần dư vào bin.

## Việc tiếp theo (thứ tự đề xuất)

1. `view` lại `my-malloc.c` và `debug.c` bản mới nhất — re-verify bug #3
   (tên hàm `my_malloc_trim` định nghĩa khớp) và #4 (`psm1` unused, còn
   tồn tại không).
2. Quyết định N (tần suất gọi `check_heap_bin_consistency` trong vòng lặp
   random test) — người dùng chưa trả lời câu hỏi này.
3. Dựng `tests/` skeleton thật (chưa tồn tại) + `Makefile` target `test`
   (build `my-malloc.o`/`debug.o` với `-DDEBUG`, link test binary với
   `-Iinclude -Isrc`).
4. Viết test layer 1 trước (internal mechanics), bắt đầu từ bảng
   `trim_chunk` đã có số tính tay sẵn — ít tốn công nhất để có test đầu
   tiên chạy được, verify được toàn bộ toolchain (build + link + assert)
   trước khi mở rộng ra layer 2-4.
5. Sau khi layer 1 chạy ổn, viết layer 3 (property test) tái dùng
   `check_*` — giá trị cao, code debug đã có sẵn, chỉ cần viết driver
   random.