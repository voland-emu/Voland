/**
 * Generic seqlock primitive for host-side shared regions in linear memory
 * (§4/§6/§18): torn-read-free synchronization for a single writer and any
 * number of readers, no blocking, no allocation. input_region.{h,c} uses
 * it today; any future single-writer shared region (output region,
 * control region) can reuse it instead of reinventing the retry loop.
 *
 * Protocol (§18): the counter starts even. A writer bumps it odd
 * (release), writes the payload, bumps it even (release) again. A reader
 * loads the counter (acquire); odd means a writer is mid-update, retry.
 * Otherwise it reads the payload, then loads the counter again
 * (acquire): changed means the payload may be torn, retry. Exactly one
 * writer per region, by convention - this is not a mutual-exclusion
 * primitive and gives undefined results under concurrent writers.
 *
 * MSVC: C11 <stdatomic.h> requires /experimental:c11atomics on this
 * project's toolset (verified against MSVC 19.51/toolset 14.51: an
 * unguarded #include fails hard with "C atomic support is not enabled"
 * otherwise). CMakeLists.txt adds that flag under MSVC in the same PR
 * that lands this header - the Phase 0 SWITCH_PRINTF_FORMAT pattern, one
 * level up (a compiler flag instead of a macro guard, since there is no
 * portable spelling to fall back to: GCC/Clang already support
 * <stdatomic.h> unconditionally).
 *
 * Starting parity is NOT assumed to be even. `common/arena`'s backing
 * memory is a plain malloc() on native (layout.h: only Emscripten's
 * WebAssembly.Memory arrives pre-zeroed), and MSVC's debug heap fills
 * fresh allocations with the byte 0xCD - odd, as a u32. A seqlock whose
 * first write just does current+1 would land on an EVEN value from that
 * odd garbage and stay stuck "mid-write" forever, livelocking every
 * reader. seqlock_write_begin forces the published value odd regardless
 * of what was there before, so the very first write self-corrects the
 * parity in one step; every write after that sees a properly-even
 * `current` and behaves exactly as the even/odd alternation above
 * describes. Caught by input_region_test under the native-noop preset's
 * Debug/MSVC build (§25 Phase 1 input region task) - release builds and
 * non-debug-heap allocators could easily have hidden it.
 *
 * Payload accesses must go through SEQLOCK_RELAXED_STORE/_LOAD below, not
 * memcpy or a plain dereference. The sequence counter's release/acquire
 * ordering gives a *successful* (non-retried) read a real happens-before
 * edge to the write it observed - but the C11 and WASM memory models
 * still classify the read a writer's in-flight, about-to-be-discarded
 * update races against as a data race on that payload byte, full stop,
 * independent of whether the retry check later notices and throws the
 * result away. ThreadSanitizer enforces exactly this and will flag a
 * plain (non-atomic) payload access even though the algorithm is
 * correct - this is the long-standing, still-unresolved "seqlock
 * problem" raised repeatedly in WG14/WG21: every real-world seqlock,
 * including the Linux kernel's, has this same formal gap and relies on
 * no shipped compiler tearing or eliminating a naturally-aligned
 * machine-word-or-smaller access. Closing it costs nothing: a relaxed
 * atomic load/store of a naturally-aligned word is the same single
 * instruction a plain load/store would be on every target this project
 * compiles for (x86, ARM, every WASM engine), so there is no reason to
 * accept the "industry standard, but technically UB" status quo instead.
 * Caught by `tests/seqlock_test.c`'s real-thread stress test under
 * ThreadSanitizer (Clang, WSL) before it ever reached input_region.c.
 */
#ifndef SWITCH_COMMON_SEQLOCK_H
#define SWITCH_COMMON_SEQLOCK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Begins a write: returns the odd sequence value just published. Follow
 * with the payload write, then seqlock_write_end(seq, that value).
 * `| 1u` (not just `+ 1u`) guarantees the published value is odd even if
 * `seq` started odd itself - uninitialized backing memory on a first
 * write, see the header comment - rather than only flipping parity
 * relative to whatever was already there. */
static inline uint32_t seqlock_write_begin(volatile atomic_uint *seq)
{
  uint32_t odd = (atomic_load_explicit(seq, memory_order_relaxed) + 1u) | 1u;
  atomic_store_explicit(seq, odd, memory_order_release);
  return odd;
}

/* Ends a write begun with seqlock_write_begin: publishes the even value,
 * making the just-written payload visible to readers. */
static inline void seqlock_write_end(volatile atomic_uint *seq, uint32_t odd)
{
  atomic_store_explicit(seq, odd + 1u, memory_order_release);
}

/* Starts a read attempt; pass the result to seqlock_read_retry after
 * reading the payload. */
static inline uint32_t seqlock_read_begin(const volatile atomic_uint *seq)
{
  return atomic_load_explicit(seq, memory_order_acquire);
}

/* True if the payload just read between seqlock_read_begin and this call
 * may be torn and must be retried: `begin` was odd, or the counter has
 * since changed. */
static inline bool seqlock_read_retry(const volatile atomic_uint *seq, uint32_t begin)
{
  return (begin & 1u) != 0u || atomic_load_explicit(seq, memory_order_acquire) != begin;
}

/* Relaxed-atomic payload accessors (see the header comment above for
 * why plain memcpy/dereference is not enough). `T` must be a type whose
 * atomic form is lock-free on every target this project compiles for -
 * `uint32_t`, `int16_t` and `float` all qualify (verified against MSVC's
 * /experimental:c11atomics and GCC/Clang's native support) - and `addr`
 * must already be aligned to `T`'s natural alignment, same as any other
 * atomic access. These only need memory_order_relaxed: the sequence
 * counter above is what establishes ordering with the rest of the
 * payload; the payload fields have no ordering relationship with each
 * other. */
#define SEQLOCK_RELAXED_STORE(T, addr, value) \
  atomic_store_explicit((volatile _Atomic(T) *)(void *)(addr), (T)(value), memory_order_relaxed)
#define SEQLOCK_RELAXED_LOAD(T, addr) \
  atomic_load_explicit((const volatile _Atomic(T) *)(const void *)(addr), memory_order_relaxed)

#endif /* SWITCH_COMMON_SEQLOCK_H */
