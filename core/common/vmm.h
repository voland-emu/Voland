/**
 * Guest virtual memory manager (softmmu). See docs/DESIGN.md section 5.
 *
 * The Switch exposes a 39-bit guest virtual address space with
 * non-contiguous, permissioned, runtime-remappable mappings. WASM has no
 * page-aliasing primitive, so every guest memory access - interpreter,
 * JIT-emitted, and HLE - goes through the two-level page-table walk owned
 * here. This file is the ONLY guest-memory gateway in the codebase: raw
 * pointer arithmetic into guest RAM outside vmm.{h,c} is a rejected PR
 * (CLAUDE.md rule 3). The static inline helpers below ARE vmm.
 *
 * Page tables (§5):
 *
 *   L1: VMM_L1_ENTRY_COUNT (8192) entries, VPN bits 26..14, each the
 *       linear-memory offset of an L2 table, or 0 if absent. Lives in the
 *       fixed layout region `page_table_l1_base` (§4); zeroed by
 *       vmm_create().
 *   L2: VMM_L2_ENTRY_COUNT (16384) entries, VPN bits 13..0, each a PTE.
 *       One L2 spans 64MB of guest VA. Carved on demand from a
 *       vmm-owned arena (never malloc'd per table), recycled through a
 *       freelist when every PTE in it is unmapped again.
 *
 *   PTE (page-aligned host offsets leave the low 12 bits free):
 *       bits 63..12  linear-memory offset of the backing page
 *       bit  2       execute permission   (VMM_PERM_X)
 *       bit  1       write permission     (VMM_PERM_W)
 *       bit  0       read permission      (VMM_PERM_R)
 *       PTE == 0     unmapped
 *
 *   "Linear-memory offset" is a plain pointer value: on Emscripten a C
 *   pointer IS an offset into the single WebAssembly.Memory (§4); on
 *   native, layout.c stores every region base the same way
 *   ((uint64_t)(uintptr_t)ptr), so one PTE format serves both. A PTE with
 *   a nonzero host offset and zero permission bits is a mapped-but-
 *   inaccessible page (guard/reserved), distinguishable from unmapped.
 *
 * Two calling conventions, one implementation (§5):
 *
 *   1. Checked (`Error`-returning) functions below: for HLE, loaders,
 *      tests. A call, a struct return and a branch per access.
 *   2. `static inline` walk/read/write helpers operating directly on the
 *      L1 table, faults reported through a VMM_Fault out-param: for the
 *      interpreter's hottest loop. The interpreter fetches the L1 pointer
 *      once per run() via vmm_page_table_l1() (stable for the context's
 *      lifetime - the L1 is a fixed layout region) and keeps its own
 *      1-entry TLB per access direction on top.
 *
 *   The checked functions in vmm.c are thin wrappers over the inline
 *   helpers. The JIT (§10/§11) emits the equivalent of
 *   vmm_translate_inline() against the same tables.
 *
 * All guest addresses crossing this interface are VIRTUAL unless the
 * parameter is explicitly named `guest_pa` (§5).
 *
 * Endianness: guest (ARM64 LE) and every host this project targets (x86,
 * arm64, wasm) are little-endian, so multi-byte accesses are byte copies.
 * Unaligned accesses are permitted (ARM64 normal-memory semantics); an
 * access that crosses a page boundary is translated page by page and is
 * all-or-nothing: both pages are validated before a single byte moves.
 */
#ifndef SWITCH_COMMON_VMM_H
#define SWITCH_COMMON_VMM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "common/result.h"

/* ------------------------------------------------------------------ */
/* Geometry (§5). Every constant below is derived from three choices:  */
/* 39-bit VA, 4KB pages, 14-bit L2 index.                              */
/* ------------------------------------------------------------------ */

#define VMM_PAGE_BITS 12u
#define VMM_PAGE_SIZE ((uint64_t)1 << VMM_PAGE_BITS)           /* 4096 */
#define VMM_PAGE_OFFSET_MASK (VMM_PAGE_SIZE - 1)                /* 0xFFF */

#define VMM_ADDRESS_BITS 39u
#define VMM_ADDRESS_SPACE_SIZE ((uint64_t)1 << VMM_ADDRESS_BITS) /* 512GB */

#define VMM_VPN_BITS (VMM_ADDRESS_BITS - VMM_PAGE_BITS)          /* 27 */
#define VMM_L2_INDEX_BITS 14u
#define VMM_L1_INDEX_BITS (VMM_VPN_BITS - VMM_L2_INDEX_BITS)     /* 13 */

#define VMM_L1_ENTRY_COUNT ((uint64_t)1 << VMM_L1_INDEX_BITS)    /* 8192 */
#define VMM_L2_ENTRY_COUNT ((uint64_t)1 << VMM_L2_INDEX_BITS)    /* 16384 */
#define VMM_L2_INDEX_MASK (VMM_L2_ENTRY_COUNT - 1)               /* 0x3FFF */

#define VMM_L1_TABLE_BYTES (VMM_L1_ENTRY_COUNT * sizeof(uint64_t)) /* 64KB */
#define VMM_L2_TABLE_BYTES (VMM_L2_ENTRY_COUNT * sizeof(uint64_t)) /* 128KB */
#define VMM_L2_SPAN_BYTES (VMM_L2_ENTRY_COUNT * VMM_PAGE_SIZE)    /* 64MB */

/* Maximum number of L2 tables a context can have live at once. 256 tables
 * cover 16GB of mapped guest VA (4GB of physical RAM mapped once, plus
 * mirrors, plus sparse code/stack/TLS windows) for a 32MB arena. Exceeding
 * it makes vmm_map fail with RESULT_OUT_OF_MEMORY (no partial mutation);
 * revisit when a real title's mapping profile is measured. */
#define VMM_L2_TABLE_CAPACITY ((uint64_t)256)
#define VMM_L2_ARENA_BYTES (VMM_L2_TABLE_CAPACITY * VMM_L2_TABLE_BYTES)

/* PTE field masks. */
#define VMM_PTE_PERM_MASK ((uint64_t)0x7)
#define VMM_PTE_HOST_MASK (~VMM_PAGE_OFFSET_MASK)

/* ------------------------------------------------------------------ */
/* Types.                                                              */
/* ------------------------------------------------------------------ */

typedef enum VMM_Permission {
  VMM_PERM_NONE = 0,
  VMM_PERM_R = 1,
  VMM_PERM_W = 2,
  VMM_PERM_X = 4,
  VMM_PERM_RW = VMM_PERM_R | VMM_PERM_W,
  VMM_PERM_RX = VMM_PERM_R | VMM_PERM_X,
  VMM_PERM_ALL = VMM_PERM_R | VMM_PERM_W | VMM_PERM_X,
} VMM_Permission;

typedef enum VMM_Fault_Kind {
  VMM_FAULT_NONE = 0,
  VMM_FAULT_OUT_OF_RANGE, /* gva >= VMM_ADDRESS_SPACE_SIZE */
  VMM_FAULT_UNMAPPED,     /* L1 entry absent or PTE == 0 */
  VMM_FAULT_PERMISSION,   /* mapped, but a required perm bit is clear */
} VMM_Fault_Kind;

/* Filled by the inline helpers on failure. `gva` is the address of the
 * faulting byte (for a cross-page access, the first byte of the page
 * that faulted), which is what CPU_EXIT_FAULT reports upward (§8). */
typedef struct VMM_Fault {
  VMM_Fault_Kind kind;
  uint32_t required_perms; /* VMM_PERM_* bits the access needed */
  uint64_t gva;
} VMM_Fault;

/* Result of vmm_query(): the maximal page run containing the queried
 * address whose pages all share the same (is_mapped, perms) state. Runs
 * merge across L2 boundaries. Physical contiguity is NOT a run criterion
 * (Horizon's QueryMemory merges by state/permission, not by backing).
 * Holes are reported too: an unmapped run extends to the next mapped page
 * or the end of the address space. */
typedef struct VMM_Region_Info {
  uint64_t base_gva;  /* page-aligned start of the run */
  uint64_t size;      /* bytes, page multiple; run is [base_gva, base_gva + size) */
  uint32_t perms;     /* VMM_PERM_* bits; VMM_PERM_NONE for an unmapped run */
  bool is_mapped;
} VMM_Region_Info;

/* Opaque. One live context at a time: its L1 table is the layout's single
 * `page_table_l1_base` region (§4), so a second context would alias it. */
typedef struct VMM_Context VMM_Context;

/* ------------------------------------------------------------------ */
/* Lifecycle.                                                          */
/* ------------------------------------------------------------------ */

/* Zeroes the L1 table in the layout region and creates the L2 arena.
 * Requires a live layout (layout_create() already called). Returns NULL
 * - and logs why - if no layout is live, a context is already live, or
 * the L2 arena cannot be created. */
VMM_Context *vmm_create(void);

/* Releases the L2 arena. The L1 region stays reserved by the layout (it
 * is re-zeroed by the next vmm_create). Safe to call with NULL. */
void vmm_destroy(VMM_Context *ctx);

/* The L1 table this context walks. Stable for the context's lifetime; the
 * interpreter caches it at run() entry, the JIT receives it as a config
 * constant at compiler init (§10). */
const uint64_t *vmm_page_table_l1(const VMM_Context *ctx);

/* ------------------------------------------------------------------ */
/* Mapping (called by kernel memory SVCs, §12). Every call validates    */
/* its whole range BEFORE mutating any table, so a failed call leaves   */
/* the tables exactly as they were.                                    */
/* ------------------------------------------------------------------ */

/* Maps [gva, gva + size) onto guest physical [guest_pa, guest_pa + size)
 * with `perms`. Preconditions, each a RESULT_INVALID_ARGUMENT:
 *   - gva, guest_pa, size page-aligned; size > 0
 *   - gva + size <= VMM_ADDRESS_SPACE_SIZE
 *   - guest_pa + size <= layout guest_ram_size
 *   - perms is a subset of VMM_PERM_ALL (VMM_PERM_NONE is allowed: a
 *     mapped, inaccessible page)
 *   - every page in the range is currently unmapped
 * Mapping the same guest_pa at two gvas is a mirror and is allowed.
 * RESULT_OUT_OF_MEMORY if the range needs more L2 tables than remain. */
Error vmm_map(VMM_Context *ctx, uint64_t gva, uint64_t guest_pa,
              uint64_t size, uint32_t perms);

/* Unmaps [gva, gva + size). Every page in the range must currently be
 * mapped (RESULT_INVALID_ARGUMENT otherwise; nothing changes). An L2
 * table whose last page is unmapped returns to the freelist. */
Error vmm_unmap(VMM_Context *ctx, uint64_t gva, uint64_t size);

/* Replaces the permission bits of every page in [gva, gva + size). Every
 * page must currently be mapped (RESULT_INVALID_ARGUMENT otherwise;
 * nothing changes). The SMC interaction (§5: write-protect translated
 * code, restore on write fault) is dispatch.c's business (§11) and only
 * uses this call. */
Error vmm_reprotect(VMM_Context *ctx, uint64_t gva, uint64_t size,
                    uint32_t perms);

/* Describes the page run containing `gva` (see VMM_Region_Info). `gva`
 * need not be page-aligned. RESULT_INVALID_ARGUMENT if gva is outside
 * the address space or `info` is NULL. */
Error vmm_query(const VMM_Context *ctx, uint64_t gva,
                VMM_Region_Info *info);

/* ------------------------------------------------------------------ */
/* Checked access (HLE, loaders, tests). Translation faults return      */
/* RESULT_MEMORY_FAULT; the detailed fault is kept in vmm_last_fault(). */
/* Reads need VMM_PERM_R, writes need VMM_PERM_W, on every page touched.*/
/* ------------------------------------------------------------------ */

Error vmm_read8(VMM_Context *ctx, uint64_t gva, uint8_t *out);
Error vmm_read16(VMM_Context *ctx, uint64_t gva, uint16_t *out);
Error vmm_read32(VMM_Context *ctx, uint64_t gva, uint32_t *out);
Error vmm_read64(VMM_Context *ctx, uint64_t gva, uint64_t *out);
Error vmm_write8(VMM_Context *ctx, uint64_t gva, uint8_t value);
Error vmm_write16(VMM_Context *ctx, uint64_t gva, uint16_t value);
Error vmm_write32(VMM_Context *ctx, uint64_t gva, uint32_t value);
Error vmm_write64(VMM_Context *ctx, uint64_t gva, uint64_t value);

/* Bulk copies for HLE buffer traffic and section loading. The whole range
 * is validated before any byte moves, so a fault leaves both the guest
 * and `out`/`src` untouched. Pages need not be physically contiguous.
 * size == 0 is a no-op that returns OK. */
Error vmm_read_block(VMM_Context *ctx, uint64_t gva, void *out, uint64_t size);
Error vmm_write_block(VMM_Context *ctx, uint64_t gva, const void *src, uint64_t size);

/* Translation for HLE code that needs a host pointer to a guest buffer.
 * Succeeds only if every page of [gva, gva + size) is mapped with
 * `required_perms` (non-zero) AND the backing pages are host-contiguous,
 * so the pointer is valid across the whole extent; a mapping whose pages
 * are scattered returns RESULT_NOT_CONTIGUOUS and the caller falls back
 * to vmm_read_block/vmm_write_block. size must be > 0.
 *
 * BORROW SEMANTICS (§5): the returned pointer is a handler-scoped borrow -
 * valid only until the enclosing vmm_borrow_scope_end(), NEVER stored in
 * service state. Green threading guarantees no guest thread (and thus no
 * MapMemory/UnmapMemory) runs while a handler executes; a stored pointer
 * outlives that guarantee and becomes a remap race. Long-lived references
 * hold GVAs and re-translate at use.
 *
 * Debug builds enforce this: the call must occur inside a borrow scope;
 * every borrow is recorded; vmm_map/unmap/reprotect overlapping a live
 * borrow asserts; scope end releases all borrows made inside it. (The GPU command ring is exempt by design: its
 * records carry guest PHYSICAL ranges - real-hardware DMA semantics.) */
Error vmm_guest_to_host(VMM_Context *ctx, uint64_t gva, uint64_t size,
                        uint32_t required_perms, void **host_ptr);

/* Brackets the region in which vmm_guest_to_host() borrows are live: the
 * HLE dispatcher wraps every handler, loaders wrap their section-copy
 * pass. Not nestable. Release builds compile these to no-ops; debug
 * builds assert on nesting, on an unbalanced end, and (at scope end)
 * that the borrow table is not overflowing. */
void vmm_borrow_scope_begin(VMM_Context *ctx);
void vmm_borrow_scope_end(VMM_Context *ctx);

/* The fault behind the most recent RESULT_MEMORY_FAULT from a checked
 * call on this context (kind == VMM_FAULT_NONE if none yet). Diagnostic;
 * HLE maps it to a Horizon result, tests assert on it. */
const VMM_Fault *vmm_last_fault(const VMM_Context *ctx);

/* ------------------------------------------------------------------ */
/* Inline fast path (interpreter). Operates on the L1 pointer from      */
/* vmm_page_table_l1(); no context, no call, no Error struct. Return    */
/* true on success; on false, *fault is filled and *out is untouched.   */
/* ------------------------------------------------------------------ */

/* The §5 fast path, verbatim: two dependent loads plus masking.
 * Returns the host pointer for the byte at `gva`, valid for the
 * remainder of that page only. `required_perms` must be non-zero. */
static inline uint8_t *vmm_translate_inline(const uint64_t *l1, uint64_t gva,
                                            uint32_t required_perms,
                                            VMM_Fault *fault) {
  if (gva >= VMM_ADDRESS_SPACE_SIZE) {
    fault->kind = VMM_FAULT_OUT_OF_RANGE;
    fault->required_perms = required_perms;
    fault->gva = gva;
    return (uint8_t *)0;
  }
  const uint64_t vpn = gva >> VMM_PAGE_BITS;
  const uint64_t l2_offset = l1[vpn >> VMM_L2_INDEX_BITS];
  if (l2_offset == 0) {
    fault->kind = VMM_FAULT_UNMAPPED;
    fault->required_perms = required_perms;
    fault->gva = gva;
    return (uint8_t *)0;
  }
  const uint64_t *l2 = (const uint64_t *)(uintptr_t)l2_offset;
  const uint64_t pte = l2[vpn & VMM_L2_INDEX_MASK];
  if (pte == 0) {
    fault->kind = VMM_FAULT_UNMAPPED;
    fault->required_perms = required_perms;
    fault->gva = gva;
    return (uint8_t *)0;
  }
  if ((pte & required_perms) != required_perms) {
    fault->kind = VMM_FAULT_PERMISSION;
    fault->required_perms = required_perms;
    fault->gva = gva;
    return (uint8_t *)0;
  }
  return (uint8_t *)(uintptr_t)((pte & VMM_PTE_HOST_MASK) | (gva & VMM_PAGE_OFFSET_MASK));
}

/* Out-of-line slow path for accesses that straddle a page boundary
 * (both pages validated before any byte moves). Implemented in vmm.c;
 * declared here because the inline helpers call it. Never call directly. */
bool vmm_read_cross_page(const uint64_t *l1, uint64_t gva, void *out,
                         uint32_t size, VMM_Fault *fault);
bool vmm_write_cross_page(const uint64_t *l1, uint64_t gva, const void *src,
                          uint32_t size, VMM_Fault *fault);

static inline bool vmm_access_crosses_page(uint64_t gva, uint32_t size) {
  return (gva & VMM_PAGE_OFFSET_MASK) + size > VMM_PAGE_SIZE;
}

#define VMM_DEFINE_INLINE_READ(bits, Type)                                    \
  static inline bool vmm_read##bits##_inline(const uint64_t *l1, uint64_t gva, \
                                             Type *out, VMM_Fault *fault) {    \
    if (vmm_access_crosses_page(gva, sizeof(Type))) {                          \
      return vmm_read_cross_page(l1, gva, out, sizeof(Type), fault);           \
    }                                                                          \
    const uint8_t *host = vmm_translate_inline(l1, gva, VMM_PERM_R, fault);    \
    if (!host) return false;                                                   \
    memcpy(out, host, sizeof(Type));                                           \
    return true;                                                               \
  }

#define VMM_DEFINE_INLINE_WRITE(bits, Type)                                    \
  static inline bool vmm_write##bits##_inline(const uint64_t *l1, uint64_t gva, \
                                              Type value, VMM_Fault *fault) {   \
    if (vmm_access_crosses_page(gva, sizeof(Type))) {                           \
      return vmm_write_cross_page(l1, gva, &value, sizeof(Type), fault);        \
    }                                                                           \
    uint8_t *host = vmm_translate_inline(l1, gva, VMM_PERM_W, fault);           \
    if (!host) return false;                                                    \
    memcpy(host, &value, sizeof(Type));                                         \
    return true;                                                                \
  }

VMM_DEFINE_INLINE_READ(8, uint8_t)
VMM_DEFINE_INLINE_READ(16, uint16_t)
VMM_DEFINE_INLINE_READ(32, uint32_t)
VMM_DEFINE_INLINE_READ(64, uint64_t)
VMM_DEFINE_INLINE_WRITE(8, uint8_t)
VMM_DEFINE_INLINE_WRITE(16, uint16_t)
VMM_DEFINE_INLINE_WRITE(32, uint32_t)
VMM_DEFINE_INLINE_WRITE(64, uint64_t)

#undef VMM_DEFINE_INLINE_READ
#undef VMM_DEFINE_INLINE_WRITE

#endif /* SWITCH_COMMON_VMM_H */
