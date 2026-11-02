/**
 * Thin Emscripten entry point. Exposes the emulator public API as exported
 * C functions so the CPU worker can call into the core over ccall/cwrap.
 * See docs/DESIGN.md section 24 (WASM build flags, EXPORTED_FUNCTIONS).
 *
 * Wrappers are `_ffi` suffixed rather than the bare core function names:
 * `emulator_create`/`emulator_run`/etc. already exist as real functions
 * taking an `Emulator*` the JS side has no way to name, so each wrapper
 * here operates on the single module-global instance instead. `layout_get`
 * needs no wrapper since it already takes no arguments.
 */
#include "common/input_region.h"
#include "common/layout.h"
#include "common/log.h"
#include "emulator.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* Prototypes for -Wmissing-prototypes: these are called from JS, never
 * from another C translation unit, so there is nowhere sensible to put a
 * shared header - but the project's own strict-warnings policy (§3) still
 * requires one. */
EXPORT int emulator_create_ffi(void);
EXPORT void emulator_destroy_ffi(void);
EXPORT int emulator_run_ffi(uint64_t cycle_budget);
EXPORT int emulator_step_ffi(void);
EXPORT uint64_t cpu_get_reg_ffi(uint32_t index);
EXPORT void cpu_set_reg_ffi(uint32_t index, uint64_t value);
EXPORT uint64_t cpu_get_pc_ffi(void);
EXPORT int cpu_backend_id_ffi(void);
EXPORT uintptr_t layout_get_ffi(void);
EXPORT uintptr_t input_region_read_controller_ffi(uint32_t slot);

static Emulator g_emulator;
static int g_initialised = 0;

EXPORT int emulator_create_ffi(void)
{
  if (g_initialised)
    return 1;
  const Error err = emulator_create(&g_emulator);
  if (err.code != RESULT_OK)
  {
    log_error("[wasm_entry] emulator_create failed: %s", err.message);
    return 0;
  }
  g_initialised = 1;
  return 1;
}

EXPORT void emulator_destroy_ffi(void)
{
  if (!g_initialised)
    return;
  emulator_destroy(&g_emulator);
  g_initialised = 0;
}

/* Returns CPU_ExitReason as an int; scheduler_tick (§7) replaces this
 * caller once Phase 2 lands. */
EXPORT int emulator_run_ffi(uint64_t cycle_budget)
{
  if (!g_initialised)
    return -1;
  return (int)emulator_run(&g_emulator, cycle_budget);
}

EXPORT int emulator_step_ffi(void)
{
  if (!g_initialised)
    return -1;
  return (int)emulator_step(&g_emulator);
}

EXPORT uint64_t cpu_get_reg_ffi(uint32_t index)
{
  if (!g_initialised)
    return 0;
  return g_emulator.cpu_backend->get_reg(g_emulator.cpu_state, (uint8_t)index);
}

EXPORT void cpu_set_reg_ffi(uint32_t index, uint64_t value)
{
  if (!g_initialised)
    return;
  g_emulator.cpu_backend->set_reg(g_emulator.cpu_state, (uint8_t)index, value);
}

EXPORT uint64_t cpu_get_pc_ffi(void)
{
  if (!g_initialised)
    return 0;
  return g_emulator.cpu_backend->get_pc(g_emulator.cpu_state);
}

/* Numeric backend id rather than marshalling `name`/`version` C strings
 * across the FFI boundary - platform/web/bindings/core.ts keeps the
 * matching display-name table. Sidesteps string-pointer marshalling
 * entirely for a value that is fixed at compile time anyway
 * (CPU_BACKEND is a CMake configure-time choice, never a runtime one). */
EXPORT int cpu_backend_id_ffi(void)
{
#if defined(SWITCH_CPU_BACKEND_NOOP)
  return 0; /* CpuBackendId.Noop, bindings/core.ts */
#else
  return -1;
#endif
}

/* Returns a linear-memory pointer to the live Memory_Layout struct (§4) as
 * a plain integer, cast from the module's own address space: uintptr_t,
 * so under wasm32 it is an i32 and reaches JS as a Number (§24).
 * platform/web bindings/layout.ts mirrors the C struct field-for-field
 * and reads it back out of the shared WebAssembly.Memory at this offset. */
EXPORT uintptr_t layout_get_ffi(void)
{
  const Memory_Layout *layout = layout_get();
  return (uintptr_t)layout;
}

/* Returns a linear-memory pointer to a static Input_Controller_State,
 * freshly read through the seqlock (common/input_region.h) for `slot`.
 * Exists only for the e2e cross-language round-trip test
 * (platform/web/e2e): production reading of the input region happens in
 * the CPU worker's native code (hid:, Phase 4), never through this FFI
 * hop. Byte layout - no compiler padding, every field is already
 * naturally aligned at its offset, the §23 Trace_Event precedent -
 * buttons u32 @0, axes i16[8] @4, flags u32 @20, motion_sample u32 @24,
 * accel f32[3] @28, gyro f32[3] @40 (52 bytes). platform/web's e2e spec
 * reads these offsets directly out of linear memory, the same way
 * bindings/layout.ts reads Memory_Layout. */
EXPORT uintptr_t input_region_read_controller_ffi(uint32_t slot)
{
  static Input_Controller_State g_controller_state;
  input_region_read_controller(slot, &g_controller_state);
  return (uintptr_t)&g_controller_state;
}

#ifndef __EMSCRIPTEN__
int main(void)
{
  log_info("[wasm_entry] native build, running smoke test");
  Emulator emu;
  const Error err = emulator_create(&emu);
  if (err.code != RESULT_OK)
  {
    log_error("emulator_create failed: %s", err.message);
    return 1;
  }
  const CPU_ExitReason reason = emulator_run(&emu, 1000);
  log_info("[wasm_entry] emulator_run exit reason=%d", (int)reason);
  emulator_destroy(&emu);
  return 0;
}
#endif
