/**
 * GPU rendering worker (docs/DESIGN.md section 16). Phase 0 scope: receive
 * the transferred OffscreenCanvas plus the shared memory + layout handshake,
 * acquire a WebGPU adapter/device, and paint a single solid-colour clear so
 * "workers start, layout handshake completed" can be observed by eye.
 * Maxwell command buffer parsing, the GPU command ring drain, and shader
 * translation arrive in Phase 3+ (§13).
 */

import type { MemoryLayout } from "@bindings/layout";
import type { GPUToMainMessage, MainToGPUMessage } from "@bindings/protocol";

const self: DedicatedWorkerGlobalScope =
  globalThis as unknown as DedicatedWorkerGlobalScope;

function log(level: "debug" | "info" | "warn" | "error", message: string): void {
  const msg: GPUToMainMessage = { type: "log", level, message };
  self.postMessage(msg);
}

async function init(canvas: OffscreenCanvas, memory: WebAssembly.Memory, layout: MemoryLayout): Promise<void> {
  log("debug", `layout handshake: framebufferSlotBase=0x${layout.framebufferSlotBase.toString(16)}`);
  void memory; // the framebuffer-slot present path (§6) lands with Phase 3

  if (!("gpu" in navigator)) {
    const err: GPUToMainMessage = { type: "error", message: "WebGPU unavailable inside worker" };
    self.postMessage(err);
    return;
  }

  const gpu = (navigator as Navigator & { gpu: GPU }).gpu;
  const adapter = await gpu.requestAdapter();
  if (!adapter) {
    const err: GPUToMainMessage = { type: "error", message: "requestAdapter() returned null" };
    self.postMessage(err);
    return;
  }

  const device = await adapter.requestDevice();
  const context = canvas.getContext("webgpu") as GPUCanvasContext | null;
  if (!context) {
    const err: GPUToMainMessage = { type: "error", message: "getContext('webgpu') returned null" };
    self.postMessage(err);
    return;
  }

  const format = gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: "opaque" });

  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [{
      view:       context.getCurrentTexture().createView(),
      clearValue: { r: 0.04, g: 0.04, b: 0.06, a: 1 },
      loadOp:     "clear",
      storeOp:    "store",
    }],
  });
  pass.end();
  device.queue.submit([encoder.finish()]);

  log("info", `WebGPU ready, format=${format}`);
  const ready: GPUToMainMessage = {
    type: "ready",
    adapterName: ((adapter as unknown as { info?: { vendor?: string } }).info?.vendor) ?? null,
  };
  self.postMessage(ready);
}

self.addEventListener("message", (event: MessageEvent<MainToGPUMessage>) => {
  if (event.data.type === "init") {
    init(event.data.canvas, event.data.memory, event.data.layout).catch((e: unknown) => {
      const err: GPUToMainMessage = {
        type: "error",
        message: `init threw: ${e instanceof Error ? e.message : String(e)}`,
      };
      self.postMessage(err);
    });
  }
});
