/**
 * Guest thread objects. See docs/DESIGN.md §7 (one CPU_State per guest
 * thread), §8 (the backend creates and owns the register file) and §12
 * (a thread's TLS block; tpidrro_el0 points at it).
 *
 * Phase 1 scope (§25 "TLS allocation + tpidrro_el0 plumbing"): the
 * object that owns a thread's execution state and TLS block, and the
 * one place that arms `tpidrro_el0`. thread_create is what the Phase 2
 * CreateThread SVC will call once it has validated its arguments against
 * the npdm and resolved the stack; the SVC itself, the handle table
 * entry, and every scheduler field of §7's Guest_Thread (run_state,
 * wake_at_ns, waiting_on, run-queue links) land with the scheduler in
 * Phase 2 - they are deliberately absent here rather than stubbed.
 *
 * Plumbing note: under green threading (§7) "set tpidrro_el0 on every
 * context switch" reduces to "every CPU_State carries its own value":
 * the scheduler switches threads by running a different state, and a
 * JIT-emitted `mrs tpidrro_el0` reads the per-thread state. So the
 * register is written once, here, through the backend's sys-reg
 * interface, and never touched again for the thread's lifetime.
 *
 * The main thread is the one thread not created here: the bootstrap
 * (process.h) allocates its TLS block from the same allocator and
 * process_enter_main_thread arms the emulator's pre-existing CPU_State
 * with the Horizon entry ABI (X1 = handle), which CreateThread threads
 * do not receive. Folding the main thread into a Guest_Thread is a
 * Phase 2 change that arrives with the scheduler owning all states.
 *
 * Stacks are the caller's: CreateThread receives an SP the guest already
 * mapped (its own heap or stack region), so thread_create takes a stack
 * top and maps nothing. This unit performs no guest memory access at
 * all; the TLS block's bytes are the allocator's (tls.h) and the IPC
 * layer's, both through vmm (rule 3).
 */
#ifndef SWITCH_HLE_KERNEL_THREAD_H
#define SWITCH_HLE_KERNEL_THREAD_H

#include <stdint.h>

#include "common/result.h"
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "hle/kernel/tls.h"

/* Horizon priority range, 0 highest .. 63 lowest. npdm.h bounds the
 * subset a title may use; the SVC enforces that, this unit enforces only
 * the architectural range. */
#define THREAD_PRIORITY_HIGHEST ((uint32_t)0)
#define THREAD_PRIORITY_LOWEST ((uint32_t)63)
/* Cores 0..2 run applications; core 3 is the system core (§7). */
#define THREAD_CORE_COUNT ((uint32_t)4)
/* AArch64 procedure call standard: SP is 16-byte aligned at entry. */
#define THREAD_STACK_ALIGN ((uint64_t)16)

/* What every thread needs from the process it belongs to. Borrowed
 * pointers; all outlive the threads. */
typedef struct Thread_Env {
  const CPU_Backend *backend;
  VMM_Context *vmm;
  TLS_Allocator *tls;
  HLE_Context *hle; /* the state's SVC/undefined handlers dispatch here */
} Thread_Env;

typedef struct Thread_Create_Params {
  uint64_t entry_point;    /* PC */
  uint64_t argument;       /* X0 */
  uint64_t stack_top;      /* SP; caller-mapped, THREAD_STACK_ALIGN-aligned */
  uint32_t priority;       /* THREAD_PRIORITY_HIGHEST .. THREAD_PRIORITY_LOWEST */
  uint32_t preferred_core; /* < THREAD_CORE_COUNT */
} Thread_Create_Params;

typedef struct Guest_Thread {
  CPU_State *cpu_state; /* owned; created by env->backend */
  uint64_t tls_gva;     /* owned TLS block (tls.h); tpidrro_el0 == this */
  uint64_t entry_point;
  uint64_t stack_top;
  uint32_t priority;
  uint32_t preferred_core;
} Guest_Thread;

/* Allocates a TLS block, creates a CPU_State bound to env->vmm with the
 * HLE SVC/undefined handlers installed (hle.h), and arms the start
 * state: X0 = argument, every other X register 0, SP = stack_top, PC =
 * entry_point, PSTATE = 0, tpidrro_el0 = the TLS block. The thread is
 * ready for backend->run() but nothing runs it until the Phase 2
 * scheduler.
 *   RESULT_INVALID_ARGUMENT NULL; priority or core out of range;
 *                           stack_top not THREAD_STACK_ALIGN-aligned
 *   RESULT_OUT_OF_MEMORY    no TLS block (tls.h) or the backend could
 *                           not create a state; nothing is left allocated */
Error thread_create(const Thread_Env *env, const Thread_Create_Params *params,
                    Guest_Thread *out);

/* Destroys the state and frees the TLS block; zeroes `thread`. Safe on a
 * zeroed Guest_Thread. */
void thread_destroy(const Thread_Env *env, Guest_Thread *thread);

#endif /* SWITCH_HLE_KERNEL_THREAD_H */
