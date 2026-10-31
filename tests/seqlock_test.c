/**
 * Unit test for core/common/seqlock.h (§18). Single-threaded: it checks
 * the state machine (even -> odd -> even, retry detection) that the real
 * concurrency property rests on, rather than racing real threads - there
 * is no threading in the codebase yet (the guest thread scheduler is
 * Phase 2, §7), so a genuine multi-threaded stress test belongs there,
 * over the same primitive.
 */
#include "common/seqlock.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                   \
  do                                                                   \
  {                                                                    \
    if (!(cond))                                                       \
    {                                                                  \
      fprintf(stderr, "[seqlock_test] FAIL %s:%d: %s\n", __FILE__,     \
              __LINE__, #cond);                                        \
      exit(1);                                                         \
    }                                                                   \
  } while (0)

int main(void)
{
  atomic_uint seq;
  atomic_init(&seq, 0);

  /* Fresh counter: even, no writer in flight. */
  uint32_t begin = seqlock_read_begin(&seq);
  CHECK(begin == 0);
  CHECK(!seqlock_read_retry(&seq, begin));

  /* A full write cycle moves the counter even -> odd -> even. */
  const uint32_t odd = seqlock_write_begin(&seq);
  CHECK(odd == 1);
  CHECK(atomic_load_explicit(&seq, memory_order_relaxed) == 1);
  /* A read started while the writer is mid-update must retry: the odd
   * value alone is enough, before the counter ever changes again. */
  CHECK(seqlock_read_retry(&seq, odd));
  seqlock_write_end(&seq, odd);
  CHECK(atomic_load_explicit(&seq, memory_order_relaxed) == 2);

  /* A read whose begin/end straddle that write cycle must retry too. */
  CHECK(seqlock_read_retry(&seq, 0));

  /* A read entirely after the cycle sees a stable even counter and does
   * not retry. */
  begin = seqlock_read_begin(&seq);
  CHECK(begin == 2);
  CHECK(!seqlock_read_retry(&seq, begin));

  /* Sequence counters keep climbing across repeated writes - never reset
   * to 0, never left odd. */
  for (int i = 0; i < 10; i++)
  {
    const uint32_t o = seqlock_write_begin(&seq);
    CHECK((o & 1u) == 1u);
    seqlock_write_end(&seq, o);
    CHECK((atomic_load_explicit(&seq, memory_order_relaxed) & 1u) == 0u);
  }
  CHECK(atomic_load_explicit(&seq, memory_order_relaxed) == 22);

  /* Regression: uninitialized backing memory is not guaranteed even on
   * native builds (common/arena is a plain malloc(); only Emscripten's
   * WebAssembly.Memory arrives pre-zeroed, per layout.h) - MSVC's debug
   * heap in particular fills fresh allocations with 0xCD, odd as a u32.
   * A write_begin that only flipped parity relative to the existing
   * value would land on an even "odd" marker here and the counter would
   * never recover, livelocking every reader forever. */
  atomic_uint dirty;
  atomic_init(&dirty, 0xCDCDCDCDu);
  CHECK((atomic_load_explicit(&dirty, memory_order_relaxed) & 1u) == 1u);
  const uint32_t first_odd = seqlock_write_begin(&dirty);
  CHECK((first_odd & 1u) == 1u);
  seqlock_write_end(&dirty, first_odd);
  CHECK((atomic_load_explicit(&dirty, memory_order_relaxed) & 1u) == 0u);
  /* And the next cycle behaves exactly like the already-even case above. */
  const uint32_t second_odd = seqlock_write_begin(&dirty);
  CHECK((second_odd & 1u) == 1u);
  seqlock_write_end(&dirty, second_odd);
  CHECK((atomic_load_explicit(&dirty, memory_order_relaxed) & 1u) == 0u);

  printf("[seqlock_test] OK\n");
  return 0;
}
