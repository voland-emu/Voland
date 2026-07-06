/**
 * Top-level emulator wiring. The core entry point for every platform.
 * Phase 0 scope: create/destroy, wire the no-op CPU backend into the stub
 * HLE dispatcher, expose the bounded run/step contract.
 */
#ifndef SWITCH_EMULATOR_H
#define SWITCH_EMULATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"
#include "common/result.h"
#include "cpu/cpu.h"
#include "hle/hle.h"

typedef struct Emulator
{
  const CPU_Backend *cpu_backend;
  /* One CPU_State stands in for "the" guest thread until the Phase 2
   * scheduler (§7) exists to multiplex real ones. */
  CPU_State *cpu_state;
  HLE_Context hle;
} Emulator;

/* Reserves the linear memory layout (§4) and wires the active CPU backend
 * (§8) to the stub HLE dispatcher. There is no Emulator_Config: guest RAM
 * size and every other region size are fixed by common/layout.h, not
 * caller-configurable - on web the single WebAssembly.Memory is created by
 * the boot sequence (§16) before the core module is even instantiated. */
Error emulator_create(Emulator *out);
void emulator_destroy(Emulator *emulator);

/* Runs the single CPU_State for at most `cycle_budget` cycles and reports
 * why it stopped. This is a direct pass-through to the active backend;
 * `run(entry_point)`-until-done does not exist in the interface (§7/§8). */
CPU_ExitReason emulator_run(Emulator *emulator, uint64_t cycle_budget);

/* Single-step. */
CPU_ExitReason emulator_step(Emulator *emulator);

/* Diagnostics. */
const char *emulator_backend_name(const Emulator *emulator);

#endif /* SWITCH_EMULATOR_H */
