# Custom Memory Allocator (C)

A simple **arena-based** heap allocator with 8-byte alignment, first-fit placement, block splitting, basic coalescing, and `realloc` support.  
Memory comes directly from the OS via **`mmap` (POSIX)** or **`VirtualAlloc` (Windows)**. Arenas are page-aligned and allocated in chunks of **≥ 1 MiB** to reduce system calls.

## Build
```bash
make         # builds app (demo) and bench (stress/benchmark)
./app        # small usage demo / sanity checks
./bench      # randomized stress and timing runs
```

## Design
- **Arena header** – per-arena metadata  
  `{ size, next, prev, first_block }`
- **Block header** – doubly linked list of blocks  
  `{ size, free, next, prev }`
- **Placement** – **first-fit** scan across blocks
- **Growth** – request ≥ 1 MiB from the OS (`mmap`/`VirtualAlloc`), rounded up to page size
- **Split** – oversize free blocks are split to reduce internal fragmentation
- **Coalesce** – adjacent free blocks are merged on `free`
- **Realloc** – in-place grow if the **next** block is free and large enough; otherwise **allocate + copy** (contents preserved), then free the old block
- **Alignment** – payloads are aligned to **8 bytes** (headers are padded accordingly)

## Files
- `include/jmalloc.h` – public API
- `src/jmalloc.c` – allocator implementation (OS abstraction + arenas + block manager)
- `src/main.c` – short demo / smoke tests
- `tests/bench.c` – randomized stress + microbench

## Notes & Limitations
- Single-threaded (no locks).  
- First-fit over a single free list (no segregated bins yet).  
- Coalescing is eager with adjacent neighbors; `realloc` currently prefers merging forward (with `next`).  
- Designed for learning and experimentation—not a drop-in production `malloc` replacement.

## Roadmap (Nice-to-Have)
- Best-fit or **segregated free lists** to cut scan time
- **Per-thread arenas** to reduce lock contention (when adding thread safety)
- Tunable large-allocation policy / `mmap` threshold
- Guard regions / canaries for overrun detection
- Thread safety via `pthread_mutex_t`
- Unit tests and CI (e.g., CTest/GitHub Actions)
- Optional 16-byte alignment on x86-64 ABI

## Platform Support
- **Linux/macOS**: `mmap`, `munmap`, `sysconf(_SC_PAGESIZE)`
- **Windows**: `VirtualAlloc`, `VirtualFree`, `GetSystemInfo`

> Educational project for systems programming practice.
