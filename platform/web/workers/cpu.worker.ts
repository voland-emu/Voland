/**
 * CPU emulation worker (docs/DESIGN.md section 16). Owns the core WASM
 * module instance and, once the Phase 2 scheduler exists, the guest
 * thread scheduler loop (§7).
 *
 * Instantiation contract: `emcmake cmake --build` (CPU_BACKEND=noop by
 * default, §24) emits `switch_core.js` + `switch_core.wasm` as an
 * EXPORT_ES6 module. The build pipeline is expected to place (copy or
 * symlink) that output at `platform/web/public/core/switch_core.js` so
 * Vite serves it as a static asset; that copy step is CI plumbing, not
 * part of this design and not wired up in this repository yet. Until it
 * is, the dynamic import below fails and this worker reports a clear
 * "error" message instead of a fake "ready" - Phase 0's boot path is
 * meant to be exercised honestly, not stubbed into looking alive.
 */

import type { CpuBackendId, SwitchCoreExports } from "@bindings/core";
import { CPU_BACKEND_DISPLAY_NAMES } from "@bindings/core";
import { readMemoryLayout } from "@bindings/layout";
import type { CPUToMainMessage, MainToCPUMessage } from "@bindings/protocol";

const self: DedicatedWorkerGlobalScope =
  globalThis as unknown as DedicatedWorkerGlobalScope;

function log(level: "debug" | "info" | "warn" | "error", message: string): void {
  const msg: CPUToMainMessage = { type: "log", level, message };
  self.postMessage(msg);
}

/** Emscripten EXPORT_ES6+MODULARIZE factory shape: a default export that
 * takes an options object (here, the imported WebAssembly.Memory for
 * -sIMPORTED_MEMORY) and resolves to the instantiated module's exports. */
type CoreModuleFactory = (options: { wasmMemory: WebAssembly.Memory }) => Promise<SwitchCoreExports>;

const CORE_MODULE_URL = "/core/switch_core.js";

async function loadCoreModule(memory: WebAssembly.Memory): Promise<SwitchCoreExports> {
  const factory = (await import(/* @vite-ignore */ CORE_MODULE_URL)).default as CoreModuleFactory;
  return factory({ wasmMemory: memory });
}

let core: SwitchCoreExports | null = null;

async function init(memory: WebAssembly.Memory): Promise<void> {
  log("info", "cpu.worker initialising");

  try {
    core = await loadCoreModule(memory);
  } catch (e) {
    const err: CPUToMainMessage = {
      type: "error",
      message: `failed to load core module from ${CORE_MODULE_URL}: ${e instanceof Error ? e.message : String(e)}. ` +
        `Build it with emcmake cmake -B build-web && cmake --build build-web (docs/DESIGN.md §24), ` +
        `then copy build-web/core/switch_core.{js,wasm} to platform/web/public/core/.`,
    };
    self.postMessage(err);
    return;
  }

  if (core._emulator_create_ffi() === 0) {
    const err: CPUToMainMessage = { type: "error", message: "emulator_create_ffi failed" };
    self.postMessage(err);
    return;
  }

  const layoutPtr = core._layout_get_ffi();
  const layout = readMemoryLayout(memory, layoutPtr);
  const layoutMsg: CPUToMainMessage = { type: "layout", layout };
  self.postMessage(layoutMsg);

  const backendId = core._cpu_backend_id_ffi() as CpuBackendId;
  const backendName = CPU_BACKEND_DISPLAY_NAMES[backendId] ?? `unknown(${backendId})`;
  log("info", `guestRamSize=${layout.guestRamSize} bytes, backend=${backendName}`);

  const ready: CPUToMainMessage = {
    type: "ready",
    backendName,
    backendVersion: "1.0.0",
  };
  self.postMessage(ready);
}

self.addEventListener("message", (event: MessageEvent<MainToCPUMessage>) => {
  const msg = event.data;

  if (msg.type === "init") {
    init(msg.memory).catch((e: unknown) => {
      const err: CPUToMainMessage = {
        type: "error",
        message: `init threw: ${e instanceof Error ? e.message : String(e)}`,
      };
      self.postMessage(err);
    });
    return;
  }

  if (msg.type === "pause" || msg.type === "resume") {
    // No scheduler yet (§7 is Phase 2); acknowledged so main.ts's
    // visibilitychange handler has somewhere real to send these.
    log("debug", `${msg.type} (no-op until the Phase 2 scheduler exists)`);
    return;
  }

  if (msg.type === "halt") {
    const halted: CPUToMainMessage = { type: "halted" };
    self.postMessage(halted);
    return;
  }
});
