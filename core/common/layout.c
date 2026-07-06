#include "common/layout.h"
#include "common/arena.h"
#include "common/log.h"

#include <stdint.h>
#include <string.h>

/* arena_create()/arena_allocate() take size_t; LAYOUT_GUEST_RAM_SIZE (4GB)
 * silently truncates to 0 on an implicit uint64_t->size_t conversion if
 * size_t is ever 32-bit, which would corrupt every region below it rather
 * than fail loudly. Every native target this project ships to (desktop,
 * iOS, macOS, Android, tvOS, visionOS) is 64-bit-only, and Emscripten's
 * size_t is 64-bit under -sMEMORY64=1 (§4) - so this is a real assumption,
 * not a hypothetical one, and it belongs here as a hard compile error. */
_Static_assert(sizeof(size_t) >= 8,
               "layout.c requires a 64-bit size_t (guest RAM alone is 4GB); "
               "32-bit targets are not supported");

/* Guest RAM on native builds goes through a raw virtual-memory reservation
 * rather than the small-regions arena: it is the one region large enough
 * (4GB) that eagerly touching every byte at boot would matter, and an
 * anonymous OS mapping is already guaranteed zero-filled with physical
 * pages committed lazily on first touch - matching "arena-carved,
 * page-aligned" (§4) far more closely than a heap allocation would.
 *
 * Under Emscripten there is no separate reserve-vs-commit distinction to
 * exploit: the browser already allocated and zero-filled the whole
 * WebAssembly.Memory at instantiation (§4), so guest RAM is arena-carved
 * like every other region instead (see layout_create()). */
#if defined(__EMSCRIPTEN__)
/* No native reservation on this path; see layout_create(). */
#elif defined(_WIN32)
#include <windows.h>
static void* guest_ram_reserve(uint64_t size) {
  return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
static void guest_ram_release(void* p, uint64_t size) {
  (void)size;
  if (p) VirtualFree(p, 0, MEM_RELEASE);
}
#else
#include <sys/mman.h>
static void* guest_ram_reserve(uint64_t size) {
  void* p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}
static void guest_ram_release(void* p, uint64_t size) {
  if (p) munmap(p, (size_t)size);
}
#endif

typedef struct Layout_Storage {
  Arena arena;      /* backs every region except guest RAM on native;
                      * backs guest RAM too on Emscripten (see above) */
  void* guest_ram;
} Layout_Storage;

static Memory_Layout   g_layout;
static Layout_Storage  g_storage;
static int             g_layout_live = 0;

Error layout_create(void) {
  if (g_layout_live) return OK;

  Layout_Storage storage;
  memset(&storage, 0, sizeof(storage));

#if defined(__EMSCRIPTEN__)
  const size_t arena_capacity = (size_t)(LAYOUT_GUEST_RAM_SIZE + LAYOUT_SMALL_REGIONS_SIZE);
#else
  const size_t arena_capacity = (size_t)LAYOUT_SMALL_REGIONS_SIZE;
#endif

  if (!arena_create(&storage.arena, arena_capacity)) {
    return ERR(RESULT_OUT_OF_MEMORY, "layout_create: failed to reserve region arena");
  }

#if defined(__EMSCRIPTEN__)
  storage.guest_ram = arena_allocate(&storage.arena, LAYOUT_GUEST_RAM_SIZE, 8);
#else
  storage.guest_ram = guest_ram_reserve(LAYOUT_GUEST_RAM_SIZE);
#endif

  void* page_table_l1       = arena_allocate(&storage.arena, LAYOUT_PAGE_TABLE_L1_SIZE, 8);
  void* framebuffer         = arena_allocate(&storage.arena, LAYOUT_FRAMEBUFFER_REGION_SIZE, 8);
  void* audio_ring          = arena_allocate(&storage.arena, LAYOUT_AUDIO_RING_SIZE, 8);
  void* gpu_ring            = arena_allocate(&storage.arena, LAYOUT_GPU_RING_SIZE, 8);
  void* gpu_completion_ring = arena_allocate(&storage.arena, LAYOUT_GPU_COMPLETION_RING_SIZE, 8);
  void* input_region        = arena_allocate(&storage.arena, LAYOUT_INPUT_REGION_SIZE, 8);
  void* trace_buffer        = arena_allocate(&storage.arena, LAYOUT_TRACE_BUFFER_SIZE, 8);
  void* breakpoint_region   = arena_allocate(&storage.arena, LAYOUT_BREAKPOINT_REGION_SIZE, 8);

  if (!storage.guest_ram || !page_table_l1 || !framebuffer || !audio_ring || !gpu_ring ||
      !gpu_completion_ring || !input_region || !trace_buffer || !breakpoint_region) {
#if !defined(__EMSCRIPTEN__)
    guest_ram_release(storage.guest_ram, LAYOUT_GUEST_RAM_SIZE);
#endif
    arena_destroy(&storage.arena);
    return ERR(RESULT_OUT_OF_MEMORY, "layout_create: failed to reserve one or more regions");
  }

  g_storage = storage;

  g_layout.guest_ram_base           = (uint64_t)(uintptr_t)g_storage.guest_ram;
  g_layout.guest_ram_size           = LAYOUT_GUEST_RAM_SIZE;
  g_layout.page_table_l1_base       = (uint64_t)(uintptr_t)page_table_l1;
  g_layout.framebuffer_slot_base    = (uint64_t)(uintptr_t)framebuffer;
  g_layout.audio_ring_base          = (uint64_t)(uintptr_t)audio_ring;
  g_layout.gpu_ring_base            = (uint64_t)(uintptr_t)gpu_ring;
  g_layout.gpu_completion_ring_base = (uint64_t)(uintptr_t)gpu_completion_ring;
  g_layout.input_region_base        = (uint64_t)(uintptr_t)input_region;
  g_layout.trace_buffer_base        = (uint64_t)(uintptr_t)trace_buffer;
  g_layout.breakpoint_region_base   = (uint64_t)(uintptr_t)breakpoint_region;

  g_layout_live = 1;

  log_info("[layout] reserved: guest_ram=%llu MiB, page_table_l1=%llu KiB, "
           "framebuffer=%llu MiB, audio_ring=%llu KiB, gpu_ring=%llu KiB, "
           "gpu_completion_ring=%llu KiB, input_region=%llu B, trace_buffer=%llu KiB, "
           "breakpoint_region=%llu B",
           (unsigned long long)(LAYOUT_GUEST_RAM_SIZE / (1024 * 1024)),
           (unsigned long long)(LAYOUT_PAGE_TABLE_L1_SIZE / 1024),
           (unsigned long long)(LAYOUT_FRAMEBUFFER_REGION_SIZE / (1024 * 1024)),
           (unsigned long long)(LAYOUT_AUDIO_RING_SIZE / 1024),
           (unsigned long long)(LAYOUT_GPU_RING_SIZE / 1024),
           (unsigned long long)(LAYOUT_GPU_COMPLETION_RING_SIZE / 1024),
           (unsigned long long)LAYOUT_INPUT_REGION_SIZE,
           (unsigned long long)(LAYOUT_TRACE_BUFFER_SIZE / 1024),
           (unsigned long long)LAYOUT_BREAKPOINT_REGION_SIZE);

  return OK;
}

void layout_destroy(void) {
  if (!g_layout_live) return;
#if !defined(__EMSCRIPTEN__)
  guest_ram_release(g_storage.guest_ram, LAYOUT_GUEST_RAM_SIZE);
#endif
  arena_destroy(&g_storage.arena);
  memset(&g_layout, 0, sizeof(g_layout));
  memset(&g_storage, 0, sizeof(g_storage));
  g_layout_live = 0;
}

const Memory_Layout* layout_get(void) {
  return g_layout_live ? &g_layout : NULL;
}
