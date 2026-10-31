/**
 * Test-only hook for the Playwright boot e2e (e2e/boot.spec.ts, CLAUDE.md's
 * `npm run e2e`). Exposes the boot milestones the e2e spec asserts:
 * crossOriginIsolated and the full shared-memory allocation succeeding
 * (pre-wasm, so they hold without the compiled core), then the guest RAM
 * size the core reported in the layout handshake (needs the core).
 * Not a general debugging API: see docs/DESIGN.md §23 for that (Phase 6+,
 * window.__VOLAND_MEMORY__ / __VOLAND_TRACE_METADATA__).
 */

import { writeControllerSlot, type ControllerState } from "@bindings/input";
import type { MemoryLayout } from "@bindings/layout";

export interface VolandE2EBootMilestone {
  readonly crossOriginIsolated: boolean;
  readonly allocatedMemoryBytes: number;
  /** Set once the CPU worker's layout handshake arrives (§16). */
  readonly guestRamBytes?: number;
  /** Set alongside guestRamBytes - the e2e/input.spec.ts cross-language
   * round-trip test (§18 Phase 1) drives the real writer
   * (bindings/input.ts) against this memory, through this layout, then
   * asks this worker to read it back through the real C reader. */
  readonly inputRegionHandle?: {
    readonly cpuWorker: Worker;
    /** Writes `slot` through the real production writer
     * (bindings/input.ts's writeControllerSlot), against the real
     * shared memory and layout - the test calls this rather than
     * duplicating the byte-packing itself. */
    readonly writeController: (slot: number, state: ControllerState) => void;
  };
}

declare global {
  interface Window {
    __VOLAND_E2E__?: VolandE2EBootMilestone;
  }
}

export function publishBootMilestone(memory: WebAssembly.Memory): void {
  window.__VOLAND_E2E__ = {
    crossOriginIsolated: typeof crossOriginIsolated === "boolean" ? crossOriginIsolated : false,
    allocatedMemoryBytes: memory.buffer.byteLength,
  };
}

export function publishLayoutMilestone(guestRamBytes: number): void {
  const boot = window.__VOLAND_E2E__;
  if (!boot) return;
  window.__VOLAND_E2E__ = { ...boot, guestRamBytes };
}

export function publishInputRegionHandle(memory: WebAssembly.Memory, layout: MemoryLayout, cpuWorker: Worker): void {
  const boot = window.__VOLAND_E2E__;
  if (!boot) return;
  window.__VOLAND_E2E__ = {
    ...boot,
    inputRegionHandle: {
      cpuWorker,
      writeController: (slot, state) => writeControllerSlot(memory, layout.inputRegionBase, slot, state),
    },
  };
}
