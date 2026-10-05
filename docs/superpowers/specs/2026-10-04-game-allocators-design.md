# Game allocators

## Goal

One `lotus` allocator interface for game code: a bump allocator, a stack allocator, a fixed-size pool, and a fixed-budget general heap. `src/core/allocator.hpp` stays untouched.

## Rules

- Single-threaded. The caller synchronizes.
- Each owning allocator takes one `malloc` block in its constructor and throws `std::bad_alloc` if that block cannot be created or the requested size overflows once overhead is added.
- `HeapAllocator(0)` and `PoolAllocator<T>(0)` are valid empty allocators. `alloc` returns `nullptr`.
- `alloc` returns `nullptr` when the request does not fit. It does not throw.
- Copy and move are deleted on every type that owns a buffer.
- `construct<T>` returns `nullptr` when `alloc` fails and does not placement-new. If `T`'s constructor throws, the memory stays allocated and the exception propagates.
- `free`, `realloc`, `pop`, and `reset` are concrete methods on the types that support them.
- `reset` and `pop` do not run destructors.
- Range, magic, and double-free checks run in every build. On failure the allocator is unchanged. `assert` fires when assertions are enabled.
- Debug fills are `0xCD` on allocate and `0xDD` on free. They are compiled out when `NDEBUG` is set.

## Types

| Type | Header | Role |
|---|---|---|
| `Allocator` | `include/lotus/allocator.hpp` | Virtual `alloc`, `remaining`, `allocated`, and null-safe `construct`. |
| `RegionAllocator` | `include/lotus/region_allocator.hpp` | Fixed region, bump cursor, virtual `reset`. |
| `LinearAllocator` | `include/lotus/linear_allocator.hpp` | Bump only. |
| `StackAllocator<Max>` | `include/lotus/stack_allocator.hpp` | Bump plus one LIFO marker per allocation that moves the cursor. |
| `PoolAllocator<T>` | `include/lotus/pool_allocator.hpp` | Fixed count of `T` slots. One object per `alloc`. |
| `HeapAllocator` | `include/lotus/heap_allocator.hpp` | Variable sizes freed in any order, inside one fixed block. |

`include/lotus/align.hpp` provides `is_pow2` and `align_up`.

Alignment passed to `alloc` must be a non-zero power of two. Otherwise `alloc` returns `nullptr`.

## Linear and stack

`reset()` on `RegionAllocator` is virtual. `StackAllocator::reset()` also sets the marker count to zero.

`alloc(0, alignment)` with a legal alignment returns a non-null pointer. A stack marker is stored only when padding moves the cursor. `pop()` on an empty stack does nothing. `pop()` rewinds one marker. Running out of markers returns `nullptr` and leaves the cursor unchanged.

`allocated() + remaining()` equals `capacity()` for both region allocators.

## Pool

`PoolAllocator<T>(count)` allocates `count` slots. The stride is `align_up(max(sizeof(T), sizeof(void*)), max(alignof(T), alignof(void*)))`. The base is aligned to that alignment. A side bitmap marks live slots. Free slots form an intrusive free list.

- `T* alloc()` returns one uninitialized slot, or `nullptr` when the pool is empty.
- `void* alloc(bytes, alignment)` returns a slot only when `bytes` is non-zero, `bytes <= sizeof(T)`, and `alignment <= alignof(T)`.
- `free(nullptr)` does nothing. `free` does not run a destructor.
- `destroy(T*)` runs the destructor once, then frees. It checks that the slot is live before the destructor runs.
- `capacity()` is the slot count. `allocated() + remaining()` equals `capacity() * stride()`.

## Heap

`HeapAllocator(size)` can satisfy one fresh `alloc(size, 16)` when `size > 0`. `capacity()` returns `size` rounded up to 16. Bookkeeping is allocated in addition to that payload.

Block header, 32 bytes, always at the physical start of the block:

- `size_and_flags`: physical size, a multiple of 16. Bit 0 set means allocated.
- `prev_size`: physical size of the previous block.
- `user_size`: bytes the caller requested.
- `magic`: constant checked on `free` and `realloc`.

The `size_t` immediately before the returned pointer stores the byte distance back to the header. Sentinels at both ends of the region stay allocated so coalescing has no edge case.

Free blocks sit in size-class bins. Classes step by 16 bytes from the 48-byte minimum block up to 512, then four classes per power of two. A bitset records non-empty bins. Search starts at the class that contains the worst-case physical size. If the head block of that class is too small, search continues at the next non-empty class, whose floor is large enough.

`free(nullptr)` does nothing. Adjacent free neighbors coalesce.

`realloc(nullptr, bytes, alignment)` is `alloc`. `realloc(ptr, 0, alignment)` frees `ptr` and returns `nullptr`. A grow keeps the pointer when the pointer already satisfies the alignment and the next physical block can be absorbed. Otherwise it allocates, copies `user_size` bytes, and frees the old block. If that allocation fails, `realloc` returns `nullptr` and the old block stays. `alloc(0, alignment)` returns `nullptr`.

`allocated()` is the sum of outstanding requested sizes. `remaining()` is the sum of free payload (`physical size - header`). Those two do not add up to `capacity()`.

`reset()` rebuilds one free block and does not run destructors.

## Tests

Existing stack tests stay. `PopDeallocatesLast` requires one `pop()` to release only the last allocation. Added stack cases: `reset()` restores the marker capacity, including through a `RegionAllocator&`; a second `pop()` restores the first block; `alloc(0)` does not consume a marker when the cursor is already aligned.

- `test/linear_allocator_test.cpp`: alignments 1 through 32, rejected non-powers of two, exhaustion, `reset`, `construct` returning `nullptr` when full, `allocated() + remaining() == capacity()`.
- `test/pool_allocator_test.cpp`: reuse, exhaustion, alignment, a two-object request returning `nullptr`, double-free, a foreign pointer, `destroy` running a destructor once, the byte-count equation.
- `test/heap_allocator_test.cpp`: alignment, any-order free then one allocation of `capacity()`, in-place and moving `realloc`, a failed `realloc` leaving the old bytes in place, `reset()`, empty heap, and a shuffled mix of sizes that coalesces back to one full-size allocation.
