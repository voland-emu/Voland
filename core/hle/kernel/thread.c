/**
 * Guest thread objects. See thread.h for the contract; docs/DESIGN.md §7,
 * §8, §12.
 */
#include "hle/kernel/thread.h"

#include <string.h>

Error thread_create(const Thread_Env *env, const Thread_Create_Params *params,
                    Guest_Thread *out) {
  if (!env || !params || !out || !env->backend || !env->vmm || !env->tls || !env->hle) {
    return ERR(RESULT_INVALID_ARGUMENT, "thread_create: NULL argument");
  }
  if (params->priority > THREAD_PRIORITY_LOWEST) {
    return ERR(RESULT_INVALID_ARGUMENT, "thread_create: priority out of range");
  }
  if (params->preferred_core >= THREAD_CORE_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "thread_create: core out of range");
  }
  if ((params->stack_top % THREAD_STACK_ALIGN) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "thread_create: stack top not 16-byte aligned");
  }
  memset(out, 0, sizeof(*out));

  uint64_t tls_gva = 0;
  Error err = tls_allocate(env->tls, &tls_gva);
  if (!error_is_ok(err)) return err;

  CPU_State *state = env->backend->create(env->vmm, env->hle);
  if (!state) {
    (void)tls_free(env->tls, tls_gva);
    return ERR(RESULT_OUT_OF_MEMORY, "thread_create: backend could not create a state");
  }
  env->backend->set_svc_handler(state, hle_on_svc);
  env->backend->set_undefined_handler(state, hle_on_undefined);

  /* Start state: X0 = argument, everything else clean. */
  for (uint8_t reg = CPU_REG_X0; reg <= CPU_REG_X30; reg++) {
    env->backend->set_reg(state, reg, 0);
  }
  env->backend->set_reg(state, CPU_REG_X0, params->argument);
  env->backend->set_sp(state, params->stack_top);
  env->backend->set_pc(state, params->entry_point);
  env->backend->set_pstate(state, 0);
  /* The tpidrro_el0 plumbing (§12): written once per thread, read by
   * the backend on every `mrs tpidrro_el0` and by IPC via tls_gva. */
  env->backend->set_sys_reg(state, CPU_SYSREG_TPIDRRO_EL0, tls_gva);

  out->cpu_state = state;
  out->tls_gva = tls_gva;
  out->entry_point = params->entry_point;
  out->stack_top = params->stack_top;
  out->priority = params->priority;
  out->preferred_core = params->preferred_core;
  return OK;
}

void thread_destroy(const Thread_Env *env, Guest_Thread *thread) {
  if (!env || !thread) return;
  if (thread->cpu_state && env->backend) {
    env->backend->destroy(thread->cpu_state);
  }
  if (thread->cpu_state && env->tls) {
    (void)tls_free(env->tls, thread->tls_gva);
  }
  memset(thread, 0, sizeof(*thread));
}
