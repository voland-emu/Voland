/**
 * Test-only hook for the Playwright boot e2e (e2e/boot.spec.ts, CLAUDE.md's
 * `npm run e2e`). Exposes the boot milestones the e2e spec asserts:
 * crossOriginIsolated and the full shared-memory allocation succeeding
 * (pre-wasm, so they hold without the compiled core), then the guest RAM
 * size the core reported in the layout handshake (needs the core).
 * Not a general debugging API: see docs/DESIGN.md §23 for that (Phase 6+,
 * window.__VOLAND_MEMORY__ / __VOLAND_TRACE_METADATA__).
 */

export interface VolandE2EBootMilestone {
  readonly crossOriginIsolated: boolean;
  readonly allocatedMemoryBytes: number;
  /** Set once the CPU worker's layout handshake arrives (§16). */
  readonly guestRamBytes?: number;
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
