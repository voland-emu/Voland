/**
 * Linear memory layout. See docs/DESIGN.md section 4.
 *
 * There is exactly one memory: on web, the core module's shared
 * WebAssembly.Memory; on native, a set of regions carved by this file at
 * emulator_create() time that together stand in for that single mapping.
 * Every subsystem's shared state (guest RAM, page tables, framebuffer
 * slots, audio ring, GPU rings, input region, trace buffer, breakpoint
 * region) is a named region reserved here in one pass, and every consumer
 * (JIT, HLE, JS workers) addresses its region only through the offsets
 * exported by layout_get() - nobody allocates their own shared buffer.
 *
 * Every region except guest RAM is carved from one `common/arena` (no
 * per-region heap calls, per CLAUDE.md's "no malloc, use common/arena"
 * rule). Guest RAM is the one region large enough that eagerly touching
 * it matters, so on native it gets its own lazy OS reservation instead
 * (mmap/VirtualAlloc); on Emscripten there is no such distinction to
 * exploit (the browser already allocated and zeroed the whole
 * WebAssembly.Memory), so it is arena-carved too.
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

/* Guest RAM: Switch 1 physical RAM. Page-aligned per §4/§5 - satisfied
 * automatically by the OS-level reservation in layout.c (mmap/VirtualAlloc
 * always return page-aligned addresses), not an explicit parameter. */
#define LAYOUT_GUEST_RAM_SIZE ((uint64_t)4 * 1024 * 1024 * 1024)

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
  uint64_t guest_ram_size;         /* 4GB (Switch 1) */
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

#endif /* SWITCH_COMMON_LAYOUT_H */
