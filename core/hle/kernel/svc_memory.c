/**
 * Memory SVCs. See svc_memory.h for the register ABI (verified against
 * libnx) and the two flagged simplifications (UnmapMemory exact-range-only,
 * QueryMemory's partial MemoryType/MemoryAttribute fidelity).
 */
#include "hle/kernel/svc_memory.h"

#include "common/assert.h"
#include "common/layout.h"
#include "common/log.h"

#include <stddef.h>

static bool ranges_overlap(uint64_t a_base, uint64_t a_size, uint64_t b_base, uint64_t b_size) {
  return a_base < b_base + b_size && b_base < a_base + a_size;
}

/* True if `gva` falls inside the source or destination half of a
 * currently-live svcMapMemory borrow. Used by classify_type() to tell a
 * plain stack/heap page apart from one that is a borrow's other half -
 * something vmm's own perm bits cannot do alone (see below). */
static const Memory_Borrow *find_borrow_by_dst(const Process *process, uint64_t gva) {
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    const Address_Region dst_region = {b->dst_base, b->size};
    if (address_region_contains(&dst_region, gva, 1)) return b;
  }
  return NULL;
}

static bool borrow_overlaps_src(const Process *process, uint64_t base, uint64_t size) {
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    if (ranges_overlap(base, size, b->src_base, b->size)) return true;
  }
  return false;
}

/* NOTE ON PARTIAL FIDELITY (see svc_memory.h's top comment for the full
 * reasoning): Horizon's real MemoryType has ~30 values; the ones missing
 * here all describe kernel objects (transfer memory, IPC buffers, module
 * code, ...) that no SVC implemented before Phase 5+ can create, so no
 * Phase 1 title can construct a gva that would need one of them - this
 * is a closure argument over what is reachable right now, not a shortcut
 * with a known-broken case. The heap/stack split below is the one place
 * vmm's own state (is_mapped + perms) is NOT enough to answer correctly:
 * a MapMemory dst alias and a plain stack page are both mapped RW, and a
 * MapMemory'd-away heap source and a plain heap page would both read as
 * "mapped" if perms were VMM_PERM_NONE for some other reason (they are
 * not, today, but the borrow table is the honest source of truth rather
 * than perms as a proxy for it). */
static uint32_t classify_type(const Process *process, uint64_t gva, bool is_mapped) {
  if (!is_mapped) return HLE_MEMTYPE_UNMAPPED;
  const Address_Space *as = &process->address_space;
  if (address_region_contains(&as->code, gva, 1)) return HLE_MEMTYPE_CODE_STATIC;
  if (address_region_contains(&as->heap, gva, 1)) {
    return borrow_overlaps_src(process, gva, 1) ? HLE_MEMTYPE_WEIRD_MAPPED_MEM : HLE_MEMTYPE_HEAP;
  }
  if (address_region_contains(&as->stack, gva, 1)) {
    return find_borrow_by_dst(process, gva) ? HLE_MEMTYPE_MAPPED_MEMORY : HLE_MEMTYPE_NORMAL;
  }
  if (address_region_contains(&as->tls_io, gva, 1)) return HLE_MEMTYPE_THREAD_LOCAL;
  /* Falls through for the alias region (nothing maps it until
   * svcMapPhysicalMemory exists) and anything outside every region. */
  return HLE_MEMTYPE_UNMAPPED;
}

static bool vmm_run_covers(const VMM_Region_Info *info, uint64_t gva, uint64_t size) {
  const Address_Region run = {info->base_gva, info->size};
  return address_region_contains(&run, gva, size);
}

/* Releases one page's physical backing. Its own borrow scope, not the
 * caller's: vmm's debug borrow cap (VMM_DEBUG_MAX_BORROWS=64, vmm.c)
 * exists to catch a handler that LEAKS borrows, not to size a loop that
 * calls this hundreds of times - scoping tightly here keeps at most one
 * borrow live at a time no matter how many pages the caller walks. */
static void free_one_page(VMM_Context *vmm, Page_Allocator *pages, uint64_t gva) {
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  const Error got = vmm_guest_to_host(vmm, gva, VMM_PAGE_SIZE, VMM_PERM_R, &host_ptr);
  SWITCH_ASSERT_ALWAYS(error_is_ok(got), "free_one_page: heap page not readable");
  const uint64_t guest_pa = (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base;
  vmm_borrow_scope_end(vmm);
  const Error freed = page_allocator_free(pages, guest_pa, 1);
  SWITCH_ASSERT_ALWAYS(error_is_ok(freed), "free_one_page: page_allocator_free failed");
}

/* Releases the physical pages backing [gva, gva + size), then unmaps the
 * whole range in one call. Every page must currently be mapped and
 * readable (both SetHeapSize's shrink path and process_teardown's heap
 * release only ever call this after confirming no live borrow overlaps
 * the range, so no page in it can be VMM_PERM_NONE); a violation is an
 * internal bug, not a bad guest argument, hence the asserts rather than
 * a Result.
 *
 * Fast path: try the whole range as ONE physical run first - the common
 * case, since a single SetHeapSize grow maps one page_allocator_allocate()
 * run in one vmm_map() call, so the range this releases is usually
 * exactly that one run. Only a range whose backing got fragmented across
 * a shrink-then-regrow (page_allocator.h's freelist) falls back to one
 * page at a time.
 *
 * Duplicated (not shared) between here and process.c's process_teardown,
 * which needs the identical logic: factoring a two-call-site helper out
 * into a new header felt like more ceremony than the ~20 lines of
 * duplication it would remove. Revisit if a third caller shows up. */
static void free_and_unmap_heap_range(VMM_Context *vmm, Page_Allocator *pages,
                                      uint64_t gva, uint64_t size) {
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  const Error got = vmm_guest_to_host(vmm, gva, size, VMM_PERM_R, &host_ptr);
  vmm_borrow_scope_end(vmm);
  if (error_is_ok(got)) {
    const uint64_t guest_pa = (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base;
    const Error freed = page_allocator_free(pages, guest_pa, size >> VMM_PAGE_BITS);
    SWITCH_ASSERT_ALWAYS(error_is_ok(freed), "free_and_unmap_heap_range: page_allocator_free failed");
  } else {
    for (uint64_t offset = 0; offset < size; offset += VMM_PAGE_SIZE) {
      free_one_page(vmm, pages, gva + offset);
    }
  }
  const Error unmapped = vmm_unmap(vmm, gva, size);
  SWITCH_ASSERT_ALWAYS(error_is_ok(unmapped), "free_and_unmap_heap_range: vmm_unmap failed");
}

void hle_svc_set_heap_size(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  const Address_Region *heap = &process->address_space.heap;

  /* Verified ABI (svc_memory.h): the requested size arrives in X1, not
   * X0 - libnx's stub never lets the kernel see X0 for this SVC. */
  const uint64_t requested = regs->x[1];

  if (requested % PROCESS_HEAP_SIZE_GRANULE != 0) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (requested > heap->size) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }

  if (requested > process->heap_size) {
    const uint64_t grow_bytes = requested - process->heap_size;
    uint64_t pa = 0;
    Error err = page_allocator_allocate(context->pages, grow_bytes >> VMM_PAGE_BITS, &pa);
    if (!error_is_ok(err)) {
      regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
      return;
    }
    err = vmm_map(context->vmm, heap->base + process->heap_size, pa, grow_bytes, VMM_PERM_RW);
    if (!error_is_ok(err)) {
      /* vmm_map validates its whole range before mutating anything, so
       * nothing to unwind there; just give the pages back. */
      const Error freed = page_allocator_free(context->pages, pa, grow_bytes >> VMM_PAGE_BITS);
      SWITCH_ASSERT_ALWAYS(error_is_ok(freed), "hle_svc_set_heap_size: rollback free failed");
      regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
      return;
    }
    process->heap_size = requested;
  } else if (requested < process->heap_size) {
    const uint64_t shrink_base = heap->base + requested;
    const uint64_t shrink_bytes = process->heap_size - requested;
    if (borrow_overlaps_src(process, shrink_base, shrink_bytes)) {
      regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
      return;
    }
    free_and_unmap_heap_range(context->vmm, context->pages, shrink_base, shrink_bytes);
    process->heap_size = requested;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = heap->base; /* fixed by address_space_init at bootstrap - no per-call ASLR to do */
}

void hle_svc_map_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  const uint64_t dst = regs->x[0];
  const uint64_t src = regs->x[1];
  const uint64_t size = regs->x[2];

  if (size == 0 || (size & VMM_PAGE_OFFSET_MASK) != 0 || (dst & VMM_PAGE_OFFSET_MASK) != 0 ||
      (src & VMM_PAGE_OFFSET_MASK) != 0) {
    regs->x[0] = HLE_RESULT_INVALID_SIZE;
    return;
  }
  if (!address_region_contains(&process->address_space.stack, dst, size) ||
      !address_region_contains(&process->address_space.heap, src, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }
  if (process->heap_borrow_count >= PROCESS_MAX_HEAP_BORROWS) {
    regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }

  VMM_Region_Info dst_info;
  Error err = vmm_query(vmm, dst, &dst_info);
  if (!error_is_ok(err) || dst_info.is_mapped || !vmm_run_covers(&dst_info, dst, size)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }

  /* Recovers src's backing guest_pa via the host pointer vmm hands back
   * for a validated, host-contiguous range (svc_memory.h's ABI note: vmm
   * has no direct gva->guest_pa accessor, and this reuses vmm_guest_to_host
   * plus layout_get() rather than adding one). The pointer itself is never
   * kept past this call - only the derived guest_pa integer is - matching
   * vmm.h's borrow-scope contract. Scoped tightly around just this call
   * (not the whole handler, and not by hle_on_svc - see hle.c). */
  vmm_borrow_scope_begin(vmm);
  void *host_ptr = NULL;
  err = vmm_guest_to_host(vmm, src, size, VMM_PERM_R, &host_ptr);
  vmm_borrow_scope_end(vmm);
  if (!error_is_ok(err)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }
  const uint64_t guest_pa = (uint64_t)(uintptr_t)host_ptr - layout_get()->guest_ram_base;

  err = vmm_reprotect(vmm, src, size, VMM_PERM_NONE);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_map_memory: reprotect of validated src failed");
  err = vmm_map(vmm, dst, guest_pa, size, VMM_PERM_RW);
  if (!error_is_ok(err)) {
    const Error undo = vmm_reprotect(vmm, src, size, VMM_PERM_RW);
    SWITCH_ASSERT_ALWAYS(error_is_ok(undo), "hle_svc_map_memory: rollback reprotect failed");
    regs->x[0] = HLE_RESULT_OUT_OF_MEMORY;
    return;
  }

  process->heap_borrows[process->heap_borrow_count++] =
      (Memory_Borrow){.dst_base = dst, .src_base = src, .size = size};
  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_unmap_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  const uint64_t dst = regs->x[0];
  const uint64_t src = regs->x[1];
  const uint64_t size = regs->x[2];

  int32_t index = -1;
  for (uint32_t i = 0; i < process->heap_borrow_count; i++) {
    const Memory_Borrow *b = &process->heap_borrows[i];
    if (b->dst_base == dst && b->src_base == src && b->size == size) {
      index = (int32_t)i;
      break;
    }
  }
  if (index < 0) {
    /* Voland requires an exact match against a recorded MapMemory call -
     * real Horizon allows a partial unmap, but libnx's thread.c (the only
     * real-world caller in the reference SDK) always unmaps the exact
     * triple threadCreate mapped, and no title is known to need more.
     * See svc_memory.h. */
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_STATE;
    return;
  }

  Error err = vmm_unmap(vmm, dst, size);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_unmap_memory: unmap of a tracked alias failed");
  err = vmm_reprotect(vmm, src, size, VMM_PERM_RW);
  SWITCH_ASSERT_ALWAYS(error_is_ok(err), "hle_svc_unmap_memory: reprotect of a tracked borrow failed");

  /* Swap-remove: heap_borrows has no meaningful order. */
  process->heap_borrows[(uint32_t)index] = process->heap_borrows[--process->heap_borrow_count];
  regs->x[0] = HLE_RESULT_SUCCESS;
}

void hle_svc_query_memory(HLE_Context *context, CPU_State *cpu_state) {
  CPU_Register_File *regs = context->cpu_backend->get_register_file(cpu_state);
  Process *process = context->process;
  VMM_Context *vmm = context->vmm;

  /* Verified ABI (svc_memory.h): X0 is the GUEST address to write the
   * MemoryInfo struct into, X2 is the address to query - X1 is never
   * read by the kernel for this SVC either (same reasoning as
   * SetHeapSize's X0: libnx's stub only uses it locally, afterward, to
   * store the kernel's returned pageinfo word). */
  const uint64_t out_gva = regs->x[0];
  const uint64_t query_addr = regs->x[2];

  VMM_Region_Info info;
  const Error err = vmm_query(vmm, query_addr, &info);
  if (!error_is_ok(err)) {
    regs->x[0] = HLE_RESULT_INVALID_MEMORY_RANGE;
    return;
  }

  const HLE_Memory_Info out = {
      .addr = info.base_gva,
      .size = info.size,
      .type = classify_type(process, query_addr, info.is_mapped),
      .attr = 0, /* MemAttr_IsBorrowed not modeled - see the note above classify_type() */
      .perm = info.perms,
      .ipc_refcount = 0,
      .device_refcount = 0,
      .padding = 0,
  };
  const Error wrote = vmm_write_block(vmm, out_gva, &out, sizeof(out));
  if (!error_is_ok(wrote)) {
    regs->x[0] = HLE_RESULT_INVALID_POINTER;
    return;
  }

  regs->x[0] = HLE_RESULT_SUCCESS;
  regs->x[1] = 0; /* pageinfo: reserved, always 0 on real hardware too */
}
