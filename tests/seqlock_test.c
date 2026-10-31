/**
 * Unit test for core/common/seqlock.h (§18). Most of this file is
 * single-threaded: it checks the state machine (even -> odd -> even,
 * retry detection) that the real concurrency property rests on.
 * test_concurrent_writer_reader() at the bottom races a real writer
 * thread against the main thread reading, since a seqlock whose only
 * coverage is single-threaded hasn't actually tested the property it
 * exists for. There is no threading in the production codebase yet (the
 * guest thread scheduler is Phase 2, §7) - this test creates its own
 * thread via the platform's native API (Win32 / pthreads) rather than
 * pulling in a scheduling dependency for one test.
 */
#include "common/seqlock.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

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

/* A seqlock-protected triple that must always read back mutually equal -
 * the property a torn read would violate. a/b/c are plain uint32_t
 * storage accessed only through SEQLOCK_RELAXED_STORE/_LOAD (not
 * atomic_uint directly - that would make them individually atomic in a
 * way that isn't the point here: the point is exercising the *payload*
 * accessors seqlock-protected regions must use, same as input_region.c). */
typedef struct Stress_Region
{
  atomic_uint seq;
  uint32_t    a, b, c;
  atomic_int  done; /* set once the reader has run enough iterations */
} Stress_Region;

static void stress_write(Stress_Region *region, uint32_t value)
{
  const uint32_t odd = seqlock_write_begin(&region->seq);
  SEQLOCK_RELAXED_STORE(uint32_t, &region->a, value);
  SEQLOCK_RELAXED_STORE(uint32_t, &region->b, value);
  SEQLOCK_RELAXED_STORE(uint32_t, &region->c, value);
  seqlock_write_end(&region->seq, odd);
}

/* Returns 0 on a torn read (a == b == c violated), never on a clean one -
 * the loop above must have retried instead of returning one. */
static int stress_read_is_consistent(Stress_Region *region)
{
  uint32_t begin, a, b, c;
  do
  {
    begin = seqlock_read_begin(&region->seq);
    a = SEQLOCK_RELAXED_LOAD(uint32_t, &region->a);
    b = SEQLOCK_RELAXED_LOAD(uint32_t, &region->b);
    c = SEQLOCK_RELAXED_LOAD(uint32_t, &region->c);
  } while (seqlock_read_retry(&region->seq, begin));
  return a == b && b == c;
}

#if defined(_WIN32)
static DWORD WINAPI stress_writer_thread(LPVOID arg)
{
  Stress_Region *region = (Stress_Region *)arg;
  uint32_t value = 0;
  while (!atomic_load_explicit(&region->done, memory_order_relaxed))
    stress_write(region, value++);
  return 0;
}
#else
static void *stress_writer_thread(void *arg)
{
  Stress_Region *region = (Stress_Region *)arg;
  uint32_t value = 0;
  while (!atomic_load_explicit(&region->done, memory_order_relaxed))
    stress_write(region, value++);
  return NULL;
}
#endif

#define STRESS_READER_ITERATIONS 500000u

/* Races a real writer thread (spinning as fast as it can) against the
 * main thread reading, for half a million reads. Any torn read fails
 * loudly via CHECK on the spot - there is no "mostly passes". */
static void test_concurrent_writer_reader(void)
{
  Stress_Region region;
  atomic_init(&region.seq, 0);
  region.a = region.b = region.c = 0;
  atomic_init(&region.done, 0);

#if defined(_WIN32)
  HANDLE writer = CreateThread(NULL, 0, stress_writer_thread, &region, 0, NULL);
  CHECK(writer != NULL);
#else
  pthread_t writer;
  CHECK(pthread_create(&writer, NULL, stress_writer_thread, &region) == 0);
#endif

  for (uint32_t i = 0; i < STRESS_READER_ITERATIONS; i++)
    CHECK(stress_read_is_consistent(&region));

  atomic_store_explicit(&region.done, 1, memory_order_relaxed);

#if defined(_WIN32)
  WaitForSingleObject(writer, INFINITE);
  CloseHandle(writer);
#else
  CHECK(pthread_join(writer, NULL) == 0);
#endif
}

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

  test_concurrent_writer_reader();

  printf("[seqlock_test] OK\n");
  return 0;
}
