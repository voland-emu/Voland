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
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "hle/kernel/page_allocator.h"
#include "hle/kernel/process.h"
#include "hle/loader/byte_source.h"

/* Scratch for one bootstrap: the ExeFS directory plus the largest
 * compressed NSO segment staged for LZ4. A shipping title's biggest
 * `main` .text compresses to ~50-60MB; created for the duration of
 * emulator_load_program() only, then freed. */
#define EMULATOR_LOADER_ARENA_BYTES ((size_t)96 * 1024 * 1024)

typedef struct Emulator
{
  /* Softmmu (§5). Created after the layout and before the CPU backend,
   * which receives it at CPU_State creation; shared by every CPU_State
   * and by HLE. */
  VMM_Context *vmm;
  const CPU_Backend *cpu_backend;
  /* One CPU_State stands in for "the" guest thread until the Phase 2
   * scheduler (§7) exists to multiplex real ones. */
  CPU_State *cpu_state;
  HLE_Context hle;

  /* Guest physical pages (§4) and the loaded process (§12). `process`
   * is valid only while `program_loaded` is true. */
  Page_Allocator pages;
  Process process;
  bool program_loaded;
} Emulator;

/* Reserves the linear memory layout (§4), creates the softmmu (§5), and
 * wires the active CPU backend (§8) to the stub HLE dispatcher. There is
 * no Emulator_Config: guest RAM size and every other region size are
 * fixed by common/layout.h, not caller-configurable - on web the single
 * WebAssembly.Memory is created by the boot sequence (§16) before the
 * core module is even instantiated. */
Error emulator_create(Emulator *out);
void emulator_destroy(Emulator *emulator);

/* "Load a game" (§12): parses the decrypted PROGRAM NCA in `nca` (§1.6:
 * pre-decrypted only; RESULT_ENCRYPTED_INPUT otherwise, message naming
 * docs/DUMP.md), bootstraps the process (process.h) and arms `cpu_state`
 * with the main thread's entry state. `aslr_seed` 0 disables ASLR. The
 * source must stay readable for the duration of the call only: every
 * byte the guest needs is in guest RAM afterwards (RomFS access is
 * fsp-srv's business, Phase 4, and re-opens the NCA). One program per
 * Emulator: a second call fails with RESULT_INVALID_ARGUMENT until
 * emulator_unload_program(). */
Error emulator_load_program(Emulator *emulator, const Byte_Source *nca, uint64_t aslr_seed);

/* Unmaps the process and resets the page allocator. No-op if nothing is
 * loaded. */
void emulator_unload_program(Emulator *emulator);

/* Runs the single CPU_State for at most `cycle_budget` cycles and reports
 * why it stopped. This is a direct pass-through to the active backend;
 * `run(entry_point)`-until-done does not exist in the interface (§7/§8). */
CPU_ExitReason emulator_run(Emulator *emulator, uint64_t cycle_budget);

/* Single-step. */
CPU_ExitReason emulator_step(Emulator *emulator);

/* Diagnostics. */
const char *emulator_backend_name(const Emulator *emulator);

#endif /* SWITCH_EMULATOR_H */
