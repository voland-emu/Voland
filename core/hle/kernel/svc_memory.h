/**
 * Memory SVCs: SetHeapSize, MapMemory, UnmapMemory, QueryMemory - a thin
 * layer over vmm (§5), the process's address space (address_space.h) and
 * the guest physical page allocator (page_allocator.h). Phase 1, §25
 * checkbox 5; §12 priority list item 2.
 *
 * Scope. Exactly these four SVCs - the ones the checkbox and the priority
 * list both name. SetMemoryPermission (0x02) and SetMemoryAttribute (0x03)
 * are separate SVCs and stay HLE_RESULT_NOT_IMPLEMENTED in hle.c's
 * dispatch table until their own task lands; GetInfo (0x29) likewise -
 * it is grouped under "Handles / Info" in §12's dispatch table, not
 * "Memory", and needs the handle table this phase does not have yet.
 *
 * Register ABI. Verified against libnx's svc.s/svc.h (the public SDK
 * every homebrew and every game links against - documentation of the
 * kernel's contract, not another emulator's implementation; CLAUDE.md
 * rule 2 is about not copying emulator code, and does not reach reading
 * the platform's own published ABI). §12's dispatch table comment says
 * arguments arrive in W0-W7/X0-X7 in C parameter order; that is NOT
 * uniformly true for these four - two of them have an out-pointer as
 * their first C parameter, and libnx's raw asm stubs shift the real
 * scalar argument down into the register the pointer would have occupied
 * rather than passing the (meaningless, caller-local) pointer to the
 * kernel at all:
 *
 *   SetHeapSize   C: Result(void** out_addr, u64 size)
 *                 in:  X1 = size (must be a PROCESS_HEAP_SIZE_GRANULE
 *                      (0x200000) multiple - libnx's own doc comment)
 *                 out: W0 = Result, X1 = heap base address
 *                 (X0 is never read by the kernel; libnx's stub only
 *                 uses it locally, to remember where to store X1 after
 *                 the svc returns)
 *
 *   MapMemory     C: Result(void* dst_addr, void* src_addr, u64 size)
 *                 in:  X0 = dst_addr, X1 = src_addr, X2 = size
 *                 out: W0 = Result
 *
 *   UnmapMemory   C: Result(void* dst_addr, void* src_addr, u64 size)
 *                 in:  X0 = dst_addr, X1 = src_addr, X2 = size
 *                 out: W0 = Result
 *
 *   QueryMemory   C: Result(MemoryInfo* meminfo_ptr, u32* pageinfo, u64 addr)
 *                 in:  X0 = meminfo_ptr (GUEST address; the handler writes
 *                      an HLE_Memory_Info there via vmm_write_block),
 *                      X2 = addr
 *                 out: W0 = Result, W1 = pageinfo (always 0 - Horizon
 *                      reserves it; Voland does the same)
 *                 (X1 is never read by the kernel, same reasoning as
 *                 SetHeapSize's X0)
 *
 * MapMemory/UnmapMemory semantics (libnx's doc comment on svcMapMemory,
 * corroborated by the switchbrew SVC page): "Mainly used for adding guard
 * pages around stack. Source range gets reprotected to Perm_None (it can
 * no longer be accessed), and MemAttr_IsBorrowed is set in the source
 * MemoryAttribute." Voland's version:
 *   - dst must lie entirely in address_space.stack, every page currently
 *     UNMAPPED.
 *   - src must lie entirely in address_space.heap, every page currently
 *     mapped and host-contiguous (vmm_guest_to_host both validates this
 *     and hands back the host pointer this handler recovers src's
 *     guest_pa from - see svc_memory.c; vmm has no "what guest_pa backs
 *     this gva" accessor, and adding one just for this one caller was
 *     rejected in favor of reusing vmm_guest_to_host + layout_get()
 *     ->guest_ram_base, which is public vmm surface already).
 *   - src is vmm_reprotect()'d to VMM_PERM_NONE (still mapped, no longer
 *     accessible - matches "reprotected to Perm_None"); dst is vmm_map()'d
 *     onto the same guest_pa with VMM_PERM_RW (the heap's own permission).
 *   - the pair is recorded in process->heap_borrows so UnmapMemory can
 *     find it and QueryMemory can answer for either half.
 *   - KNOWN SIMPLIFICATION, flagged for review rather than silently
 *     shipped (CLAUDE.md "deviations update the doc"): real Horizon
 *     allows UnmapMemory to release part of a previously mapped range.
 *     Voland's UnmapMemory requires an exact (dst, src, size) match
 *     against a recorded Memory_Borrow and fails RESULT_INVALID_ARGUMENT
 *     otherwise. No title is known to need partial unmap for the guard-
 *     page use case; revisit if one surfaces.
 *
 * QueryMemory's HLE_Memory_Info.type - ANOTHER FLAGGED SIMPLIFICATION:
 * Horizon's real MemoryType enum has ~30 values (module code, transfer
 * memory, IPC buffers, kernel stack, ...) covering kernel objects Voland
 * does not model yet. HLE_Memory_Type below reuses Horizon's own numeric
 * encoding for the values Voland CAN currently tell apart (so a title
 * that decodes the raw integer is not lied to for those), derived by
 * which address_space.h region a queried gva falls in, and reports
 * HLE_MEMTYPE_UNMAPPED for anything outside every region. `attr` is
 * always reported as 0: MemAttr_IsBorrowed on a MapMemory source is not
 * modeled (the source's `perm` field alone already tells a title the
 * range is inaccessible, which is the observable behavior that matters
 * most). Extend both when a title's behavior demands more precision.
 */
#ifndef SWITCH_HLE_KERNEL_SVC_MEMORY_H
#define SWITCH_HLE_KERNEL_SVC_MEMORY_H

#include <stdint.h>

#include "cpu/cpu.h"
#include "hle/hle.h"

/* Subset of Horizon's MemoryType enum (libnx svc.h) Voland can currently
 * tell apart from address_space.h's region carve-up. Values match
 * Horizon's numbering where a Voland region corresponds 1:1 to a real
 * MemoryType; there is no entry here for a Horizon type Voland cannot
 * yet distinguish. */
typedef enum HLE_Memory_Type {
  HLE_MEMTYPE_UNMAPPED = 0x00,       /* address_space.h: outside every region, or unmapped within one */
  HLE_MEMTYPE_CODE_STATIC = 0x03,    /* address_space.code */
  HLE_MEMTYPE_HEAP = 0x05,           /* address_space.heap, not currently borrowed out */
  HLE_MEMTYPE_WEIRD_MAPPED_MEM = 0x07, /* address_space.heap, borrowed out via MapMemory (Perm_None) */
  HLE_MEMTYPE_MAPPED_MEMORY = 0x0B, /* address_space.stack, a live MapMemory alias */
  HLE_MEMTYPE_THREAD_LOCAL = 0x0C,  /* address_space.tls_io */
  HLE_MEMTYPE_NORMAL = 0x02,        /* address_space.stack, not a MapMemory alias (plain stack) */
} HLE_Memory_Type;

/* Byte-for-byte Horizon's MemoryInfo (libnx svc.h): QueryMemory writes
 * this into guest memory at the caller-supplied address via
 * vmm_write_block, so the layout must match what a title's own struct
 * definition expects. ipc_refcount/device_refcount are always 0 - Voland
 * has no IPC buffers or device mappings yet for this to count. */
typedef struct HLE_Memory_Info {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
  uint32_t attr;
  uint32_t perm;
  uint32_t ipc_refcount;
  uint32_t device_refcount;
  uint32_t padding;
} HLE_Memory_Info;

/* CPU_SVC_Handler-shaped: called from hle_on_svc's dispatch (§12) with the
 * same (context, cpu_state) pair hle_on_svc itself received. Every
 * handler reads its arguments from
 * context->cpu_backend->get_register_file(cpu_state) per the ABI table
 * above and writes its Result to X0 (additional outputs per that table).
 * context and cpu_state are never NULL - hle_on_svc guarantees both, the
 * same contract hle_on_svc itself is held to; context->process is assumed
 * to be the caller's loaded process (§12: only reachable once
 * emulator_load_program() has run). */
void hle_svc_set_heap_size(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_map_memory(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_unmap_memory(HLE_Context *context, CPU_State *cpu_state);
void hle_svc_query_memory(HLE_Context *context, CPU_State *cpu_state);

#endif /* SWITCH_HLE_KERNEL_SVC_MEMORY_H */
