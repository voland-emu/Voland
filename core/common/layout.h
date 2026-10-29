/**
 * Linear memory layout. See docs/DESIGN.md section 4.
 *
 * There is exactly one memory: on web, the core module's shared wasm32
 * WebAssembly.Memory of LAYOUT_LINEAR_MEMORY_PAGES pages; on native, a set of regions carved by this file at
 * emulator_create() time that together stand in for that single mapping.
 * Every subsystem's shared state (guest RAM, page tables, framebuffer
 * slots, audio ring, GPU rings, input region, trace buffer, breakpoint
 * region) is a named region reserved here in one pass, and every consumer
 * (JIT, HLE, JS workers) addresses its region only through the offsets
 * exported by layout_get() - nobody allocates their own shared buffer.
 *
 * Every region except guest RAM is carved from one `common/arena` (no
 * per-region heap calls, per CLAUDE.md's "no malloc, use common/arena"
 * rule). Guest RAM is the one region large enough that committing it
 * matters, so on native it gets its own OS reservation that commits
 * nothing up front (VirtualAlloc MEM_RESERVE on Windows, mmap
 * MAP_NORESERVE elsewhere); pages are committed as page_allocator hands
 * them out, through layout_guest_ram_commit(). On Emscripten the browser
 * already allocated and zeroed the whole WebAssembly.Memory, so guest RAM
 * is arena-carved too and committing is a bounds check.
 *
 * Phase 0 reserves every region; only guest RAM's size is load-bearing yet.
 * The page-table, ring, and buffer sizes below are placeholders sized from
 * the numbers each owning section already commits to (page table: §5,
 * framebuffer: §6, audio ring: §14, trace buffer: §23); they are revisited
 * when those subsystems land, not before.
 */
#ifndef SWITCH_COMMON_LAYOUT_H
#define SWITCH_COMMON_LAYOUT_H

#include <stdint.h>

#include "common/result.h"

#define LAYOUT_MIB ((uint64_t)1024 * 1024)

/* The web build's single memory: 65535 wasm pages, 4 GiB minus one page.
 * Not 65536: a region ending exactly at 2^32 has an end pointer that wraps
 * to 0 in a 32-bit size_t (§4). Mirrors -sINITIAL_MEMORY (CMakeLists.txt)
 * and TOTAL_MEMORY_PAGES (platform/web/src/main.ts); growth is disabled,
 * so a mismatch fails at instantiation. layout.c static-asserts that the
 * fixed regions fit inside it with the §4 host budget left over. */
#define LAYOUT_WASM_PAGE_BYTES ((uint64_t)65536)
#define LAYOUT_LINEAR_MEMORY_PAGES ((uint64_t)65535)
#define LAYOUT_LINEAR_MEMORY_BYTES (LAYOUT_LINEAR_MEMORY_PAGES * LAYOUT_WASM_PAGE_BYTES)

/* Guest RAM: the application pool plus the HLE carve-out (§4), not the
 * console's whole 4 GB. Horizon gives a retail title the application pool
 * (3285 MiB in the 4 GB arrangement); the applet and system pools belong
 * to the OS, which Voland's HLE replaces. The carve-out follows the pool
 * and holds guest pages that system services own on hardware (pl:u fonts,
 * hid:, time: and applet shared memory); page_allocator gains the split
 * with its first HLE-owned consumer (hid:, Phase 4). Guest physical
 * address n is linear-memory offset guest_ram_base + n. Page-aligned per
 * §4/§5. */
#define LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE ((uint64_t)3285 * LAYOUT_MIB)
#define LAYOUT_GUEST_RAM_HLE_CARVEOUT_SIZE ((uint64_t)48 * LAYOUT_MIB)
#define LAYOUT_GUEST_RAM_HLE_CARVEOUT_BASE_PA LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE
#define LAYOUT_GUEST_RAM_SIZE \
  (LAYOUT_GUEST_RAM_APPLICATION_POOL_SIZE + LAYOUT_GUEST_RAM_HLE_CARVEOUT_SIZE) /* 3333 MiB */

/* Softmmu L1 page table: 8192 entries * 8 bytes (§5). L2 tables are
 * allocated on demand from an arena in Phase 1 and are not part of the
 * fixed layout. */
#define LAYOUT_PAGE_TABLE_L1_SIZE ((uint64_t)8192 * 8)

/* Framebuffer slots: publish/consume counters + slot metadata x2 + two
 * pixel slots sized for up to 1920x1080 RGBA8 (§6). Real GPU output
 * (§13) may re-size this once resolution negotiation exists. */
#define LAYOUT_FRAMEBUFFER_MAX_WIDTH ((uint64_t)1920)
#define LAYOUT_FRAMEBUFFER_MAX_HEIGHT ((uint64_t)1080)
#define LAYOUT_FRAMEBUFFER_BYTES_PER_PIXEL ((uint64_t)4)
#define LAYOUT_FRAMEBUFFER_SLOT_BYTES \
  (LAYOUT_FRAMEBUFFER_MAX_WIDTH * LAYOUT_FRAMEBUFFER_MAX_HEIGHT * LAYOUT_FRAMEBUFFER_BYTES_PER_PIXEL)
#define LAYOUT_FRAMEBUFFER_HEADER_BYTES ((uint64_t)128) /* counters + 2x slot metadata */
#define LAYOUT_FRAMEBUFFER_REGION_SIZE \
  (LAYOUT_FRAMEBUFFER_HEADER_BYTES + 2 * LAYOUT_FRAMEBUFFER_SLOT_BYTES)

/* Audio ring: write/read index + capacity_frames x stereo x f32 (§14).
 * capacity_frames=4096 is the default (~85ms at 48kHz stereo). */
#define LAYOUT_AUDIO_RING_CAPACITY_FRAMES ((uint64_t)4096)
#define LAYOUT_AUDIO_RING_CHANNELS ((uint64_t)2)
#define LAYOUT_AUDIO_RING_HEADER_BYTES ((uint64_t)8) /* write index + read index */
#define LAYOUT_AUDIO_RING_SIZE \
  (LAYOUT_AUDIO_RING_HEADER_BYTES + LAYOUT_AUDIO_RING_CAPACITY_FRAMES * LAYOUT_AUDIO_RING_CHANNELS * sizeof(float))

/* GPU command ring and GPU->CPU completion ring (§13). Sized generously
 * as a placeholder; the real command format lands in docs/GPU_COMMAND_STREAM.md
 * (Phase 4). */
#define LAYOUT_GPU_RING_SIZE ((uint64_t)4 * 1024 * 1024)
#define LAYOUT_GPU_COMPLETION_RING_SIZE ((uint64_t)64 * 1024)

/* Input region: 8 controller slots, seqlock + buttons + axes + flags,
 * rounded up to 32 bytes/slot (§18). */
#define LAYOUT_INPUT_REGION_MAX_CONTROLLERS ((uint64_t)8)
#define LAYOUT_INPUT_REGION_SLOT_BYTES ((uint64_t)32)
#define LAYOUT_INPUT_REGION_SIZE \
  (LAYOUT_INPUT_REGION_MAX_CONTROLLERS * LAYOUT_INPUT_REGION_SLOT_BYTES)

/* Trace buffer: write index + pad + capacity x 24-byte Trace_Event (§23). */
#define LAYOUT_TRACE_BUFFER_CAPACITY ((uint64_t)65536)
#define LAYOUT_TRACE_EVENT_BYTES ((uint64_t)24)
#define LAYOUT_TRACE_BUFFER_HEADER_BYTES ((uint64_t)8)
#define LAYOUT_TRACE_BUFFER_SIZE \
  (LAYOUT_TRACE_BUFFER_HEADER_BYTES + LAYOUT_TRACE_BUFFER_CAPACITY * LAYOUT_TRACE_EVENT_BYTES)

/* Breakpoint region: atomic flag bits, read by the debugger and the CPU
 * worker (§23). Placeholder size until the debugger protocol is designed. */
#define LAYOUT_BREAKPOINT_REGION_SIZE ((uint64_t)4096)

/* Every region except guest RAM, summed for the one arena_create() call
 * that backs all of them (layout.c). +64 covers alignment padding across
 * the individual arena_allocate() calls (8 regions x <=7 bytes each). */
#define LAYOUT_SMALL_REGIONS_SIZE \
  (LAYOUT_PAGE_TABLE_L1_SIZE + LAYOUT_FRAMEBUFFER_REGION_SIZE + LAYOUT_AUDIO_RING_SIZE + \
   LAYOUT_GPU_RING_SIZE + LAYOUT_GPU_COMPLETION_RING_SIZE + LAYOUT_INPUT_REGION_SIZE + \
   LAYOUT_TRACE_BUFFER_SIZE + LAYOUT_BREAKPOINT_REGION_SIZE + (uint64_t)64)

typedef struct Memory_Layout {
  uint64_t guest_ram_base;         /* linear-memory offset of guest physical RAM */
  uint64_t guest_ram_size;         /* application pool + HLE carve-out (§4) */
  uint64_t page_table_l1_base;     /* §5 */
  uint64_t framebuffer_slot_base;  /* §6: 2 slots + publish counter */
  uint64_t audio_ring_base;        /* §14 */
  uint64_t gpu_ring_base;          /* §13 */
  uint64_t gpu_completion_ring_base; /* §13: GPU->CPU syncpoint completions */
  uint64_t input_region_base;      /* §18 */
  uint64_t trace_buffer_base;      /* §23 */
  uint64_t breakpoint_region_base; /* §23 */
} Memory_Layout;

/* Reserves every region in one pass. Idempotent: calling this while a
 * layout is already live is a no-op that returns OK. */
Error layout_create(void);

/* Releases every region. Safe to call when no layout is live. */
void layout_destroy(void);

/* Returns the live layout, or NULL if layout_create() has not been called
 * (or has been destroyed). */
const Memory_Layout* layout_get(void);

/* Makes guest RAM [guest_pa, guest_pa + size) backed and writable. On
 * Windows this commits the pages (MEM_COMMIT) against the commit limit; on
 * every other platform the reservation is already usable and this only
 * validates the range. Committing an already committed page is a no-op.
 * A page reads as zero after its FIRST commit only: nothing is decommitted
 * before layout_destroy(), so a page freed and handed out again keeps its
 * previous owner's bytes, and whoever maps it zeroes it if the guest
 * expects zeroed memory (svcSetHeapSize does).
 * RESULT_INVALID_ARGUMENT if no layout is live, `size` is 0, either value
 * is not VMM page-aligned, or the range leaves guest RAM;
 * RESULT_OUT_OF_MEMORY if the OS refuses the commit (nothing is committed
 * that wasn't before). */
Error layout_guest_ram_commit(uint64_t guest_pa, uint64_t size);

#endif /* SWITCH_COMMON_LAYOUT_H */
