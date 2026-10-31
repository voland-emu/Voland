/**
 * Main-thread input writer (§18): polls the Gamepad API and canvas
 * pointer events each rAF and publishes the result into the input
 * region through the seqlock protocol (bindings/input.ts). This is the
 * one writer the region's contract names (§6: "Input region | Main
 * thread | CPU Worker | seqlock"); hid: (Phase 4) is the only reader,
 * which is why the matching C-side reader (core/common/input_region.c)
 * ships with its own native tests instead of a browser-side consumer.
 *
 * WebHID Joy-Con/Pro Controller support (§18's "progressive enhancement,
 * Chrome-only" tier, including IMU motion) is NOT implemented here: raw
 * HID report parsing is substantial, separately-scoped work. Every slot
 * reports motionSample=0 ("no sensor") until that lands - a true
 * statement for Gamepad-API-sourced controllers, which the Gamepad API
 * never exposes motion data for in the first place.
 */

/* Relative, not the "@bindings/*" alias: this file is imported directly
 * by tests/unit/input-writer.test.ts under plain Node
 * (--experimental-strip-types only erases types, it does not resolve
 * bundler path aliases - vite.config.ts's "@bindings" mapping only
 * applies inside the Vite build). Every other value-importing module
 * here uses relative paths for the same reason; main.ts/cpu.worker.ts
 * get away with the alias only because nothing yet imports them from a
 * unit test. */
import {
  INPUT_REGION_MAX_CONTROLLERS,
  InputFlags,
  NEUTRAL_CONTROLLER_STATE,
  writeControllerSlot,
  writeTouchBlock,
  type ControllerState,
  type TouchPoint,
} from "../bindings/input.ts";
import type { MemoryLayout } from "../bindings/layout.ts";

/** Switch A/B and X/Y are swapped relative to the Standard Gamepad (Xbox)
 * layout (§18). buttonMap[i] names which Standard Gamepad button index
 * feeds Switch button bit i. Exported for tests/unit/input-writer.test.ts,
 * which checks every entry against the Standard Gamepad spec's physical
 * button positions one at a time - an off-by-one here is exactly the kind
 * of mistake that is easy to make and easy to miss by eye. */
export const STANDARD_BUTTON_MAP: readonly number[] = [
  1, 0,            // A, B
  3, 2,            // X, Y
  4, 5, 6, 7,      // L, R, ZL, ZR
  8, 9, 10, 11,    // Minus, Plus, LStick, RStick
  12, 13, 14, 15,  // D-pad
  16,              // Home
];

const AXIS_COUNT = 8;
const MAX_TOUCH_POINTS = 10;
/* §18: touch points are reported in 1280x720 screen space, independent
 * of the canvas's actual CSS or backing-store size. */
const TOUCH_SCREEN_WIDTH = 1280;
const TOUCH_SCREEN_HEIGHT = 720;

/** Exported for tests - the Gamepad API's axes are -1..1 floats, the wire
 * format is i16, and both ends of that range (and values outside it, which
 * real hardware does report briefly) need to land somewhere defined. */
export function axisToI16(value: number): number {
  return Math.round(Math.max(-1, Math.min(1, value)) * 32767);
}

/** Exported for tests - takes a plain object shaped like the subset of
 * Gamepad this reads (buttons[].pressed, axes[], connected), not a real
 * browser Gamepad, so a test can construct one without a DOM. */
export function readControllerState(pad: Gamepad): ControllerState {
  let buttons = 0;
  for (let i = 0; i < STANDARD_BUTTON_MAP.length; i++) {
    if (pad.buttons[STANDARD_BUTTON_MAP[i]]?.pressed) buttons |= 1 << i;
  }

  const axes = new Array<number>(AXIS_COUNT).fill(0);
  for (let i = 0; i < Math.min(AXIS_COUNT, pad.axes.length); i++) {
    axes[i] = axisToI16(pad.axes[i]);
  }

  return {
    buttons,
    axes,
    flags: InputFlags.Connected,
    motionSample: 0, // no WebHID IMU source yet - see header comment
    accel: [0, 0, 0],
    gyro: [0, 0, 0],
  };
}

function pollGamepads(memory: WebAssembly.Memory, inputRegionBase: number): void {
  const pads = navigator.getGamepads();
  for (let slot = 0; slot < INPUT_REGION_MAX_CONTROLLERS; slot++) {
    const pad = pads[slot];
    const state = pad?.connected ? readControllerState(pad) : NEUTRAL_CONTROLLER_STATE;
    writeControllerSlot(memory, inputRegionBase, slot, state);
  }
}

/** Tracks active touches by pointerId (the same identity canvas pointer
 * events already assign, reused directly as the wire format's finger id)
 * and converts client coordinates to the §18 1280x720 screen space.
 * Exported for tests - `canvas` only needs getBoundingClientRect and
 * addEventListener, so a test can pass a plain fake rather than a real
 * HTMLCanvasElement (no jsdom dependency, per CLAUDE.md's "no third-party
 * dependencies not listed in DESIGN.md"). */
export function createTouchTracker(canvas: HTMLCanvasElement): () => readonly TouchPoint[] {
  const active = new Map<number, TouchPoint>();

  function toScreenSpace(event: PointerEvent): { x: number; y: number } {
    const rect = canvas.getBoundingClientRect();
    return {
      x: ((event.clientX - rect.left) / rect.width) * TOUCH_SCREEN_WIDTH,
      y: ((event.clientY - rect.top) / rect.height) * TOUCH_SCREEN_HEIGHT,
    };
  }

  function onDownOrMove(event: PointerEvent): void {
    if (event.pointerType !== "touch") return;
    if (!active.has(event.pointerId) && active.size >= MAX_TOUCH_POINTS) return;
    const { x, y } = toScreenSpace(event);
    active.set(event.pointerId, { fingerId: event.pointerId, x, y });
  }
  function onUp(event: PointerEvent): void {
    active.delete(event.pointerId);
  }

  canvas.addEventListener("pointerdown", onDownOrMove);
  canvas.addEventListener("pointermove", onDownOrMove);
  canvas.addEventListener("pointerup", onUp);
  canvas.addEventListener("pointercancel", onUp);

  return () => Array.from(active.values());
}

/**
 * Starts the main-thread input writer: each rAF, publishes gamepad and
 * touch state into the input region (§18). Returns a stop function
 * (cancels the rAF loop); nothing calls it yet, since the writer runs
 * for the page's lifetime, but leaving the loop unstoppable by
 * construction would make it untestable in isolation too.
 */
export function startInputWriter(memory: WebAssembly.Memory, layout: MemoryLayout): () => void {
  const canvas = document.getElementById("game") as HTMLCanvasElement | null;
  const readTouchPoints = canvas ? createTouchTracker(canvas) : (): readonly TouchPoint[] => [];

  let rafHandle = 0;
  let stopped = false;

  function tick(): void {
    if (stopped) return;
    pollGamepads(memory, layout.inputRegionBase);
    writeTouchBlock(memory, layout.inputRegionBase, readTouchPoints());
    rafHandle = requestAnimationFrame(tick);
  }
  rafHandle = requestAnimationFrame(tick);

  return () => {
    stopped = true;
    cancelAnimationFrame(rafHandle);
  };
}
