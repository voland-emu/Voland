#include "common/layout.h"
#include "common/arena.h"
#include "common/log.h"
#include <stdint.h>
#include <string.h>

#include "common/vmm.h"

#include <stdint.h>
#include <string.h>

/* The §4 host budget, checked where the numbers live. Guest RAM, the fixed
 * regions and the vmm L2 arena are carved at boot; what remains of the
 * 65535-page memory is the "everything else" row of §4's table (Emscripten
 * static data and stacks, the malloc heap, predecoded records, the JIT
 * emission buffer, the renderer's heap). §4 budgets that row at ~709 MiB;
 * a change to any fixed size that eats into it fails here, and §4's table
 * is updated in the same PR. The check is unconditional rather than
 * Emscripten-only: native builds use the same pool sizes (§4), and a
 * native-only build must not be the one that lets the budget slip. */
#define LAYOUT_HOST_BUDGET_BYTES ((uint64_t)709 * LAYOUT_MIB)
#define LAYOUT_BOOT_CARVED_BYTES \
  (LAYOUT_GUEST_RAM_SIZE + LAYOUT_SMALL_REGIONS_SIZE + VMM_L2_ARENA_BYTES)
_Static_assert(LAYOUT_BOOT_CARVED_BYTES + LAYOUT_HOST_BUDGET_BYTES <= LAYOUT_LINEAR_MEMORY_BYTES,
               "guest RAM + fixed layout regions + vmm L2 arena leave less than "
               "§4's ~709 MiB host budget in the 65535-page memory");
_Static_assert(LAYOUT_LINEAR_MEMORY_BYTES <= SIZE_MAX,
               "the linear memory must be addressable by size_t (wasm32: 4 GiB - 64 KiB)");
_Static_assert((LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE & VMM_PAGE_OFFSET_MASK) == 0 &&
                   (LAYOUT_GUEST_RAM_HLE_CARVEOUT_SIZE & VMM_PAGE_OFFSET_MASK) == 0,
               "guest RAM pools must be VMM page multiples (§4/§5)");

/* Guest RAM on native builds goes through a raw virtual-memory reservation
 * rather than the small-regions arena: it is the one region large enough
 * (3333 MiB) that committing it at boot would matter. §4: committing the
 * whole region charges all of it against the Windows commit limit before
 * a page is touched, so Windows reserves only (MEM_RESERVE) and commits
 * per allocation through layout_guest_ram_commit(); POSIX maps with
 * MAP_NORESERVE so no swap is reserved, and anonymous pages are committed
 * lazily on first touch anyway. Either way pages read as zero on first
 * use.
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
  return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
}
static void guest_ram_release(void* p, uint64_t size) {
  (void)size;
  if (p) VirtualFree(p, 0, MEM_RELEASE);
}
static int guest_ram_commit(void* p, uint64_t size) {
  return VirtualAlloc(p, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}
#else
#include <sys/mman.h>
/* macOS defines MAP_NORESERVE but ignores it; some libcs don't define it. */
#if defined(MAP_NORESERVE)
#define LAYOUT_MAP_NORESERVE MAP_NORESERVE
#else
#define LAYOUT_MAP_NORESERVE 0
#endif
static void* guest_ram_reserve(uint64_t size) {
  void* p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | LAYOUT_MAP_NORESERVE, -1, 0);
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
  /* + one page: malloc only guarantees 16-byte alignment, and guest RAM
   * must start on a VMM page (PTEs keep permissions in the low 12 bits of
   * the host offset, §5). */
  const size_t arena_capacity =
      (size_t)(LAYOUT_GUEST_RAM_SIZE + LAYOUT_SMALL_REGIONS_SIZE + VMM_PAGE_SIZE);
#else
  const size_t arena_capacity = (size_t)LAYOUT_SMALL_REGIONS_SIZE;
#endif

  if (!arena_create(&storage.arena, arena_capacity)) {
    return ERR(RESULT_OUT_OF_MEMORY, "layout_create: failed to reserve region arena");
  }

#if defined(__EMSCRIPTEN__)
  /* arena_allocate aligns relative to the arena's base, not absolutely,
   * so take one spare page and round the pointer up here. */
  {
    void* raw = arena_allocate(&storage.arena, (size_t)(LAYOUT_GUEST_RAM_SIZE + VMM_PAGE_SIZE), 1);
    storage.guest_ram =
        raw ? (void*)(((uintptr_t)raw + (uintptr_t)VMM_PAGE_OFFSET_MASK) & ~(uintptr_t)VMM_PAGE_OFFSET_MASK)
            : NULL;
  }
#else
  storage.guest_ram = guest_ram_reserve(LAYOUT_GUEST_RAM_SIZE);
#endif

  void* page_table_l1       = arena_allocate(&storage.arena, LAYOUT_PAGE_TABLE_L1_SIZE, 8);
  void* framebuffer         = arena_allocate(&storage.arena, LAYOUT_FRAMEBUFFER_REGION_SIZE, 8);
  void* audio_ring          = arena_allocate(&storage.arena, LAYOUT_AUDIO_RING_SIZE, 8);
  void* gpu_ring            = arena_allocate(&storage.arena, LAYOUT_GPU_RING_SIZE, 8);
  void* gpu_completion_ring = arena_allocate(&storage.arena, LAYOUT_GPU_COMPLETION_RING_SIZE, 8);
  void* input_region        = arena_allocate(&storage.arena, LAYOUT_INPUT_REGION_SIZE, 8);
  void* output_region       = arena_allocate(&storage.arena, LAYOUT_OUTPUT_REGION_SIZE, 8);
  void* trace_buffer        = arena_allocate(&storage.arena, LAYOUT_TRACE_BUFFER_SIZE, 8);
  void* breakpoint_region   = arena_allocate(&storage.arena, LAYOUT_BREAKPOINT_REGION_SIZE, 8);

  if (!storage.guest_ram || !page_table_l1 || !framebuffer || !audio_ring || !gpu_ring ||
      !gpu_completion_ring || !input_region || !output_region || !trace_buffer || !breakpoint_region) {
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
  g_layout.output_region_base       = (uint64_t)(uintptr_t)output_region;
  g_layout.trace_buffer_base        = (uint64_t)(uintptr_t)trace_buffer;
  g_layout.breakpoint_region_base   = (uint64_t)(uintptr_t)breakpoint_region;

  g_layout_live = 1;

  log_info("[layout] reserved: guest_ram=%llu MiB, page_table_l1=%llu KiB, "
           "framebuffer=%llu MiB, audio_ring=%llu KiB, gpu_ring=%llu KiB, "
           "gpu_completion_ring=%llu KiB, input_region=%llu B, output_region=%llu B, "
           "trace_buffer=%llu KiB, breakpoint_region=%llu B",
           (unsigned long long)(LAYOUT_GUEST_RAM_SIZE / (1024 * 1024)),
           (unsigned long long)(LAYOUT_PAGE_TABLE_L1_SIZE / 1024),
           (unsigned long long)(LAYOUT_FRAMEBUFFER_REGION_SIZE / (1024 * 1024)),
           (unsigned long long)(LAYOUT_AUDIO_RING_SIZE / 1024),
           (unsigned long long)(LAYOUT_GPU_RING_SIZE / 1024),
           (unsigned long long)(LAYOUT_GPU_COMPLETION_RING_SIZE / 1024),
           (unsigned long long)LAYOUT_INPUT_REGION_SIZE,
           (unsigned long long)LAYOUT_OUTPUT_REGION_SIZE,
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

Error layout_guest_ram_commit(uint64_t guest_pa, uint64_t size) {
  if (!g_layout_live) {
    return ERR(RESULT_INVALID_ARGUMENT, "layout_guest_ram_commit: no live layout");
  }
  if (size == 0 || (guest_pa & VMM_PAGE_OFFSET_MASK) != 0 || (size & VMM_PAGE_OFFSET_MASK) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "layout_guest_ram_commit: range not page-aligned or empty");
  }
  /* Overflow-safe: compare size against what remains past guest_pa. */
  if (guest_pa >= LAYOUT_GUEST_RAM_SIZE || size > LAYOUT_GUEST_RAM_SIZE - guest_pa) {
    return ERR(RESULT_INVALID_ARGUMENT, "layout_guest_ram_commit: range outside guest RAM");
  }
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
  if (!guest_ram_commit((uint8_t*)g_storage.guest_ram + guest_pa, size)) {
    return ERR(RESULT_OUT_OF_MEMORY, "layout_guest_ram_commit: VirtualAlloc(MEM_COMMIT) failed");
  }
#endif
  return OK;
}
