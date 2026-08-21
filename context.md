# Context: my-malloc — trim giữa heap & refactor kiến trúc

Tóm tắt phiên làm việc, dùng để tiếp tục ở session sau. Vai trò AI: mentor
senior systems engineer, dạy theo 5-step (big picture -> diagram -> code
tối thiểu -> pitfall -> câu hỏi kiểm tra), luôn grounded qua source thật
(đã curl trực tiếp glibc `malloc.c`, `sysconf.c`, `getpagesize.c`, và
`dlmalloc.c`) thay vì suy đoán từ trí nhớ.

## Đã học / đã xác nhận (grounded qua source thật)

1. **glibc's `mtrim()`** (trong `malloc.c`) là bản đầy đủ, madvise từng
   chunk giữa heap; **KHÔNG** được `free()` tự động gọi — chỉ `systrim`
   (top-only, qua sbrk âm) mới tự động, khi size vừa free
   `>= FASTBIN_CONSOLIDATION_THRESHOLD` VÀ topchunk
   `>= M_TRIM_THRESHOLD`. `mtrim()` đầy đủ chỉ chạy khi app tự gọi
   `malloc_trim(3)`.
2. **dlmalloc KHÔNG dùng `madvise` ở đâu cả** (grep xác nhận 0 kết quả).
   Cơ chế trả bộ nhớ giữa heap của nó (`release_unused_segments`) chỉ
   `munmap()` **nguyên một segment mmap riêng biệt** khi segment đó
   trống hoàn toàn — dựa vào kiến trúc multi-segment (nhiều vùng heap
   rời rạc) mà project của bạn không có. **Kết luận: học theo glibc's
   `mtrim`, không theo dlmalloc.**
3. `sysconf(_SC_PAGESIZE)` trên Linux/glibc **không phải syscall** — chỉ
   đọc biến toàn cục `GLRO(dl_pagesize)` được cache 1 lần lúc process
   khởi động (từ `AT_PAGESZ` trong auxv). Gọi nhiều lần không tốn kém.
4. Công thức round-up/round-down: `round_up = (x + mask) & ~mask`,
   `round_down = x & ~mask`, với `mask = page_size - 1` (chỉ đúng khi
   page_size là luỹ thừa 2). Đã đối chiếu 1-1 với `mtrim()` thật: glibc
   tính theo "trừ dần độ dài" (`size -= offset; size & ~psm1`), code của
   bạn tính theo "2 con trỏ start/end" — toán học tương đương.
5. Vì sao `madvise` phải gọi **riêng cho từng chunk trong bin**, không
   gộp: các chunk trong cùng bin không liền nhau về vật lý (chỉ liền về
   logic qua `list`), gộp madvise sẽ đụng vào chunk khác đang allocated
   ở giữa -> corruption.
6. Invariant "không 2 free chunk liền kề nhau trong heap" được đảm bảo
   bởi `coalesce()` gọi trước mọi `insert_*` trong `my_free`, và tính
   bắc cầu của `split()` (không tự coalesce nhưng an toàn vì chỉ cắt bên
   trong 1 chunk đã từng được coalesce).
7. `static` (file-scope) + không khai báo trong `.h` = ẩn hoàn toàn khỏi
   file `.c` khác (che giấu implementation detail, tiền lệ có sẵn:
   `ensure_state()` trong `debug.c`).

## Thiết kế đã chốt cho tính năng trim

- `trim_chunk(mblockptr *block)` — **private/static**, xử lý 1 chunk,
  trả về `size_t` byte thực sự đã madvise (khác glibc's boolean `result`
  — cố ý, để `my_malloc_trim` cộng dồn tổng byte chính xác cho mục đích
  logging/debug).
- `my_malloc_trim(void)` — **public API**, duyệt `gm.bins[i]` từ
  `i = get_bin_bucket(LINUX_PAGE)` trở lên (bỏ qua small bin, không thể
  chứa trọn 1 page — tương đương `psindex` trong glibc), gọi `trim_chunk`
  cho từng chunk, cộng dồn, trả tổng.
- **KHÔNG tự động gọi trong `my_free`** — đúng model glibc (`mtrim` là
  API riêng, không phải side-effect của free). Top-chunk shrink
  (`SHRINK_THRESHOLD` trong `my_free`) giữ nguyên, tách biệt, tương
  đương `systrim` tự động của glibc.

## Bug tìm thấy trong `my-malloc.c` bạn vừa upload — CẦN SỬA TRƯỚC KHI REFACTOR

1. **`get_bin_bucket` bị đổi tên thành `get_bin`** trong `my-malloc.c`,
   nhưng `debug.c` (dòng trong `check_bins()`) vẫn gọi `get_bin_bucket`,
   và `my-malloc.h` vẫn khai prototype cũ -> **lỗi linker** khi build
   bản `DEBUG`. Cần đồng bộ tên ở cả 3 chỗ.
2. **`insert_small_chunk`/`insert_large_chunk` bị thêm `static`** trong
   `.c`, nhưng `my-malloc.h` vẫn khai `non-static` -> **lỗi compile**
   ("static declaration follows non-static declaration"). Ngoài ra
   `split()` gọi 2 hàm này *trước* điểm định nghĩa trong file -> cần
   forward-declare nếu giữ static.
3. **Hàm trim public đặt tên `malloc_trim`** — trùng tên với hàm thật
   trong libc (`int malloc_trim(size_t)`), khác chữ ký
   (`size_t malloc_trim(void)`) -> nguy cơ conflicting-types hoặc link
   nhầm symbol. **Phải đổi thành `my_malloc_trim`.**
4. Biến `psm1` trong `malloc_trim()`/`trim_chunk` khai báo nhưng không
   dùng -> warning `unused variable`, nên xoá hoặc dùng làm bộ lọc sớm
   giống glibc.

## Việc đã xác nhận HOÀN THÀNH từ phiên trước (không cần làm lại)

- `split()` giờ đã gọi `insert_small_chunk`/`insert_large_chunk` thay vì
  `list_add_after` thô — khớp yêu cầu insert-sorted đã đề ra.
- Khối dead code sbrk-extend trong `my_realloc`
  (`if (next_block == gm.heap_end)`) đã bị xoá — không còn thấy trong
  file hiện tại.

## Kiến trúc refactor đang bàn dở (CHƯA áp dụng vào code thật)

Mục tiêu người dùng: "dự án đẹp, đầy đủ API + hàm test nội bộ".

```
my-malloc/
├── include/
│   └── my-malloc.h       <-- CHỈ public API: my_malloc, my_free,
│                              my_realloc, my_calloc, my_malloc_trim
├── src/
│   ├── internal.h         <-- MỚI, chưa tạo: get_bin_bucket,
│   │                          debug_get_state, struct mblockptr,
│   │                          malloc_state... debug.c VÀ my-malloc.c
│   │                          đều include file này thay vì include
│   │                          lẫn nhau
│   ├── my-malloc.c
│   ├── debug.c / debug.h
│   └── list.h
├── tests/                 <-- chưa tạo
│   ├── test_coalesce.c    (test hàm static, dùng #include "../src/my-malloc.c"
│   │                       hoặc macro STATIC/static để "mở khoá" linkage)
│   └── test_public_api.c  (black-box, chỉ include my-malloc.h, giống
│                            triết lý check_heap/check_bins hiện tại)
└── Makefile                <-- chưa dựng, build lib + build test riêng
```

Quy tắc 3 câu hỏi để quyết định static/public đã chốt:
1. Hàm có phải "hợp đồng" người dùng thư viện cần gọi? -> `.h` public, không static.
2. Hàm có bị gọi chéo giữa nhiều file `.c` trong CHÍNH project (không
   phải người dùng ngoài)? -> khai trong `internal.h`, không static
   (bắt buộc kỹ thuật, coi như "package-private").
3. Mặc định -> `static`, không khai ở đâu ngoài file định nghĩa.

Áp dụng: `coalesce`, `split`, `try_expand`, `find_suitable_block`,
`insert_small_chunk`, `insert_large_chunk`, `trim_chunk` -> static.
`get_bin_bucket`, `debug_get_state` -> `internal.h`, không static (vì
`debug.c` cần gọi).

## Việc tiếp theo gợi ý (theo thứ tự)

1. Sửa 4 bug ở trên trong `my-malloc.c`/`my-malloc.h`/`debug.c` — verify
   lại bằng cách build thử (chưa có Makefile, có thể cần dựng tạm 1
   lệnh gcc thủ công để build + link `DEBUG` version trước).
2. Dựng `internal.h`, tách `include/` vs `src/`, di chuyển
   `get_bin_bucket`/`debug_get_state` vào đó.
3. Quyết định kỹ thuật test cho hàm static: `#include` file `.c` thẳng
   vào file test, hay macro `STATIC`/`static` bật tắt qua
   `#ifdef UNIT_TEST`. Chưa chốt — đã trình bày trade-off cho người
   dùng tự chọn, người dùng chưa trả lời.
4. Dựng `Makefile` build lib + test riêng.
5. Viết test case cho `trim_chunk` dùng số ví dụ cụ thể đã tính tay
   trong phiên này (payload=4096, page=4096, kiểm cả case
   payload_start đã align sẵn lẫn chưa align) để verify công thức
   round-up/round-down.