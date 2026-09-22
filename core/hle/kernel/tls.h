/**
 * Guest thread-local-storage allocator. See docs/DESIGN.md §12 ("The
 * call surface": "Thread-local storage is the transport") and §25 Phase
 * 1 ("TLS allocation + tpidrro_el0 plumbing").
 *
 * Every guest thread owns one TLS_BLOCK_BYTES (0x200) block whose first
 * TLS_IPC_COMMAND_BUFFER_BYTES (0x100) are the IPC command buffer;
 * `tpidrro_el0` points at the block. Blocks are carved from guest-visible
 * pages in the process's tls_io region (address_space.h), eight per page.
 * This is Horizon's per-process TLS page list in its smallest useful
 * form: pages are mapped RW through vmm on demand, blocks are handed out
 * from any page with a free slot before a new page is mapped, and a freed
 * block returns to its page's pool. A page whose blocks are all free
 * stays mapped (the physical page could not be returned anyway,
 * page_allocator.h); it is reused by the next allocation.
 *
 * Placement: page n lives at region.base + n * VMM_PAGE_SIZE. The tls_io
 * region is shared with transfer memory later (§12); a region allocator
 * that interleaves the two is that work's business - until then TLS pages
 * grow upward from the region base and nothing else is placed there.
 *
 * Contents: a block is zeroed when allocated, matching the kernel (a
 * fresh thread's IPC buffer is empty). Nothing here reads the block
 * afterwards; the IPC layer accesses it through vmm like any guest
 * memory (rule 3).
 *
 * Capacity: TLS_MAX_PAGES pages, i.e. TLS_MAX_BLOCKS concurrent threads.
 * Horizon's own per-process limit comes from the kernel's resource limit
 * (thread count), not the npdm; a title that exceeds this cap gets
 * RESULT_OUT_OF_MEMORY from tls_allocate and the CreateThread SVC
 * surfaces it as the kernel's out-of-resource result.
 *
 * Not thread-safe by construction: all guest threads are green threads
 * in one CPU worker (§7).
 */
#ifndef SWITCH_HLE_KERNEL_TLS_H
#define SWITCH_HLE_KERNEL_TLS_H

#include <stdbool.h>
#include <stdint.h>

#include "common/result.h"
#include "common/vmm.h"
#include "hle/kernel/address_space.h"
#include "hle/kernel/page_allocator.h"

#define TLS_BLOCK_BYTES ((uint64_t)0x200)
#define TLS_BLOCKS_PER_PAGE ((uint32_t)(VMM_PAGE_SIZE / TLS_BLOCK_BYTES)) /* 8 */
/* The IPC command buffer is the first 0x100 bytes of a block (§12). */
#define TLS_IPC_COMMAND_BUFFER_BYTES ((uint64_t)0x100)
/* 64 pages * 8 blocks = 512 threads per process. */
#define TLS_MAX_PAGES ((uint32_t)64)
#define TLS_MAX_BLOCKS (TLS_MAX_PAGES * TLS_BLOCKS_PER_PAGE)

typedef struct TLS_Page {
  uint64_t gva;      /* page base; mapped RW */
  uint8_t used_mask; /* bit b set = block b is allocated */
} TLS_Page;

typedef struct TLS_Allocator {
  VMM_Context *vmm;      /* borrowed; outlives the allocator */
  Page_Allocator *pages; /* borrowed; physical pages for new TLS pages */
  Address_Region region; /* the process's tls_io region */
  TLS_Page mapped[TLS_MAX_PAGES];
  uint32_t page_count;    /* pages mapped so far */
  uint32_t blocks_in_use; /* across all pages */
} TLS_Allocator;

/* Binds an empty allocator to `region` (page-aligned, at least one page).
 * Maps nothing yet. RESULT_INVALID_ARGUMENT on NULL or a bad region. */
Error tls_allocator_init(TLS_Allocator *out, VMM_Context *vmm, Page_Allocator *pages,
                         Address_Region region);

/* Allocates one zeroed block; `*out_gva` receives its guest address,
 * TLS_BLOCK_BYTES-aligned inside a mapped page. Maps a fresh RW page when
 * no mapped page has a free block. The lowest free block of the lowest
 * page is chosen, so the sequence of addresses is deterministic.
 *   RESULT_INVALID_ARGUMENT NULL
 *   RESULT_OUT_OF_MEMORY    TLS_MAX_PAGES reached, the region is full,
 *                           physical pages exhausted, or vmm L2 tables
 *                           exhausted; the allocator is unchanged */
Error tls_allocate(TLS_Allocator *allocator, uint64_t *out_gva);

/* Returns a block to its page's pool. The block is NOT zeroed (the next
 * tls_allocate does that). RESULT_INVALID_ARGUMENT if `gva` is not the
 * base of a currently allocated block (unaligned, unmapped page, or
 * already free) - a stale thread must not corrupt the pool. */
Error tls_free(TLS_Allocator *allocator, uint64_t gva);

/* True iff `gva` is the base of a currently allocated block. */
bool tls_is_allocated(const TLS_Allocator *allocator, uint64_t gva);

/* Unmaps every TLS page and zeroes the allocator. Physical pages are not
 * reclaimed (page_allocator.h). Safe on a zeroed allocator. Outstanding
 * blocks are simply forgotten: the process is going away. */
void tls_allocator_teardown(TLS_Allocator *allocator);

#endif /* SWITCH_HLE_KERNEL_TLS_H */
