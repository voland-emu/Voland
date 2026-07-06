/**
 * Typed surface of the WASM core's exported functions. The actual module
 * is produced by `emcmake cmake ... && cmake --build build`, which emits
 * `switch_core.js` + `switch_core.wasm` (see CMakeLists.txt EXPORTED_
 * FUNCTIONS, mirrored 1:1 below). Every export is `_ffi` suffixed because
 * the real core functions take an `Emulator*` the JS side has no way to
 * name; each wrapper operates on the module-global instance instead.
 *
 * MEMORY64 + WASM_BIGINT means every pointer-or-uint64-typed export
 * exchanges `bigint`, never `number`, at the WASM boundary.
 */

export interface SwitchCoreExports {
  readonly _emulator_create_ffi:  () => number;
  readonly _emulator_destroy_ffi: () => void;
  readonly _emulator_run_ffi:     (cycleBudget: bigint) => number; /* CPU_ExitReason */
  readonly _emulator_step_ffi:    () => number;                    /* CPU_ExitReason */

  readonly _layout_get_ffi:      () => bigint; /* Memory_Layout* in linear memory */
  readonly _cpu_backend_id_ffi:  () => number; /* CpuBackendId */

  readonly _cpu_get_reg_ffi: (index: number) => bigint;
  readonly _cpu_set_reg_ffi: (index: number, value: bigint) => void;
  readonly _cpu_get_pc_ffi:  () => bigint;
}

/* Mirrors the #if ladder in core/stubs/wasm_entry.c's cpu_backend_id_ffi -
 * CPU_BACKEND is a CMake configure-time choice, never a runtime one, so
 * this sidesteps marshalling the backend's `name`/`version` C strings
 * across the MEMORY64 FFI boundary for a value fixed at build time. */
export const enum CpuBackendId {
  Noop = 0,
  Interpreter = 1,
  Ballistic = 2,
}

export const CPU_BACKEND_DISPLAY_NAMES: Readonly<Record<number, string>> = {
  [CpuBackendId.Noop]:        "noop",
  [CpuBackendId.Interpreter]: "interpreter",
  [CpuBackendId.Ballistic]:   "ballistic",
};

/* CPU_ExitReason, mirrors core/cpu/cpu.h. */
export const enum CpuExitReason {
  CyclesElapsed = 0,
  Svc           = 1,
  Halt          = 2,
  Breakpoint    = 3,
  Fault         = 4,
}

/* ARM64 general-purpose register indices. Mirrors core/cpu/cpu.h.
 * Index 31 does not exist at this interface - there is no XZR/SP constant
 * here; SP has its own accessor and XZR is not architectural state. */
export const CPU_REG_X0  = 0;
export const CPU_REG_X29 = 29;
export const CPU_REG_X30 = 30;
