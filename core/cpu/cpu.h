/**
 * Abstract CPU backend interface. This is the most important abstraction
 * in the project: every backend (noop, interpreter, ballistic) implements
 * it exactly. Nothing outside backend code should care which backend is
 * active. See docs/DESIGN.md section 8.
 */
#ifndef SWITCH_CPU_CPU_H
#define SWITCH_CPU_CPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/vmm.h"

typedef struct CPU_State CPU_State;
typedef struct CPU_Backend CPU_Backend;

/* ------------------------------------------------------------------ */
/* Common architectural state, embedded by every backend. Layout is    */
/* fixed: the JIT's linear-memory register file (Ballistic, §10)       */
/* already pins it.                                                    */
/* ------------------------------------------------------------------ */

typedef struct CPU_Register_File {
  uint64_t x[31];  /* X0-X30; index 31 does not exist (see get_reg note) */
  uint64_t sp;
  uint64_t pc;
  uint32_t pstate; /* NZCV */
} CPU_Register_File;

typedef enum CPU_ExitReason {
  CPU_EXIT_CYCLES_ELAPSED, /* budget consumed; thread is preempted */
  CPU_EXIT_SVC,            /* svc_handler ran; thread may now be blocked */
  CPU_EXIT_HALT,           /* guest executed a halting condition */
  CPU_EXIT_BREAKPOINT,
  CPU_EXIT_FAULT,          /* MMU fault or undefined instruction */
} CPU_ExitReason;

/* ------------------------------------------------------------------ */
/* Callbacks set by the emulator core, invoked by the backend.         */
/* ------------------------------------------------------------------ */

typedef void (*CPU_SVC_Handler)(CPU_State *state, uint32_t swi, void *userdata);
typedef void (*CPU_Undefined_Handler)(CPU_State *state, uint32_t instruction, void *userdata);
typedef void (*CPU_Breakpoint_Handler)(CPU_State *state, uint64_t address, void *userdata);

/* ------------------------------------------------------------------ */
/* Backend interface vtable.                                           */
/* ------------------------------------------------------------------ */

struct CPU_Backend
{
  /* Lifecycle. One CPU_State per guest thread; vmm is shared. */
  CPU_State *(*create)(VMM_Context *vmm, void *userdata);
  void (*destroy)(CPU_State *state);

  /* Execution. Runs the thread whose register file is `state` for at most
   * `cycle_budget` cycles. Must be able to stop at translation-block
   * boundaries when the budget expires. Never runs to completion. */
  CPU_ExitReason (*run)(CPU_State *state, uint64_t cycle_budget);
  CPU_ExitReason (*step)(CPU_State *state);

  /* Exit details, valid after run()/step() returns. */
  uint64_t (*get_fault_address)(CPU_State *state);   /* CPU_EXIT_FAULT */
  uint64_t (*get_cycles_consumed)(CPU_State *state); /* for virtual time */

  /* General purpose registers X0-X30 (index 0-30).
   * Index 31 is INVALID at this interface. ARM64 encoding field 31 means
   * XZR or SP depending on instruction context; that disambiguation is
   * internal to each backend's decode. SP has dedicated accessors below.
   * Backends must debug-assert index <= 30. */
  uint64_t (*get_reg)(CPU_State *state, uint8_t reg_index);
  void (*set_reg)(CPU_State *state, uint8_t reg_index, uint64_t value);

  uint64_t (*get_pc)(CPU_State *state);
  void (*set_pc)(CPU_State *state, uint64_t value);
  uint64_t (*get_sp)(CPU_State *state);
  void (*set_sp)(CPU_State *state, uint64_t value);
  uint32_t (*get_pstate)(CPU_State *state); /* NZCV */
  void (*set_pstate)(CPU_State *state, uint32_t value);

  /* Fast path for HLE. Every SVC reads/writes 6-10 registers; paying an
   * indirect call per register on the hottest HLE path is waste. Backends
   * embed CPU_Register_File as their architectural state and return a
   * pointer that is stable for the state's lifetime. HLE reads regs->x[8]
   * directly. The accessors above remain for the debugger and tests. */
  CPU_Register_File *(*get_register_file)(CPU_State *state);

  uint64_t (*get_sys_reg)(CPU_State *state, uint32_t encoded_reg);
  void (*set_sys_reg)(CPU_State *state, uint32_t encoded_reg, uint64_t value);

  /* Code cache management. Called by the SMC path (§5) and module loaders.
   * Shared across all CPU_States of the same process. */
  void (*invalidate_cache)(CPU_State *state, uint64_t guest_va, uint64_t size_bytes);
  void (*clear_cache)(CPU_State *state);

  void (*set_svc_handler)(CPU_State *state, CPU_SVC_Handler handler);
  void (*set_undefined_handler)(CPU_State *state, CPU_Undefined_Handler handler);
  void (*set_breakpoint_handler)(CPU_State *state, CPU_Breakpoint_Handler handler);

  const char *name; /* "noop", "interpreter", "ballistic" */
  const char *version;
  bool supports_jit;
};

/* ------------------------------------------------------------------ */
/* Register index reference.                                           */
/* ------------------------------------------------------------------ */

/* X0-X7: args/returns   X29: FP   X30: LR
 * (X8 is NOT the syscall number on Horizon - the SVC immediate is. §12.) */
#define CPU_REG_X0 0u
#define CPU_REG_X29 29u
#define CPU_REG_X30 30u
/* There is no CPU_REG_XZR / CPU_REG_SP constant at this interface.
 * SP: get_sp/set_sp. XZR: not architectural state; nothing to access. */

/* ------------------------------------------------------------------ */
/* Backend registry.                                                   */
/* ------------------------------------------------------------------ */

extern const CPU_Backend CPU_BACKEND_NOOP;
extern const CPU_Backend CPU_BACKEND_INTERPRETER; /* Phase 2 */
extern const CPU_Backend CPU_BACKEND_BALLISTIC;   /* only if -DCPU_BACKEND=ballistic */

/* Returns the backend selected at configure time (see CPU_BACKEND CMake option). */
const CPU_Backend *cpu_get_active_backend(void);

#endif /* SWITCH_CPU_CPU_H */
