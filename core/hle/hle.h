/**
 * HLE dispatcher. See docs/DESIGN.md section 12 for the priority list and
 * the dispatch table this file's switch mirrors.
 *
 * The dispatcher:
 *   - owns an HLE_Context: the active CPU backend vtable plus the shared
 *     state every SVC handler needs (the softmmu, the loaded process, the
 *     guest physical page allocator) - the same "environment bundle" shape
 *     as Thread_Env (thread.h), so handlers stay plain functions over data
 *     they don't own.
 *   - intercepts every SVC via the CPU backend's svc handler
 *   - for an SVC with a real handler (svc_memory.h and friends as they
 *     land), dispatches to it; for everything else, logs the syscall id and
 *     writes HLE_RESULT_NOT_IMPLEMENTED back into X0 (§12 unimplemented-
 *     surface policy)
 *
 * `vmm`/`process`/`pages` are pointers into the owning Emulator's fields
 * (see emulator.c), stored at hle_context_init() time before the process is
 * necessarily loaded - same pattern as `out->cpu_backend->create(out->vmm,
 * &out->hle)` already relies on elsewhere in emulator_create(). A handler
 * that touches `process` is only ever reached once emulator_load_program()
 * has populated it; nothing here re-checks that at dispatch time, matching
 * how the rest of HLE trusts the caller's lifecycle contract.
 */
#ifndef SWITCH_HLE_HLE_H
#define SWITCH_HLE_HLE_H

#include "common/vmm.h"
#include "cpu/cpu.h"
#include "hle/kernel/page_allocator.h"
#include "hle/kernel/process.h"

/* Switch OS result codes (subset relevant to Phase 0). Horizon Result
 * encoding: (description << 9) | module. Module 1 is the kernel - these
 * constants are genuine kernel results, e.g. 0xE401 = (114 << 9) | 1.
 * Service-specific results use their own module (§12). */
#define HLE_RESULT_SUCCESS 0x00000000u
#define HLE_RESULT_NOT_IMPLEMENTED 0xF601u
#define HLE_RESULT_INVALID_HANDLE 0xE401u
#define HLE_RESULT_INVALID_POINTER 0xCC01u /* KernelError_InvalidAddress=102 */
/* Was 0x1A01 (module=13, not a real kernel description) until the memory
 * SVCs (svc_memory.h) became the first code to actually write this into
 * a guest register - verified against libnx's result.h
 * (KernelError_OutOfMemory=104) and corrected here rather than shipped
 * wrong the first time it became observable. */
#define HLE_RESULT_OUT_OF_MEMORY 0xD001u /* KernelError_OutOfMemory=104 */
#define HLE_RESULT_NOT_FOUND 0xE002u
#define HLE_RESULT_ALREADY_EXISTS 0xFA02u
/* Added for the memory SVCs (svc_memory.h); same libnx result.h source. */
#define HLE_RESULT_INVALID_SIZE 0xCA01u          /* KernelError_InvalidSize=101 */
#define HLE_RESULT_INVALID_MEMORY_STATE 0xD401u  /* KernelError_InvalidMemoryState=106, aka InvalidCurrentMemory */
#define HLE_RESULT_INVALID_MEMORY_RANGE 0xDC01u  /* KernelError_InvalidMemoryRange=110 */

#define HLE_MAKE_RESULT(module, description) \
  ((uint32_t)(((description) << 9) | ((module) & 0x1FF)))

typedef struct HLE_Context
{
  const CPU_Backend *cpu_backend;
  VMM_Context *vmm;      /* softmmu (§5); the only guest-memory gateway memory SVCs use */
  Process *process;      /* the loaded process (§12); valid once emulator_load_program() has run */
  Page_Allocator *pages; /* guest physical pages (§4), shared with the bootstrap */
  uint64_t svc_call_count;
} HLE_Context;

void hle_context_init(HLE_Context *context, const CPU_Backend *backend,
                      VMM_Context *vmm, Process *process, Page_Allocator *pages);

/* CPU_SVC_Handler-compatible entry point. `swi` is the SVC instruction's
 * immediate - the actual Horizon syscall id. It is NOT in X8; that is the
 * Linux ARM64 convention and does not apply here (§12). */
void hle_on_svc(CPU_State *cpu_state, uint32_t swi, void *userdata);

/* CPU_Undefined_Handler-compatible entry point. */
void hle_on_undefined(CPU_State *cpu_state, uint32_t instruction, void *userdata);

#endif /* SWITCH_HLE_HLE_H */
