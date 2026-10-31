/**
 * Mirrors core/common/input_region.h's wire format field-for-field (§18,
 * v3.29). The main thread is the one writer (startInputWriter in
 * src/input.ts, each rAF); the CPU Worker's hid: service (Phase 4) is the
 * reader. This module holds only the byte-packing - no DOM, no Gamepad
 * API - so it is unit-testable under Node the same way bindings/layout.ts
 * is (tests/unit/input.test.ts).
 *
 * Seqlock protocol (§18, common/seqlock.h): bump the sequence word odd
 * (a writer is mid-update), write the payload, bump it even again.
 * Atomics.store is required here, not a plain typed-array write - this
 * memory is a SharedArrayBuffer shared with the CPU Worker, and only
 * Atomics operations give the release ordering the protocol depends on
 * across workers.
 */

export const INPUT_REGION_MAX_CONTROLLERS = 8;
export const INPUT_REGION_SLOT_BYTES = 64;
export const INPUT_TOUCH_MAX_POINTS = 10;
export const INPUT_TOUCH_POINT_BYTES = 12;
export const INPUT_TOUCH_BLOCK_BYTES = 8 + INPUT_TOUCH_MAX_POINTS * INPUT_TOUCH_POINT_BYTES;

/* Offsets within one controller slot (§18). */
const CONTROLLER_OFF_SEQ           = 0;
const CONTROLLER_OFF_BUTTONS       = 4;
const CONTROLLER_OFF_AXES          = 8;
const CONTROLLER_OFF_FLAGS         = 24;
const CONTROLLER_OFF_MOTION_SAMPLE = 28;
const CONTROLLER_OFF_ACCEL         = 32;
const CONTROLLER_OFF_GYRO          = 44;

/* Offsets within the touch block (§18). */
const TOUCH_OFF_SEQ             = 0;
const TOUCH_OFF_POINT_COUNT     = 4;
const TOUCH_OFF_POINTS          = 8;
const TOUCH_POINT_OFF_FINGER_ID = 0;
const TOUCH_POINT_OFF_X         = 4;
const TOUCH_POINT_OFF_Y         = 8;

/* flags bitfield (§12/§18); only Connected is defined so far. A plain
 * const object, not `const enum` - Node's --experimental-strip-types
 * (platform/web's test runner) only erases types, and a `const enum`
 * needs real value semantics, not erasure. */
export const InputFlags = {
  Connected: 1 << 0,
} as const;

export interface ControllerState {
  readonly buttons: number;
  readonly axes: readonly number[]; // length 8, i16 range
  readonly flags: number;
  readonly motionSample: number;    // 0 = no motion sensor
  readonly accel: readonly number[]; // length 3, g
  readonly gyro: readonly number[];  // length 3, rad/s
}

export interface TouchPoint {
  readonly fingerId: number;
  readonly x: number; // 1280x720 screen space
  readonly y: number;
}

/** A touch frame. `points.length` must not exceed INPUT_TOUCH_MAX_POINTS. */
export type TouchState = readonly TouchPoint[];

export const NEUTRAL_CONTROLLER_STATE: ControllerState = {
  buttons: 0,
  axes: [0, 0, 0, 0, 0, 0, 0, 0],
  flags: 0,
  motionSample: 0,
  accel: [0, 0, 0],
  gyro: [0, 0, 0],
};

function controllerSlotOffset(inputRegionBase: number, slot: number): number {
  if (!Number.isInteger(slot) || slot < 0 || slot >= INPUT_REGION_MAX_CONTROLLERS) {
    throw new RangeError(`controller slot ${slot} is out of range [0, ${INPUT_REGION_MAX_CONTROLLERS})`);
  }
  return inputRegionBase + slot * INPUT_REGION_SLOT_BYTES;
}

function touchBlockOffset(inputRegionBase: number): number {
  return inputRegionBase + INPUT_REGION_MAX_CONTROLLERS * INPUT_REGION_SLOT_BYTES;
}

/** Writes controller `slot`'s state through the seqlock protocol. */
export function writeControllerSlot(
  memory: WebAssembly.Memory,
  inputRegionBase: number,
  slot: number,
  state: ControllerState,
): void {
  const offset = controllerSlotOffset(inputRegionBase, slot);
  const seq = new Int32Array(memory.buffer, offset + CONTROLLER_OFF_SEQ, 1);
  const view = new DataView(memory.buffer, offset, INPUT_REGION_SLOT_BYTES);
  const little = true;

  const odd = (Atomics.load(seq, 0) + 1) | 0;
  Atomics.store(seq, 0, odd);

  view.setUint32(CONTROLLER_OFF_BUTTONS, state.buttons, little);
  for (let i = 0; i < 8; i++) view.setInt16(CONTROLLER_OFF_AXES + i * 2, state.axes[i] ?? 0, little);
  view.setUint32(CONTROLLER_OFF_FLAGS, state.flags, little);
  view.setUint32(CONTROLLER_OFF_MOTION_SAMPLE, state.motionSample, little);
  for (let i = 0; i < 3; i++) view.setFloat32(CONTROLLER_OFF_ACCEL + i * 4, state.accel[i] ?? 0, little);
  for (let i = 0; i < 3; i++) view.setFloat32(CONTROLLER_OFF_GYRO + i * 4, state.gyro[i] ?? 0, little);

  Atomics.store(seq, 0, (odd + 1) | 0);
}

/** Writes the touch block through the seqlock protocol. */
export function writeTouchBlock(memory: WebAssembly.Memory, inputRegionBase: number, touch: TouchState): void {
  if (touch.length > INPUT_TOUCH_MAX_POINTS) {
    throw new RangeError(`touch frame has ${touch.length} points, more than ${INPUT_TOUCH_MAX_POINTS}`);
  }
  const offset = touchBlockOffset(inputRegionBase);
  const seq = new Int32Array(memory.buffer, offset + TOUCH_OFF_SEQ, 1);
  const view = new DataView(memory.buffer, offset, INPUT_TOUCH_BLOCK_BYTES);
  const little = true;

  const odd = (Atomics.load(seq, 0) + 1) | 0;
  Atomics.store(seq, 0, odd);

  view.setUint32(TOUCH_OFF_POINT_COUNT, touch.length, little);
  for (let i = 0; i < INPUT_TOUCH_MAX_POINTS; i++) {
    const p = touch[i];
    const pointOffset = TOUCH_OFF_POINTS + i * INPUT_TOUCH_POINT_BYTES;
    view.setUint32(pointOffset + TOUCH_POINT_OFF_FINGER_ID, p?.fingerId ?? 0, little);
    view.setFloat32(pointOffset + TOUCH_POINT_OFF_X, p?.x ?? 0, little);
    view.setFloat32(pointOffset + TOUCH_POINT_OFF_Y, p?.y ?? 0, little);
  }

  Atomics.store(seq, 0, (odd + 1) | 0);
}

/**
 * Decodes the Input_Controller_State the `input_region_read_controller_ffi`
 * export returns (core/stubs/wasm_entry.c) - a plain decoded struct, NOT
 * the region's seqlock wire format above (no sequence word, and every
 * offset past `buttons` is 4 bytes earlier). e2e-only: this is the
 * cross-language round-trip check that the real writer (above) and the
 * real reader (core/common/input_region.c) agree on the protocol, even
 * though no HLE service consumes it until Phase 4.
 */
export function readControllerStateFfi(memory: WebAssembly.Memory, ptr: number): ControllerState {
  const view = new DataView(memory.buffer, ptr, 52);
  const little = true;
  return {
    buttons: view.getUint32(0, little),
    axes: Array.from({ length: 8 }, (_, i) => view.getInt16(4 + i * 2, little)),
    flags: view.getUint32(20, little),
    motionSample: view.getUint32(24, little),
    accel: Array.from({ length: 3 }, (_, i) => view.getFloat32(28 + i * 4, little)),
    gyro: Array.from({ length: 3 }, (_, i) => view.getFloat32(40 + i * 4, little)),
  };
}
