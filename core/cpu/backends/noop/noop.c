/**
 * Implementation of the no-op CPU backend. See docs/DESIGN.md section 9.
 * Executes zero ARM instructions but faithfully tracks register state and
 * honors the bounded exit-reason contract so HLE services, loader code,
 * the scheduler (§7, once it lands), and worker plumbing can all be
 * validated before the interpreter/recompiler exists.
 */
#include "cpu/backends/noop/noop.h"
#include "common/assert.h"
#include "common/log.h"

#include <stdlib.h>

typedef struct NoopState
{
  CPU_Register_File regs;

  uint64_t fault_address;
  uint64_t cycles_consumed;

  VMM_Context *vmm; /* unused until Phase 1 (§5); the no-op backend never
                      * performs a guest memory access. */
  void *userdata;
  CPU_SVC_Handler svc_handler;
  CPU_Undefined_Handler undefined_handler;
  CPU_Breakpoint_Handler breakpoint_handler;
} NoopState;

static CPU_State *noop_create(VMM_Context *vmm, void *userdata)
{
  NoopState *state = (NoopState *)calloc(1, sizeof(NoopState));
  if (!state)
    return NULL;
  state->vmm = vmm;
  state->userdata = userdata;
  return (CPU_State *)state;
}

static void noop_destroy(CPU_State *state)
{
  free(state);
}

static CPU_ExitReason noop_run(CPU_State *state, uint64_t cycle_budget)
{
  NoopState *s = (NoopState *)state;
  /* Pretend the budget was consumed so virtual time advances and the
   * scheduler loop (§7) can be exercised end to end. */
  s->cycles_consumed = cycle_budget;
  log_debug("[cpu/noop] run() budget=%llu consumed, no instructions executed",
            (unsigned long long)cycle_budget);
  return CPU_EXIT_CYCLES_ELAPSED;
}

static CPU_ExitReason noop_step(CPU_State *state)
{
  NoopState *s = (NoopState *)state;
  s->cycles_consumed = 1;
  return CPU_EXIT_CYCLES_ELAPSED;
}

static uint64_t noop_get_fault_address(CPU_State *state)
{
  return ((NoopState *)state)->fault_address;
}

static uint64_t noop_get_cycles_consumed(CPU_State *state)
{
  return ((NoopState *)state)->cycles_consumed;
}

static uint64_t noop_get_reg(CPU_State *state, uint8_t index)
{
  SWITCH_ASSERT(index <= 30, "noop_get_reg: index out of range");
  return ((NoopState *)state)->regs.x[index];
}

static void noop_set_reg(CPU_State *state, uint8_t index, uint64_t value)
{
  SWITCH_ASSERT(index <= 30, "noop_set_reg: index out of range");
  ((NoopState *)state)->regs.x[index] = value;
}

static uint64_t noop_get_pc(CPU_State *state) { return ((NoopState *)state)->regs.pc; }
static void noop_set_pc(CPU_State *state, uint64_t v) { ((NoopState *)state)->regs.pc = v; }
static uint64_t noop_get_sp(CPU_State *state) { return ((NoopState *)state)->regs.sp; }
static void noop_set_sp(CPU_State *state, uint64_t v) { ((NoopState *)state)->regs.sp = v; }
static uint32_t noop_get_pstate(CPU_State *state) { return ((NoopState *)state)->regs.pstate; }
static void noop_set_pstate(CPU_State *state, uint32_t v) { ((NoopState *)state)->regs.pstate = v; }

static CPU_Register_File *noop_get_register_file(CPU_State *state)
{
  return &((NoopState *)state)->regs;
}

static uint64_t noop_get_sys_reg(CPU_State *state, uint32_t reg)
{
  (void)state;
  (void)reg;
  return 0;
}
static void noop_set_sys_reg(CPU_State *state, uint32_t reg, uint64_t value)
{
  (void)state;
  (void)reg;
  (void)value;
}

static void noop_invalidate_cache(CPU_State *state, uint64_t addr, uint64_t size)
{
  (void)state;
  (void)addr;
  (void)size;
}
static void noop_clear_cache(CPU_State *state) { (void)state; }

static void noop_set_svc_handler(CPU_State *state, CPU_SVC_Handler h)
{
  ((NoopState *)state)->svc_handler = h;
}
static void noop_set_undefined_handler(CPU_State *state, CPU_Undefined_Handler h)
{
  ((NoopState *)state)->undefined_handler = h;
}
static void noop_set_breakpoint_handler(CPU_State *state, CPU_Breakpoint_Handler h)
{
  ((NoopState *)state)->breakpoint_handler = h;
}

const CPU_Backend CPU_BACKEND_NOOP = {
    .create = noop_create,
    .destroy = noop_destroy,
    .run = noop_run,
    .step = noop_step,
    .get_fault_address = noop_get_fault_address,
    .get_cycles_consumed = noop_get_cycles_consumed,
    .get_reg = noop_get_reg,
    .set_reg = noop_set_reg,
    .get_pc = noop_get_pc,
    .set_pc = noop_set_pc,
    .get_sp = noop_get_sp,
    .set_sp = noop_set_sp,
    .get_pstate = noop_get_pstate,
    .set_pstate = noop_set_pstate,
    .get_register_file = noop_get_register_file,
    .get_sys_reg = noop_get_sys_reg,
    .set_sys_reg = noop_set_sys_reg,
    .invalidate_cache = noop_invalidate_cache,
    .clear_cache = noop_clear_cache,
    .set_svc_handler = noop_set_svc_handler,
    .set_undefined_handler = noop_set_undefined_handler,
    .set_breakpoint_handler = noop_set_breakpoint_handler,
    .name = "noop",
    .version = "1.0.0",
    .supports_jit = false,
};
