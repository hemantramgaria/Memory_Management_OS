# Memory Management OS Assignment

✅ This repository contains a compact custom memory allocator implementation intended for learning and demonstration in an Operating Systems or Memory Management assignment. The primary implementation is in `2022MT11854mmu.h` and offers several allocation strategies.

---

## Purpose and Goals

- Demonstrate different memory allocation strategies (first-fit, next-fit, best-fit, worst-fit, and buddy allocator) in a single, compact implementation.
- Provide both a free-list allocator (arena based) with optional AVL tree indexing (for best/worst fit) and a buddy allocator implementation.
- Illustrate block splitting, coalescing, and basic memory book-keeping header carried per block.

---

## High level architecture

There are two main allocator implementations contained in the same single-file header:

1. Arena-based free-list allocator (Non-buddy):
   - Global arena mmap'd with fixed `ARENA_SIZE` bytes.
   - Each block has a `header_t` (size, free flag, magic value, pointers to physical prev/next, and free-list prev/next).
   - A global free list (doubly-linked) references free blocks.
   - Allocation algorithms supported by this allocator:
     - `malloc_first_fit` — iterate free-list from head and pick the first fit.
     - `malloc_next_fit` — maintain a `nextfit_cur` pointer, resume search from that position.
     - `malloc_best_fit` / `malloc_worst_fit` — use an AVL index keyed by block sizes (and pointer as a tiebreaker), providing efficient best/worst fit candidate lookup.
   - Block splitting: large free blocks are split into a requested payload and a smaller free block header if space allows.
   - Coalescing: on `my_free` free blocks are coalesced (merge with adjacent free blocks by `prev_phys` / `next_phys`).

2. Buddy allocator:
   - A separate arena mmap'd for buddy usage.
   - Blocks have a `buddy_header` with `order`, `free`, and linked-list pointers for free lists per order.
   - Uses free lists per order (`buddy_free_lists`) where each order holds blocks of size MIN_BLOCK << order.
   - Allocation finds the minimal order for requested size + header and splits higher-order blocks until required order.
   - Freeing merges a block with its buddy if possible — resulting in coalesced larger blocks.

Shared definitions and constants:
- `ARENA_SIZE` — default arena size, 4096.
- `MAGIC` — a magic constant used in headers to detect corruption or invalid frees.
- `ALIGN8(x)` — alignment macro aligning requested sizes to 8 bytes.

---

## Key data structures

- `header_t`: Non-buddy block metadata used in the free-list allocator.
- `avl_node_t`: Index nodes used by the AVL tree (best/worst fit) referencing `header_t` blocks.
- `buddy_hdr_t`: Header used by the buddy allocator for each block optimized for constant-time merges and splitting.

---

## Important functions (public API)

- `void* malloc_first_fit(size_t size);`
- `void* malloc_next_fit(size_t size);`
- `void* malloc_best_fit(size_t size);`
- `void* malloc_worst_fit(size_t size);`
- `void* malloc_buddy_alloc(size_t size);`
- `void my_free(void* ptr);`

Notes:
- All allocation functions map to different strategies. Use the corresponding `malloc_*` to allocate memory using the desired strategy.
- `my_free` is the free implementation; it knows whether the last used strategy was buddy to use the correct free path.

---

## How to use (general guidelines)

- Include the header in a C source file and call the allocation functions as you would normally call `malloc` variants. Example:

  ```c
  #include "2022MT11854mmu.h"

  int main() {
      int* p = (int*) malloc_first_fit(sizeof(int)*10); // allocate with first-fit
      // use memory
      my_free(p); // free it
      return 0;
  }
  ```

- To experiment with different strategies, call a different `malloc_*` function for each allocation.
- Always free using `my_free` (not the standard `free`).
- `malloc_buddy_alloc` uses the buddy allocator arena; when used, subsequent calls to allocation may set the global allocator mode to buddy, and `my_free` will route to buddy-specific free.

---

## Build / Test (general instructions)

- This header defines and implements the functions; compile a driver `.c` file that includes this header.
- Example commands (POSIX environment; Linux/macOS):

  ```bash
  gcc -Wall -g -o test test.c
  ./test
  ```

- `test.c` example should include `2022MT11854mmu.h` and perform a sequence of allocations and frees using the different functions. Use logging (printf) to verify behavior.

---

## Debugging and Observability

- Header contains `MAGIC` values used to sanity-check headers (invalid pointer or double frees).
- There are stderr prints for some failure cases (AVL pool exhaustion, double free detection, etc.) that can help during testing.
- You can add additional printf statements in the header if you need extra trace output.

---

## Limitations & Notes

- This is a teaching/demonstration allocator and is **not** production-ready:
  - Not thread-safe (no locks are used).
  - Single, fixed-size arenas (`ARENA_SIZE`) for both implementations; no arena resizing.
  - No `realloc` or `calloc` implementations are included.
  - `avl_pool` is a small fixed pool (256 nodes); allocation of a large number of free blocks may exhaust it.
  - Buddy allocator parameters are fixed (`BUDDY_MIN_BLOCK` and `BUDDY_MAX_ORDER`) and tuned to the defined arena size.

---

## Suggestions for extension

- Add `realloc` and `calloc` wrapper functions.
- Support multiple or resizable arenas to emulate larger heaps.
- Improve AVL node management (dynamic allocation rather than a fixed pool) and thread safety.
- Add instrumentation to track fragmentation, utilization, and average search costs.

---

## Contact / Notes

- This is meant to accompany a university assignment; adjust constants and behaviors to match assignment constraints.
- Consider moving implementation code to a `.c` file and having a cleaner header that exposes just the API for production use.

---

💡 Tip: Start by using small test programs to try each strategy in isolation, then compare fragmentation and allocation speed to explore trade-offs.
