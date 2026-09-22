#include "hle/hle.h"
#include "common/assert.h"
#include "common/log.h"
#include "hle/kernel/svc_memory.h"

#include <stddef.h>

void hle_context_init(HLE_Context *context, const CPU_Backend *backend,
                      VMM_Context *vmm, Process *process, Page_Allocator *pages)
{
  SWITCH_ASSERT_ALWAYS(context != NULL, "hle_context_init: context is NULL");
  SWITCH_ASSERT_ALWAYS(backend != NULL, "hle_context_init: backend is NULL");
  context->cpu_backend = backend;
  context->vmm = vmm;
  context->process = process;
  context->pages = pages;
  context->svc_call_count = 0;
}

static const char *svc_name(uint32_t swi)
{
  switch (swi)
  {
  case 0x01:
    return "SetHeapSize";
  case 0x02:
    return "SetMemoryPermission";
  case 0x03:
    return "SetMemoryAttribute";
  case 0x04:
    return "MapMemory";
  case 0x05:
    return "UnmapMemory";
  case 0x06:
    return "QueryMemory";
  case 0x08:
    return "CreateThread";
  case 0x09:
    return "StartThread";
  case 0x0A:
    return "ExitThread";
  case 0x0B:
    return "SleepThread";
  case 0x0C:
    return "GetThreadPriority";
  case 0x11:
    return "SignalEvent";
  case 0x12:
    return "ClearEvent";
  case 0x13:
    return "MapSharedMemory";
  case 0x14:
    return "UnmapSharedMemory";
  case 0x15:
    return "CreateTransferMemory";
  case 0x18:
    return "WaitSynchronization";
  case 0x19:
    return "CancelSynchronization";
  case 0x1A:
    return "ArbitrateLock";
  case 0x1B:
    return "ArbitrateUnlock";
  case 0x1C:
    return "WaitProcessWideKeyAtomic";
  case 0x1D:
    return "SignalProcessWideKey";
  case 0x1F:
    return "ConnectToNamedPort";
  case 0x21:
    return "SendSyncRequest";
  case 0x22:
    return "SendSyncRequestWithUserBuffer";
  case 0x26:
    return "CloseHandle";
  case 0x29:
    return "GetInfo";
  default:
    return "<unknown>";
  }
}

void hle_on_svc(CPU_State *cpu_state, uint32_t swi, void *userdata)
{
  HLE_Context *context = (HLE_Context *)userdata;
  SWITCH_ASSERT_ALWAYS(context != NULL, "hle_on_svc: context is NULL");
  SWITCH_ASSERT_ALWAYS(context->cpu_backend != NULL, "hle_on_svc: backend is NULL");

  const CPU_Backend *cpu = context->cpu_backend;
  CPU_Register_File *regs = cpu->get_register_file(cpu_state);

  context->svc_call_count++;

  /* Handlers below that call vmm_guest_to_host bracket each individual
   * call in its own vmm_borrow_scope_begin/end, right where it happens
   * (svc_memory.c) - NOT one scope around this whole dispatch. A
   * dispatch-wide scope was tried first and immediately hit vmm's debug
   * borrow cap (VMM_DEBUG_MAX_BORROWS=64, vmm.c): SetHeapSize's shrink
   * path can call vmm_guest_to_host once per released page, hundreds of
   * times in one call, and the cap exists to catch a handler that LEAKS
   * borrows, not to size a loop's borrow budget. Scoping tightly around
   * each call keeps at most one borrow live at a time regardless of how
   * many pages a handler walks. */

  /* §12's dispatch table, Phase 1 slice: the four memory SVCs this
   * checkbox implements (thin layers over vmm/process/pages - see
   * hle/kernel/svc_memory.h) plus the §12 unimplemented-surface policy
   * default arm for everything else: log, then HLE_RESULT_NOT_IMPLEMENTED
   * in W0. */
  switch (swi)
  {
  case 0x01:
    hle_svc_set_heap_size(context, cpu_state);
    break;
  case 0x04:
    hle_svc_map_memory(context, cpu_state);
    break;
  case 0x05:
    hle_svc_unmap_memory(context, cpu_state);
    break;
  case 0x06:
    hle_svc_query_memory(context, cpu_state);
    break;
  default:
    log_warn("[hle] SVC 0x%02x (%s) at PC 0x%016llx - unimplemented",
             swi,
             svc_name(swi),
             (unsigned long long)regs->pc);
    regs->x[0] = HLE_RESULT_NOT_IMPLEMENTED;
    break;
  }
}

void hle_on_undefined(CPU_State *cpu_state, uint32_t instruction, void *userdata)
{
  HLE_Context *context = (HLE_Context *)userdata;
  const uint64_t pc = context && context->cpu_backend
                          ? context->cpu_backend->get_pc(cpu_state)
                          : 0;
  log_error("[hle] undefined instruction 0x%08x at PC 0x%016llx",
            instruction,
            (unsigned long long)pc);
}
