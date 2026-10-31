/**
 * Typed message protocol between the main thread and the workers. See
 * docs/DESIGN.md section 16 ("Worker message protocol - lifecycle only").
 * Per-frame data (input, frames, audio, GPU commands) never travels over
 * postMessage - it lives in the single shared WebAssembly.Memory (§4) and
 * is addressed through MemoryLayout offsets. Phase 0 ships the minimum
 * lifecycle surface: init + log plumbing + halt. Loading games, gamepad
 * input, and save-state messages arrive in later phases.
 */
import type { ControllerState } from "./input";
import type { MemoryLayout } from "./layout";

export type LogLevel = "trace" | "debug" | "info" | "warn" | "error";

export type MainToCPUMessage =
  | { readonly type: "init"; readonly memory: WebAssembly.Memory }
  | { readonly type: "pause" }
  | { readonly type: "resume" }
  | { readonly type: "halt" }
  /* e2e-only (§18 Phase 1 cross-language round-trip, e2e/input.spec.ts):
   * one request/response, not the continuous per-frame stream the
   * lifecycle-only rule above guards against. Reads `slot` back through
   * the real C reader (input_region_read_controller_ffi) to prove the
   * seqlock writer (bindings/input.ts, running in the main thread) and
   * the seqlock reader (core/common/input_region.c, running in this
   * worker) agree on the wire format - no hid: service exists yet to
   * exercise this any other way. */
  | { readonly type: "debugReadInputController"; readonly slot: number };

export type CPUToMainMessage =
  | { readonly type: "layout"; readonly layout: MemoryLayout }
  | { readonly type: "ready"; readonly backendName: string; readonly backendVersion: string }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "halted" }
  | { readonly type: "error"; readonly message: string }
  | { readonly type: "debugInputController"; readonly slot: number; readonly state: ControllerState };

export type MainToGPUMessage =
  | {
      readonly type: "init";
      readonly canvas: OffscreenCanvas;
      readonly memory: WebAssembly.Memory;
      readonly layout: MemoryLayout;
    }
  | { readonly type: "resize"; readonly width: number; readonly height: number };

export type GPUToMainMessage =
  | { readonly type: "ready"; readonly adapterName: string | null }
  | { readonly type: "log"; readonly level: LogLevel; readonly message: string }
  | { readonly type: "error"; readonly message: string };
