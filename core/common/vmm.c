/**
 * Softmmu implementation. See vmm.h for the interface contract and
 * docs/DESIGN.md section 5 for the design.
 *
 * Structure:
 *   - one static context (the L1 table is the layout's single region, so
 *     two live contexts would alias it - see vmm.h)
 *   - L2 tables carved from a vmm-owned arena, recycled via a freelist
 *     threaded through each free table's first entry
 *   - every mutation validates its whole range, then commits
 *   - every checked access is a thin wrapper over the header's inline
 *     helpers; block copies validate every page, then copy
 *   - debug-only borrow tracking for vmm_guest_to_host()
 */
#include "common/vmm.h"

#include "common/arena.h"
#include "common/assert.h"
#include "common/layout.h"
#include "common/log.h"

#include <stddef.h>
#include <string.h>

_Static_assert(VMM_L1_TABLE_BYTES == LAYOUT_PAGE_TABLE_L1_SIZE,
               "vmm L1 geometry must match the layout's page_table_l1 region (§4/§5)");
_Static_assert(sizeof(void *) == sizeof(uint64_t),
               "PTEs store host pointers in bits 63..12; a 64-bit host is required");

/* L2 tables are cache-line aligned. The arena has room for one extra
 * alignment's worth of padding so the last of VMM_L2_TABLE_CAPACITY
 * tables still fits after the first allocation is aligned. */
#define VMM_L2_TABLE_ALIGNMENT ((size_t)64)

#define VMM_TOTAL_PAGE_COUNT (VMM_ADDRESS_SPACE_SIZE >> VMM_PAGE_BITS)

#ifndef NDEBUG
#define VMM_DEBUG_BORROWS 1
/* A handler borrowing more than this many distinct buffers is a bug in
 * its own right; the table is fixed-size so tracking never allocates. */
#define VMM_DEBUG_MAX_BORROWS 64u
typedef struct VMM_Borrow {
  uint64_t gva;
  uint64_t size;
} VMM_Borrow;
#endif

struct VMM_Context {
  uint64_t *l1;             /* layout page_table_l1 region */
  uint64_t guest_ram_base;  /* linear-memory offset of guest PA 0 */
  uint64_t guest_ram_size;

  Arena l2_arena;
  uint64_t *l2_freelist;    /* singly linked through entry [0] of each free table */
  uint64_t l2_free_count;   /* tables on the freelist */
  uint64_t l2_carved_count; /* tables ever carved from the arena */

  /* Mapped (nonzero) PTE count per L2, indexed by L1 index. Zero for an
   * absent L2. Reaching zero on unmap returns the table to the freelist. */
  uint32_t l2_mapped_count[VMM_L1_ENTRY_COUNT];

  VMM_Fault last_fault;

#ifdef VMM_DEBUG_BORROWS
  VMM_Borrow borrows[VMM_DEBUG_MAX_BORROWS];
  uint32_t borrow_count;
  int borrow_scope_depth;
#endif
};

static VMM_Context g_vmm;
static int g_vmm_live = 0;

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static bool is_page_aligned(uint64_t value) {
  return (value & VMM_PAGE_OFFSET_MASK) == 0;
}

/* [gva, gva + size) is page-aligned, non-empty, and inside the address
 * space. Overflow-safe. */
static bool is_valid_page_range(uint64_t gva, uint64_t size) {
  return is_page_aligned(gva) && is_page_aligned(size) && size > 0 &&
         gva < VMM_ADDRESS_SPACE_SIZE && size <= VMM_ADDRESS_SPACE_SIZE - gva;
}

/* [gva, gva + size) with any alignment, non-empty, inside the address
 * space. Used by the access paths. */
static bool is_valid_byte_range(uint64_t gva, uint64_t size) {
  return size > 0 && gva < VMM_ADDRESS_SPACE_SIZE && size <= VMM_ADDRESS_SPACE_SIZE - gva;
}

static uint64_t *l2_table_for_vpn(const VMM_Context *ctx, uint64_t vpn) {
  return (uint64_t *)(uintptr_t)ctx->l1[vpn >> VMM_L2_INDEX_BITS];
}

static uint64_t pte_load(const VMM_Context *ctx, uint64_t vpn) {
  const uint64_t *l2 = l2_table_for_vpn(ctx, vpn);
  return l2 ? l2[vpn & VMM_L2_INDEX_MASK] : 0;
}

static void pte_store(VMM_Context *ctx, uint64_t vpn, uint64_t pte) {
  uint64_t *l2 = l2_table_for_vpn(ctx, vpn);
  SWITCH_ASSERT(l2 != NULL, "pte_store: L2 absent");
  l2[vpn & VMM_L2_INDEX_MASK] = pte;
}

static uint64_t make_pte(const VMM_Context *ctx, uint64_t guest_pa, uint32_t perms) {
  const uint64_t host = ctx->guest_ram_base + guest_pa;
  SWITCH_ASSERT(is_page_aligned(host), "make_pte: backing page not page-aligned");
  return (host & VMM_PTE_HOST_MASK) | ((uint64_t)perms & VMM_PTE_PERM_MASK);
}

/* ------------------------------------------------------------------ */
/* L2 table pool.                                                      */
/* ------------------------------------------------------------------ */

static uint64_t l2_tables_available(const VMM_Context *ctx) {
  return ctx->l2_free_count + (VMM_L2_TABLE_CAPACITY - ctx->l2_carved_count);
}

/* Returns an all-zero table. Callers check l2_tables_available() first. */
static uint64_t *l2_table_acquire(VMM_Context *ctx) {
  if (ctx->l2_freelist) {
    uint64_t *table = ctx->l2_freelist;
    ctx->l2_freelist = (uint64_t *)(uintptr_t)table[0];
    ctx->l2_free_count--;
    table[0] = 0; /* the only nonzero entry in a free table is the link */
    return table;
  }
  SWITCH_ASSERT_ALWAYS(ctx->l2_carved_count < VMM_L2_TABLE_CAPACITY,
                       "l2_table_acquire: capacity exhausted (caller skipped the check)");
  uint64_t *table = (uint64_t *)arena_allocate(&ctx->l2_arena, (size_t)VMM_L2_TABLE_BYTES,
                                               VMM_L2_TABLE_ALIGNMENT);
  SWITCH_ASSERT_ALWAYS(table != NULL, "l2_table_acquire: arena exhausted below capacity");
  memset(table, 0, (size_t)VMM_L2_TABLE_BYTES);
  ctx->l2_carved_count++;
  return table;
}

/* `table` must be all-zero (mapped count reached zero). */
static void l2_table_release(VMM_Context *ctx, uint64_t *table) {
  table[0] = (uint64_t)(uintptr_t)ctx->l2_freelist;
  ctx->l2_freelist = table;
  ctx->l2_free_count++;
}

/* Number of L1 slots in [gva, gva + size) with no L2 yet. */
static uint64_t count_absent_l2_slots(const VMM_Context *ctx, uint64_t gva, uint64_t size) {
  const uint64_t first = (gva >> VMM_PAGE_BITS) >> VMM_L2_INDEX_BITS;
  const uint64_t last = ((gva + size - 1) >> VMM_PAGE_BITS) >> VMM_L2_INDEX_BITS;
  uint64_t absent = 0;
  for (uint64_t i = first; i <= last; i++) {
    if (ctx->l1[i] == 0) absent++;
  }
  return absent;
}

/* ------------------------------------------------------------------ */
/* Debug borrow tracking.                                              */
/* ------------------------------------------------------------------ */

#ifdef VMM_DEBUG_BORROWS
static void debug_assert_no_live_borrow_overlaps(const VMM_Context *ctx, uint64_t gva,
                                                 uint64_t size) {
  for (uint32_t i = 0; i < ctx->borrow_count; i++) {
    const VMM_Borrow *b = &ctx->borrows[i];
    const bool disjoint = (gva + size <= b->gva) || (b->gva + b->size <= gva);
    SWITCH_ASSERT_ALWAYS(disjoint,
                         "vmm: page-table mutation overlaps a live vmm_guest_to_host borrow "
                         "(§5 borrow semantics violated)");
  }
}

static void debug_record_borrow(VMM_Context *ctx, uint64_t gva, uint64_t size) {
  SWITCH_ASSERT_ALWAYS(ctx->borrow_scope_depth == 1,
                       "vmm_guest_to_host called outside a vmm_borrow_scope (§5)");
  SWITCH_ASSERT_ALWAYS(ctx->borrow_count < VMM_DEBUG_MAX_BORROWS,
                       "vmm: too many live borrows in one scope");
  ctx->borrows[ctx->borrow_count].gva = gva;
  ctx->borrows[ctx->borrow_count].size = size;
  ctx->borrow_count++;
}
#endif

void vmm_borrow_scope_begin(VMM_Context *ctx) {
#ifdef VMM_DEBUG_BORROWS
  SWITCH_ASSERT_ALWAYS(ctx != NULL, "vmm_borrow_scope_begin: ctx is NULL");
  SWITCH_ASSERT_ALWAYS(ctx->borrow_scope_depth == 0, "vmm_borrow_scope_begin: scopes do not nest");
  ctx->borrow_scope_depth = 1;
  ctx->borrow_count = 0;
#else
  (void)ctx;
#endif
}

void vmm_borrow_scope_end(VMM_Context *ctx) {
#ifdef VMM_DEBUG_BORROWS
  SWITCH_ASSERT_ALWAYS(ctx != NULL, "vmm_borrow_scope_end: ctx is NULL");
  SWITCH_ASSERT_ALWAYS(ctx->borrow_scope_depth == 1, "vmm_borrow_scope_end: no scope open");
  ctx->borrow_scope_depth = 0;
  ctx->borrow_count = 0; /* handler exit releases every borrow made inside it */
#else
  (void)ctx;
#endif
}

/* ------------------------------------------------------------------ */
/* Lifecycle.                                                          */
/* ------------------------------------------------------------------ */

VMM_Context *vmm_create(void) {
  const Memory_Layout *layout = layout_get();
  if (!layout) {
    log_error("[vmm] vmm_create: no live layout (call layout_create first)");
    return NULL;
  }
  if (g_vmm_live) {
    log_error("[vmm] vmm_create: a context is already live");
    return NULL;
  }

  memset(&g_vmm, 0, sizeof(g_vmm));

  const size_t arena_capacity = (size_t)VMM_L2_ARENA_BYTES + VMM_L2_TABLE_ALIGNMENT;
  if (!arena_create(&g_vmm.l2_arena, arena_capacity)) {
    log_error("[vmm] vmm_create: failed to reserve the L2 table arena (%llu bytes)",
              (unsigned long long)arena_capacity);
    memset(&g_vmm, 0, sizeof(g_vmm));
    return NULL;
  }

  g_vmm.l1 = (uint64_t *)(uintptr_t)layout->page_table_l1_base;
  g_vmm.guest_ram_base = layout->guest_ram_base;
  g_vmm.guest_ram_size = layout->guest_ram_size;

  /* The layout carves the L1 region from a malloc'd arena without zeroing
   * it; a stale nonzero entry would be walked as an L2 pointer. */
  memset(g_vmm.l1, 0, (size_t)VMM_L1_TABLE_BYTES);

  g_vmm_live = 1;

  log_info("[vmm] created: %llu-bit VA, %llu L1 entries, up to %llu L2 tables (%llu MiB arena)",
           (unsigned long long)VMM_ADDRESS_BITS,
           (unsigned long long)VMM_L1_ENTRY_COUNT,
           (unsigned long long)VMM_L2_TABLE_CAPACITY,
           (unsigned long long)(VMM_L2_ARENA_BYTES / (1024 * 1024)));
  return &g_vmm;
}

void vmm_destroy(VMM_Context *ctx) {
  if (!ctx) return;
  SWITCH_ASSERT_ALWAYS(ctx == &g_vmm && g_vmm_live, "vmm_destroy: not the live context");
  /* Leave no dangling L2 offsets in the shared L1 region: the arena that
   * backed them is about to go away. */
  memset(ctx->l1, 0, (size_t)VMM_L1_TABLE_BYTES);
  arena_destroy(&ctx->l2_arena);
  memset(ctx, 0, sizeof(*ctx));
  g_vmm_live = 0;
}

const uint64_t *vmm_page_table_l1(const VMM_Context *ctx) {
  SWITCH_ASSERT_ALWAYS(ctx != NULL, "vmm_page_table_l1: ctx is NULL");
  return ctx->l1;
}

const VMM_Fault *vmm_last_fault(const VMM_Context *ctx) {
  SWITCH_ASSERT_ALWAYS(ctx != NULL, "vmm_last_fault: ctx is NULL");
  return &ctx->last_fault;
}

/* ------------------------------------------------------------------ */
/* Mapping.                                                            */
/* ------------------------------------------------------------------ */

Error vmm_map(VMM_Context *ctx, uint64_t gva, uint64_t guest_pa, uint64_t size,
              uint32_t perms) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_map: ctx is NULL");
  if (!is_valid_page_range(gva, size)) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_map: gva/size not page-aligned, empty, or out of range");
  }
  if (!is_page_aligned(guest_pa) || guest_pa >= ctx->guest_ram_size ||
      size > ctx->guest_ram_size - guest_pa) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_map: guest_pa/size not page-aligned or beyond guest RAM");
  }
  if ((perms & ~(uint32_t)VMM_PERM_ALL) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_map: perms has bits outside VMM_PERM_ALL");
  }

  /* Validate: every page unmapped, enough L2 tables for the new slots. */
  const uint64_t first_vpn = gva >> VMM_PAGE_BITS;
  const uint64_t page_count = size >> VMM_PAGE_BITS;
  for (uint64_t i = 0; i < page_count; i++) {
    if (pte_load(ctx, first_vpn + i) != 0) {
      return ERR(RESULT_INVALID_ARGUMENT, "vmm_map: range overlaps an existing mapping");
    }
  }
  if (count_absent_l2_slots(ctx, gva, size) > l2_tables_available(ctx)) {
    return ERR(RESULT_OUT_OF_MEMORY, "vmm_map: L2 table capacity exhausted (VMM_L2_TABLE_CAPACITY)");
  }
#ifdef VMM_DEBUG_BORROWS
  debug_assert_no_live_borrow_overlaps(ctx, gva, size);
#endif

  /* Commit. */
  for (uint64_t i = 0; i < page_count; i++) {
    const uint64_t vpn = first_vpn + i;
    const uint64_t l1_index = vpn >> VMM_L2_INDEX_BITS;
    if (ctx->l1[l1_index] == 0) {
      ctx->l1[l1_index] = (uint64_t)(uintptr_t)l2_table_acquire(ctx);
    }
    pte_store(ctx, vpn, make_pte(ctx, guest_pa + (i << VMM_PAGE_BITS), perms));
    ctx->l2_mapped_count[l1_index]++;
  }
  return OK;
}

/* Shared precondition of unmap/reprotect: every page in the range mapped. */
static Error require_range_mapped(const VMM_Context *ctx, uint64_t gva, uint64_t size) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm: ctx is NULL");
  if (!is_valid_page_range(gva, size)) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm: gva/size not page-aligned, empty, or out of range");
  }
  const uint64_t first_vpn = gva >> VMM_PAGE_BITS;
  const uint64_t page_count = size >> VMM_PAGE_BITS;
  for (uint64_t i = 0; i < page_count; i++) {
    if (pte_load(ctx, first_vpn + i) == 0) {
      return ERR(RESULT_INVALID_ARGUMENT, "vmm: range contains an unmapped page");
    }
  }
  return OK;
}

Error vmm_unmap(VMM_Context *ctx, uint64_t gva, uint64_t size) {
  const Error precondition = require_range_mapped(ctx, gva, size);
  if (!error_is_ok(precondition)) return precondition;
#ifdef VMM_DEBUG_BORROWS
  debug_assert_no_live_borrow_overlaps(ctx, gva, size);
#endif

  const uint64_t first_vpn = gva >> VMM_PAGE_BITS;
  const uint64_t page_count = size >> VMM_PAGE_BITS;
  for (uint64_t i = 0; i < page_count; i++) {
    const uint64_t vpn = first_vpn + i;
    const uint64_t l1_index = vpn >> VMM_L2_INDEX_BITS;
    pte_store(ctx, vpn, 0);
    SWITCH_ASSERT(ctx->l2_mapped_count[l1_index] > 0, "vmm_unmap: mapped count underflow");
    ctx->l2_mapped_count[l1_index]--;
    if (ctx->l2_mapped_count[l1_index] == 0) {
      uint64_t *table = l2_table_for_vpn(ctx, vpn);
      ctx->l1[l1_index] = 0;
      l2_table_release(ctx, table);
    }
  }
  return OK;
}

Error vmm_reprotect(VMM_Context *ctx, uint64_t gva, uint64_t size, uint32_t perms) {
  if ((perms & ~(uint32_t)VMM_PERM_ALL) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_reprotect: perms has bits outside VMM_PERM_ALL");
  }
  const Error precondition = require_range_mapped(ctx, gva, size);
  if (!error_is_ok(precondition)) return precondition;
#ifdef VMM_DEBUG_BORROWS
  debug_assert_no_live_borrow_overlaps(ctx, gva, size);
#endif

  const uint64_t first_vpn = gva >> VMM_PAGE_BITS;
  const uint64_t page_count = size >> VMM_PAGE_BITS;
  for (uint64_t i = 0; i < page_count; i++) {
    const uint64_t vpn = first_vpn + i;
    const uint64_t pte = pte_load(ctx, vpn);
    pte_store(ctx, vpn, (pte & VMM_PTE_HOST_MASK) | ((uint64_t)perms & VMM_PTE_PERM_MASK));
  }
  return OK;
}

/* ------------------------------------------------------------------ */
/* Query.                                                              */
/* ------------------------------------------------------------------ */

/* Run key: unmapped pages are 0; mapped pages are perms | 8 so a mapped
 * PERM_NONE page (key 8) never merges with a hole (key 0). */
#define VMM_QUERY_MAPPED_BIT ((uint64_t)8)

static uint64_t page_run_key(const VMM_Context *ctx, uint64_t vpn) {
  const uint64_t pte = pte_load(ctx, vpn);
  return pte ? ((pte & VMM_PTE_PERM_MASK) | VMM_QUERY_MAPPED_BIT) : 0;
}

static bool l2_slot_absent(const VMM_Context *ctx, uint64_t vpn) {
  return ctx->l1[vpn >> VMM_L2_INDEX_BITS] == 0;
}

Error vmm_query(const VMM_Context *ctx, uint64_t gva, VMM_Region_Info *info) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_query: ctx is NULL");
  if (!info) return ERR(RESULT_INVALID_ARGUMENT, "vmm_query: info is NULL");
  if (gva >= VMM_ADDRESS_SPACE_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_query: gva outside the address space");
  }

  const uint64_t vpn = gva >> VMM_PAGE_BITS;
  const uint64_t key = page_run_key(ctx, vpn);
  const bool hole = (key == 0);

  /* Extend backward. Whole absent L2 slots are skipped in one step when
   * the run is a hole. */
  uint64_t start = vpn;
  while (start > 0) {
    if (hole && (start & VMM_L2_INDEX_MASK) == 0 && l2_slot_absent(ctx, start - 1)) {
      start -= VMM_L2_ENTRY_COUNT;
      continue;
    }
    if (page_run_key(ctx, start - 1) != key) break;
    start--;
  }

  /* Extend forward. */
  uint64_t end = vpn + 1;
  while (end < VMM_TOTAL_PAGE_COUNT) {
    if (hole && (end & VMM_L2_INDEX_MASK) == 0 && l2_slot_absent(ctx, end)) {
      end += VMM_L2_ENTRY_COUNT;
      continue;
    }
    if (page_run_key(ctx, end) != key) break;
    end++;
  }

  info->base_gva = start << VMM_PAGE_BITS;
  info->size = (end - start) << VMM_PAGE_BITS;
  info->is_mapped = !hole;
  info->perms = hole ? (uint32_t)VMM_PERM_NONE : (uint32_t)(key & VMM_PTE_PERM_MASK);
  return OK;
}

/* ------------------------------------------------------------------ */
/* Access: validate-then-copy core shared by the block and cross-page  */
/* paths. Operates on the L1 pointer so the header's inline helpers    */
/* can reach it without a context.                                     */
/* ------------------------------------------------------------------ */

static void fill_out_of_range_fault(VMM_Fault *fault, uint64_t gva, uint32_t perms) {
  fault->kind = VMM_FAULT_OUT_OF_RANGE;
  fault->required_perms = perms;
  fault->gva = gva;
}

/* Translates the first byte of every page in [gva, gva + size). */
static bool validate_byte_range(const uint64_t *l1, uint64_t gva, uint64_t size, uint32_t perms,
                                VMM_Fault *fault) {
  if (!is_valid_byte_range(gva, size)) {
    fill_out_of_range_fault(fault, gva, perms);
    return false;
  }
  const uint64_t end = gva + size;
  uint64_t cursor = gva;
  while (cursor < end) {
    if (!vmm_translate_inline(l1, cursor, perms, fault)) return false;
    cursor = (cursor & ~VMM_PAGE_OFFSET_MASK) + VMM_PAGE_SIZE;
  }
  return true;
}

/* Copies page by page. Range already validated with the right perms. */
static void copy_guest_to_host(const uint64_t *l1, uint64_t gva, void *out, uint64_t size) {
  uint8_t *dst = (uint8_t *)out;
  uint64_t cursor = gva;
  uint64_t remaining = size;
  while (remaining > 0) {
    VMM_Fault unused;
    const uint8_t *host = vmm_translate_inline(l1, cursor, VMM_PERM_R, &unused);
    SWITCH_ASSERT(host != NULL, "copy_guest_to_host: range was not validated");
    const uint64_t page_left = VMM_PAGE_SIZE - (cursor & VMM_PAGE_OFFSET_MASK);
    const uint64_t chunk = remaining < page_left ? remaining : page_left;
    memcpy(dst, host, (size_t)chunk);
    dst += chunk;
    cursor += chunk;
    remaining -= chunk;
  }
}

static void copy_host_to_guest(const uint64_t *l1, uint64_t gva, const void *src, uint64_t size) {
  const uint8_t *from = (const uint8_t *)src;
  uint64_t cursor = gva;
  uint64_t remaining = size;
  while (remaining > 0) {
    VMM_Fault unused;
    uint8_t *host = vmm_translate_inline(l1, cursor, VMM_PERM_W, &unused);
    SWITCH_ASSERT(host != NULL, "copy_host_to_guest: range was not validated");
    const uint64_t page_left = VMM_PAGE_SIZE - (cursor & VMM_PAGE_OFFSET_MASK);
    const uint64_t chunk = remaining < page_left ? remaining : page_left;
    memcpy(host, from, (size_t)chunk);
    from += chunk;
    cursor += chunk;
    remaining -= chunk;
  }
}

bool vmm_read_cross_page(const uint64_t *l1, uint64_t gva, void *out, uint32_t size,
                         VMM_Fault *fault) {
  if (!validate_byte_range(l1, gva, size, VMM_PERM_R, fault)) return false;
  copy_guest_to_host(l1, gva, out, size);
  return true;
}

bool vmm_write_cross_page(const uint64_t *l1, uint64_t gva, const void *src, uint32_t size,
                          VMM_Fault *fault) {
  if (!validate_byte_range(l1, gva, size, VMM_PERM_W, fault)) return false;
  copy_host_to_guest(l1, gva, src, size);
  return true;
}

/* ------------------------------------------------------------------ */
/* Checked access wrappers.                                            */
/* ------------------------------------------------------------------ */

static Error record_fault(VMM_Context *ctx, const VMM_Fault *fault) {
  ctx->last_fault = *fault;
  switch (fault->kind) {
  case VMM_FAULT_OUT_OF_RANGE:
    return ERR(RESULT_MEMORY_FAULT, "vmm: guest address outside the address space");
  case VMM_FAULT_UNMAPPED:
    return ERR(RESULT_MEMORY_FAULT, "vmm: guest address unmapped");
  case VMM_FAULT_PERMISSION:
    return ERR(RESULT_MEMORY_FAULT, "vmm: guest access permission denied");
  case VMM_FAULT_NONE:
  default:
    return ERR(RESULT_MEMORY_FAULT, "vmm: fault");
  }
}

#define VMM_DEFINE_CHECKED_READ(bits, Type)                                    \
  Error vmm_read##bits(VMM_Context *ctx, uint64_t gva, Type *out) {            \
    if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_read: ctx is NULL");    \
    if (!out) return ERR(RESULT_INVALID_ARGUMENT, "vmm_read: out is NULL");    \
    VMM_Fault fault;                                                           \
    if (!vmm_read##bits##_inline(ctx->l1, gva, out, &fault)) {                 \
      return record_fault(ctx, &fault);                                        \
    }                                                                          \
    return OK;                                                                 \
  }

#define VMM_DEFINE_CHECKED_WRITE(bits, Type)                                   \
  Error vmm_write##bits(VMM_Context *ctx, uint64_t gva, Type value) {          \
    if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_write: ctx is NULL");   \
    VMM_Fault fault;                                                           \
    if (!vmm_write##bits##_inline(ctx->l1, gva, value, &fault)) {              \
      return record_fault(ctx, &fault);                                        \
    }                                                                          \
    return OK;                                                                 \
  }

VMM_DEFINE_CHECKED_READ(8, uint8_t)
VMM_DEFINE_CHECKED_READ(16, uint16_t)
VMM_DEFINE_CHECKED_READ(32, uint32_t)
VMM_DEFINE_CHECKED_READ(64, uint64_t)
VMM_DEFINE_CHECKED_WRITE(8, uint8_t)
VMM_DEFINE_CHECKED_WRITE(16, uint16_t)
VMM_DEFINE_CHECKED_WRITE(32, uint32_t)
VMM_DEFINE_CHECKED_WRITE(64, uint64_t)

#undef VMM_DEFINE_CHECKED_READ
#undef VMM_DEFINE_CHECKED_WRITE

Error vmm_read_block(VMM_Context *ctx, uint64_t gva, void *out, uint64_t size) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_read_block: ctx is NULL");
  if (size == 0) return OK;
  if (!out) return ERR(RESULT_INVALID_ARGUMENT, "vmm_read_block: out is NULL");
  VMM_Fault fault;
  if (!validate_byte_range(ctx->l1, gva, size, VMM_PERM_R, &fault)) {
    return record_fault(ctx, &fault);
  }
  copy_guest_to_host(ctx->l1, gva, out, size);
  return OK;
}

Error vmm_write_block(VMM_Context *ctx, uint64_t gva, const void *src, uint64_t size) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_write_block: ctx is NULL");
  if (size == 0) return OK;
  if (!src) return ERR(RESULT_INVALID_ARGUMENT, "vmm_write_block: src is NULL");
  VMM_Fault fault;
  if (!validate_byte_range(ctx->l1, gva, size, VMM_PERM_W, &fault)) {
    return record_fault(ctx, &fault);
  }
  copy_host_to_guest(ctx->l1, gva, src, size);
  return OK;
}

Error vmm_guest_to_host(VMM_Context *ctx, uint64_t gva, uint64_t size, uint32_t required_perms,
                        void **host_ptr) {
  if (!ctx) return ERR(RESULT_INVALID_ARGUMENT, "vmm_guest_to_host: ctx is NULL");
  if (!host_ptr) return ERR(RESULT_INVALID_ARGUMENT, "vmm_guest_to_host: host_ptr is NULL");
  if (size == 0) return ERR(RESULT_INVALID_ARGUMENT, "vmm_guest_to_host: size is 0");
  if (required_perms == 0 || (required_perms & ~(uint32_t)VMM_PERM_ALL) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "vmm_guest_to_host: required_perms must be a non-empty subset of VMM_PERM_ALL");
  }

  VMM_Fault fault;
  if (!is_valid_byte_range(gva, size)) {
    fill_out_of_range_fault(&fault, gva, required_perms);
    return record_fault(ctx, &fault);
  }

  uint8_t *first = vmm_translate_inline(ctx->l1, gva, required_perms, &fault);
  if (!first) return record_fault(ctx, &fault);

  /* Every further page must be mapped with the perms AND sit exactly
   * where a contiguous run would put it. */
  const uint64_t end = gva + size;
  uint64_t cursor = (gva & ~VMM_PAGE_OFFSET_MASK) + VMM_PAGE_SIZE;
  while (cursor < end) {
    const uint8_t *host = vmm_translate_inline(ctx->l1, cursor, required_perms, &fault);
    if (!host) return record_fault(ctx, &fault);
    if (host != first + (cursor - gva)) {
      return ERR(RESULT_NOT_CONTIGUOUS, "vmm_guest_to_host: range spans non-adjacent host pages");
    }
    cursor += VMM_PAGE_SIZE;
  }

#ifdef VMM_DEBUG_BORROWS
  debug_record_borrow(ctx, gva, size);
#endif
  *host_ptr = first;
  return OK;
}
