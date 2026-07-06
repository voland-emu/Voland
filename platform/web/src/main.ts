/**
 * Phase 0 boot sequence (docs/DESIGN.md section 16). The page loads as a
 * black screen with a boot log on the right; once the CPU and GPU workers
 * each report "ready" we dynamically import the Solid shell and fade the
 * boot overlay out. Keeping the Solid runtime out of the boot-critical
 * path means the black screen appears immediately and the UI chunk only
 * loads once the core has actually come up.
 *
 * There is no Audio Worker (§14) - the audio ring is drained directly by
 * an AudioWorkletProcessor once DSP HLE exists (Phase 4). Phase 0 only
 * needs the CPU and GPU workers to validate the boot path end to end.
 */

import type { CPUToMainMessage, GPUToMainMessage, MainToCPUMessage, MainToGPUMessage } from "@bindings/protocol";
import type { MemoryLayout } from "@bindings/layout";
import { detectCapabilities, type PlatformCapabilities } from "./capabilities";
import { publishBootMilestone } from "./e2e-hooks";
import { appendLogLine, setStatus } from "./log";

/**
 * Total shared linear memory: guest RAM + every layout.h region +
 * Emscripten's own data/stack/heap (~5.25GiB). Fixed at boot -
 * `initial === maximum`, growth disabled, so views never detach (§4).
 * This MUST equal CMakeLists.txt's INITIAL_MEMORY/MAXIMUM_MEMORY in bytes
 * (5_637_144_576) or the core module fails to instantiate.
 */
const WASM_PAGE_BYTES = 65_536n;
const TOTAL_MEMORY_BYTES = 5_637_144_576n; // ~5.25GiB
const TOTAL_MEMORY_PAGES = TOTAL_MEMORY_BYTES / WASM_PAGE_BYTES; // 86016n

const WORKER_READY_TIMEOUT_MS = 10_000;

function fatal(reason: string): void {
  setStatus(`fatal: ${reason}`);
  appendLogLine("error", reason);
}

function reportCapabilities(caps: PlatformCapabilities): void {
  const lines: string[] = [];
  (Object.keys(caps) as Array<keyof PlatformCapabilities>).forEach(key => {
    lines.push(`${key}=${caps[key] ? "yes" : "no"}`);
  });
  appendLogLine("debug", `capabilities: ${lines.join(", ")}`);
}

/** Guarded one-time reload so a fresh visit under SW-injected COOP/COEP
 * headers (rather than real server headers) gets a second pass once the
 * Service Worker actually controls the page (§16). */
async function reloadOnceForCrossOriginIsolation(): Promise<boolean> {
  if (sessionStorage.getItem("voland-coi-reload-attempted")) return false;
  sessionStorage.setItem("voland-coi-reload-attempted", "1");
  if ("serviceWorker" in navigator) {
    await navigator.serviceWorker.ready;
  }
  location.reload();
  return true;
}

/**
 * lib.dom.d.ts's `WebAssembly.MemoryDescriptor` doesn't know about the
 * memory64 shape yet: `initial`/`maximum` are BigInt page counts, not
 * Number, once `address: "i64"` is present.
 */
interface Memory64Descriptor {
  readonly initial: bigint;
  readonly maximum: bigint;
  readonly shared: true;
  readonly address: "i64";
}

function allocateSharedMemory(): WebAssembly.Memory | null {
  // Verified directly against this environment's V8 and against
  // Emscripten's own -sMEMORY64=1 glue output: both use `address: "i64"`
  // with BigInt page counts, NOT `index: "i64"` with Number page counts.
  // The latter is silently accepted as an unrecognized property and falls
  // back to an ordinary 32-bit memory, which then throws RangeError the
  // moment page counts exceed the wasm32 cap of 65536 pages (4GiB) -
  // exactly this case. (docs/DESIGN.md's own §16 example uses `index`;
  // that's stale relative to what actually shipped - do not copy it.)
  const descriptor: Memory64Descriptor = {
    initial: TOTAL_MEMORY_PAGES,
    maximum: TOTAL_MEMORY_PAGES,
    shared: true,
    address: "i64",
  };
  try {
    return new WebAssembly.Memory(descriptor as unknown as WebAssembly.MemoryDescriptor);
  } catch (e) {
    appendLogLine("error", `WebAssembly.Memory allocation failed: ${(e as Error).message}`);
    return null;
  }
}

function attachWorkerLogRelay<TMessage extends { type: string }>(
  workerName: string,
  worker: Worker,
  readyHandler: (msg: TMessage) => void,
  failHandler: (reason: string) => void,
): void {
  worker.addEventListener("message", (event: MessageEvent<TMessage>) => {
    const msg = event.data;
    if (msg.type === "log") {
      const logMsg = msg as TMessage & { level: "debug" | "info" | "warn" | "error"; message: string };
      appendLogLine(logMsg.level, `${workerName}: ${logMsg.message}`);
      return;
    }
    if (msg.type === "error") {
      const errMsg = msg as TMessage & { message: string };
      appendLogLine("error", `${workerName} error: ${errMsg.message}`);
      failHandler(errMsg.message);
      return;
    }
    readyHandler(msg);
  });
  worker.addEventListener("error", (event: ErrorEvent) => {
    appendLogLine("error", `${workerName} uncaught: ${event.message}`);
    failHandler(event.message || "uncaught worker error");
  });
}

interface BootResult {
  readonly adapterLabel: string;
  readonly cpuBackend:   string;
  readonly guestRamMiB:  number;
}

async function boot(): Promise<BootResult | null> {
  appendLogLine("info", "Voland web - Phase 0 skeleton booting");

  if ("serviceWorker" in navigator) {
    // Vite serves sw.ts directly (on-the-fly transform) in dev; the
    // production build emits it as a stable, unhashed sw.js (vite.config.ts).
    const swUrl = import.meta.env.DEV ? "/sw.ts" : "/sw.js";
    try {
      await navigator.serviceWorker.register(swUrl, { type: "module" });
    } catch (e) {
      appendLogLine("warn", `Service Worker registration failed: ${(e as Error).message}`);
    }
  }

  if (!crossOriginIsolated) {
    const reloaded = await reloadOnceForCrossOriginIsolation();
    if (reloaded) return null; // navigation is in flight
    fatal("Cross-origin isolation is not active. Ensure the server sets " +
          "COOP: same-origin and COEP: require-corp (§16).");
    return null;
  }

  const caps = detectCapabilities();
  reportCapabilities(caps);

  if (!caps.webGPU) {
    fatal("WebGPU is not available. Update your browser (WebGPU is Baseline " +
          "since Jan 2026: Chrome/Edge 113+, Safari 26+, Firefox 141+).");
    return null;
  }
  if (!caps.opfs) {
    fatal("Origin Private Filesystem (OPFS) is not available.");
    return null;
  }

  /* ONE memory. Guest RAM and every other shared region live inside it
   * (§4). No separate SharedArrayBuffers. */
  const memory = allocateSharedMemory();
  if (!memory) {
    fatal("Could not allocate emulator memory (~5.25GiB). Your browser or " +
          "device limits shared memory; use a native build.");
    return null;
  }
  appendLogLine("info", `allocated shared memory: ${TOTAL_MEMORY_PAGES} pages (~5.25GiB)`);
  publishBootMilestone(memory);

  const canvas = document.getElementById("game") as HTMLCanvasElement | null;
  if (!canvas) {
    fatal("boot: #game canvas missing from index.html");
    return null;
  }
  const offscreen = canvas.transferControlToOffscreen();

  const cpuWorker = new Worker(new URL("../workers/cpu.worker.ts", import.meta.url), { type: "module" });
  const gpuWorker = new Worker(new URL("../workers/gpu.worker.ts", import.meta.url), { type: "module" });

  type Slot = "pending" | "ready" | "failed";
  let cpuSlot: Slot = "pending";
  let gpuSlot: Slot = "pending";

  let cpuBackend:   string | null = null;
  let adapterLabel: string | null = null;
  let layout:       MemoryLayout | null = null;

  function slotGlyph(slot: Slot): string {
    return slot === "ready" ? "ok" : slot === "failed" ? "fail" : "…";
  }
  function updateStatus(): void {
    setStatus([`cpu=${slotGlyph(cpuSlot)}`, `gpu=${slotGlyph(gpuSlot)}`].join(" · "));
  }
  updateStatus();

  const cpuReady = new Promise<void>((resolve) => {
    attachWorkerLogRelay<CPUToMainMessage>("cpu", cpuWorker, msg => {
      if (msg.type === "layout") {
        layout = msg.layout;
        appendLogLine("info", `layout handshake: guestRamBase=0x${layout.guestRamBase.toString(16)}`);
        gpuWorker.postMessage(
          { type: "init", canvas: offscreen, memory, layout } satisfies MainToGPUMessage,
          [offscreen],
        );
        return;
      }
      if (msg.type === "ready") {
        cpuBackend = `${msg.backendName} v${msg.backendVersion}`;
        appendLogLine("info", `cpu backend: ${cpuBackend}`);
        cpuSlot = "ready";
        updateStatus();
        resolve();
      } else if (msg.type === "halted") {
        appendLogLine("warn", "cpu halted");
      }
    }, () => { cpuSlot = "failed"; updateStatus(); resolve(); });
  });

  const gpuReady = new Promise<void>((resolve) => {
    attachWorkerLogRelay<GPUToMainMessage>("gpu", gpuWorker, msg => {
      if (msg.type === "ready") {
        adapterLabel = msg.adapterName ?? "Software/Unknown";
        appendLogLine("info", `gpu adapter: ${adapterLabel}`);
        gpuSlot = "ready";
        updateStatus();
        resolve();
      }
    }, () => { gpuSlot = "failed"; updateStatus(); resolve(); });
  });

  const timeout = new Promise<void>((resolve) => {
    window.setTimeout(() => {
      if (cpuSlot === "pending") { cpuSlot = "failed"; appendLogLine("error", `cpu worker did not report within ${WORKER_READY_TIMEOUT_MS}ms`); }
      if (gpuSlot === "pending") { gpuSlot = "failed"; appendLogLine("error", `gpu worker did not report within ${WORKER_READY_TIMEOUT_MS}ms`); }
      updateStatus();
      resolve();
    }, WORKER_READY_TIMEOUT_MS);
  });

  cpuWorker.postMessage({ type: "init", memory } satisfies MainToCPUMessage);
  appendLogLine("info", "cpu worker posted init message; awaiting layout handshake");

  await Promise.race([Promise.all([cpuReady, gpuReady]), timeout]);

  /* rAF stops in hidden tabs, freezing input writes (§18); auto-pause
   * keeps the policy explicit instead of leaving it as a silent symptom. */
  document.addEventListener("visibilitychange", () => {
    const msg: MainToCPUMessage = { type: document.hidden ? "pause" : "resume" };
    cpuWorker.postMessage(msg);
  });

  // TypeScript's control flow analysis doesn't see assignments made inside
  // closures (the "layout" case above), so it treats `layout` as still
  // `null` here; the cast reasserts the declared type.
  const finalLayout = layout as MemoryLayout | null;
  return {
    adapterLabel: adapterLabel ?? "unavailable",
    cpuBackend:   cpuBackend   ?? "unavailable",
    guestRamMiB:  finalLayout ? Number(finalLayout.guestRamSize / (1024n * 1024n)) : 0,
  };
}

boot()
  .then(async result => {
    if (!result) return;
    appendLogLine("info", "core online - mounting Solid shell");
    const { mountShell } = await import("./ui/mount");
    mountShell(result);
  })
  .catch(e => fatal(`unhandled boot error: ${(e as Error).message}`));
