# Context: my-malloc — realloc & mmap branch

Tóm tắt phiên làm việc, dùng để tiếp tục ở session sau.

## Đã sửa xong

1. **Nhánh mmap trong `my_realloc` — invariant `payload` sai sau `mremap`**
   - Bug: `nb->payload = request_size;` (không làm tròn page) → phá vỡ bất biến
     "payload luôn khớp kích thước mmap thật đã page-round", gây lệch `old_size`
     ở lần `mremap`/`munmap` tiếp theo.
   - Fix: tính `total_page_up` (làm tròn theo `LINUX_PAGE`) và gán
     `nb->payload = total_page_up - HEADER_SIZE - FOOTER_SIZE;` — giống hệt
     công thức đã dùng trong `my_malloc`.

2. **Nhánh mmap khi `mremap` fail — thiếu fallback**
   - Bug cũ: `return NULL;` ngay khi fail, không thử `malloc-copy-free` như
     glibc thật làm (đã grounded qua `_int_realloc`).
   - Fix: không `return`, để rơi tự nhiên xuống đoạn `malloc-copy-free` chung
     cuối hàm `my_realloc`. Đã dọn sạch một đoạn `if` chết (dead code) từng
     xuất hiện khi sửa nửa vời.

3. **`insert_large_chunk` cần sorted-descending** (đổi thiết kế bin sang best-fit)
   → `split()` đang chèn remainder bằng `list_add_after` thô, phá vỡ sort
   invariant. **Fix đề xuất**: thay bằng gọi `insert_small_chunk`/
   `insert_large_chunk` tùy theo size, giống cách `my_free` đang làm.
   *(Chưa xác nhận đã áp dụng vào file thật hay chưa — cần kiểm tra lại.)*

4. **Nhánh sbrk-thẳng trong `my_realloc` (`if (next_block == gm.heap_end)`)**
   - Bug: điều kiện gần như không bao giờ đúng, vì luôn tồn tại header của
     top chunk xen giữa block cuối và `heap_end` (theo đúng invariant
     `check_top_chunk`). Nếu chạy được, code tự `sbrk()` tay và không cập
     nhật `gm.topchunkptr`/`gm.topsize` → phá invariant ngay.
   - Fix: **xóa hẳn khối này**. Case nó nhắm tới (current_block kề top
     chunk) đã được `try_expand()` xử lý đúng (nhánh `next == gm.topchunkptr`).

## Đang mở / chưa làm

- **`try_expand` giữ nguyên vị trí gọi hiện tại** trong `my_realloc` (không
  cần di chuyển) — chỉ cần xóa khối dead code ở trên.
- **Guard `request_size < MMAP_THRESHOLD`** trước `try_expand` và sbrk-extend:
  **giữ nguyên, không phải dư thừa.** Lý do: thiết kế hiện tại buộc bất biến
  "size lớn ⇒ phải là mmap chunk" vì logic shrink-to-OS trong `my_free` chỉ
  xử lý top chunk, chưa có cơ chế trim chunk giữa heap. Nếu bỏ guard mà chưa
  có cơ chế trim tổng quát, sẽ tạo ra chunk to không có `MMAP_BIT` kẹt vĩnh
  viễn trong bin, không bao giờ trả được cho OS.

- **Thiết kế "trim tốt hơn" cho chunk giữa heap** (chủ đề đang dang dở):
  - Sự thật nền tảng: `sbrk`/`brk` **chỉ di chuyển được một điểm ở cuối heap**
    — không thể trả bộ nhớ cho một chunk nằm giữa heap bằng sbrk, bất kể to
    cỡ nào.
  - Hướng đúng (giống glibc/nhiều allocator khác): dùng
    `madvise(addr, len, MADV_DONTNEED)` lên các trang vật lý *bên trong*
    payload của free chunk — **không đổi gì về mặt logic** (chunk vẫn nằm
    nguyên trong bin, size không đổi), chỉ nhả RAM vật lý; chạm lại sau này
    kernel tự cấp trang mới.
  - Bẫy quan trọng: phải **round-in** địa chỉ (`ceil` cho start, `floor` cho
    end) theo page, tuyệt đối không round-out — nếu không sẽ vô tình
    `MADV_DONTNEED` đè lên header/footer của chính chunk hoặc chunk kế bên,
    gây corruption khi kernel zero-fill lại trang đó.
  - Câu hỏi chưa chốt: gọi trim ngay trong `my_free` mỗi lần chunk đủ to được
    free (đơn giản, tốn syscall mỗi lần), hay quét định kỳ theo ngưỡng tổng
    free memory (giống `malloc_trim`, cần thêm state theo dõi)?

## Việc tiếp theo gợi ý

1. Xác nhận lại `split()` đã áp dụng fix insert-sorted hay chưa (mục 3).
2. Xóa khối dead code sbrk-extend (mục 4) trong file thật.
3. Quyết định thời điểm gọi trim (câu hỏi cuối cùng ở trên) rồi bắt tay viết
   `madvise`-based trim cho free chunk giữa heap.