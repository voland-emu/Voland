/**
 * Unit tests for src/input.ts's pure logic (§18): the Standard Gamepad
 * button remap, axis scaling, and the touch coordinate tracker. These
 * are the parts of the main-thread input writer most likely to have a
 * subtle off-by-one or boundary bug and, before this file, had zero
 * coverage - bindings/input.test.ts only covers the byte-packing layer
 * underneath. No DOM is used: a Gamepad-shaped plain object stands in
 * for a real Gamepad, and a plain object with getBoundingClientRect/
 * addEventListener stands in for a canvas (no jsdom dependency, per
 * CLAUDE.md's "no third-party dependencies not listed in DESIGN.md").
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { InputFlags } from "../../bindings/input.ts";
import { STANDARD_BUTTON_MAP, axisToI16, createTouchTracker, readControllerState } from "../../src/input.ts";

function fakeGamepad(pressedStandardIndices: readonly number[], axes: readonly number[] = []): Gamepad {
  const buttons = Array.from({ length: 17 }, (_, i) => ({
    pressed: pressedStandardIndices.includes(i),
    touched: false,
    value: 0,
  }));
  return { connected: true, buttons, axes } as unknown as Gamepad;
}

type FakeCanvas = { handlers: Map<string, (event: unknown) => void> };

function fakeCanvas(rect: { left: number; top: number; width: number; height: number }): {
  canvas: HTMLCanvasElement;
  fire: (type: string, event: Record<string, unknown>) => void;
} {
  const state: FakeCanvas = { handlers: new Map() };
  const canvas = {
    getBoundingClientRect: () => rect,
    addEventListener: (type: string, handler: (event: unknown) => void) => state.handlers.set(type, handler),
  } as unknown as HTMLCanvasElement;
  return {
    canvas,
    fire: (type, event) => state.handlers.get(type)?.(event),
  };
}

test("STANDARD_BUTTON_MAP has 17 entries, each a distinct Standard Gamepad index 0-16", () => {
  assert.equal(STANDARD_BUTTON_MAP.length, 17);
  assert.equal(new Set(STANDARD_BUTTON_MAP).size, STANDARD_BUTTON_MAP.length);
  STANDARD_BUTTON_MAP.forEach((i) => assert.ok(i >= 0 && i <= 16));
});

test("readControllerState maps each Switch bit from exactly its documented Standard Gamepad button", () => {
  for (let switchBit = 0; switchBit < STANDARD_BUTTON_MAP.length; switchBit++) {
    const standardIndex = STANDARD_BUTTON_MAP[switchBit];
    const state = readControllerState(fakeGamepad([standardIndex]));
    assert.equal(
      state.buttons,
      1 << switchBit,
      `pressing Standard Gamepad button ${standardIndex} should set only Switch bit ${switchBit}, got 0b${state.buttons.toString(2)}`,
    );
  }
});

test("readControllerState: A/B and X/Y are swapped relative to the Standard Gamepad (§18)", () => {
  // Standard Gamepad south (0) -> Switch B (bit 1); east (1) -> Switch A (bit 0).
  assert.equal(readControllerState(fakeGamepad([0])).buttons, 1 << 1);
  assert.equal(readControllerState(fakeGamepad([1])).buttons, 1 << 0);
  // Standard Gamepad west (2) -> Switch Y (bit 3); north (3) -> Switch X (bit 2).
  assert.equal(readControllerState(fakeGamepad([2])).buttons, 1 << 3);
  assert.equal(readControllerState(fakeGamepad([3])).buttons, 1 << 2);
});

test("readControllerState combines multiple pressed buttons into one bitmask", () => {
  const state = readControllerState(fakeGamepad([0, 1, 16]));
  assert.equal(state.buttons, (1 << 1) | (1 << 0) | (1 << 16));
});

test("readControllerState reports Connected and motionSample=0 (no WebHID IMU source yet)", () => {
  const state = readControllerState(fakeGamepad([]));
  assert.equal(state.flags, InputFlags.Connected);
  assert.equal(state.motionSample, 0);
  assert.deepEqual(state.accel, [0, 0, 0]);
  assert.deepEqual(state.gyro, [0, 0, 0]);
});

test("readControllerState scales axes from -1..1 floats into the i16 wire range, zero-fills the rest", () => {
  const state = readControllerState(fakeGamepad([], [0, 1, -1, 0.5, -0.5]));
  assert.deepEqual(state.axes.slice(0, 5), [0, 32767, -32767, 16384, -16383]);
  assert.deepEqual(state.axes.slice(5), [0, 0, 0]);
});

test("axisToI16 clamps values outside -1..1 (real hardware briefly reports these)", () => {
  assert.equal(axisToI16(2), 32767);
  assert.equal(axisToI16(-2), -32767);
  assert.equal(axisToI16(1), 32767);
  assert.equal(axisToI16(-1), -32767);
  assert.equal(axisToI16(0), 0);
});

test("createTouchTracker converts client coordinates to the §18 1280x720 screen space", () => {
  const { canvas, fire } = fakeCanvas({ left: 100, top: 50, width: 640, height: 360 });
  const readTouchPoints = createTouchTracker(canvas);

  fire("pointerdown", { pointerType: "touch", pointerId: 1, clientX: 100 + 320, clientY: 50 + 180 });

  const points = readTouchPoints();
  assert.equal(points.length, 1);
  assert.equal(points[0].fingerId, 1);
  assert.equal(points[0].x, 640); // halfway across a 640-wide rect -> 640 of 1280
  assert.equal(points[0].y, 360); // halfway down a 360-high rect -> 360 of 720
});

test("createTouchTracker ignores non-touch pointer types (mouse, pen)", () => {
  const { canvas, fire } = fakeCanvas({ left: 0, top: 0, width: 1, height: 1 });
  const readTouchPoints = createTouchTracker(canvas);

  fire("pointerdown", { pointerType: "mouse", pointerId: 1, clientX: 0, clientY: 0 });

  assert.equal(readTouchPoints().length, 0);
});

test("createTouchTracker updates a tracked point on pointermove and removes it on pointerup", () => {
  const { canvas, fire } = fakeCanvas({ left: 0, top: 0, width: 1280, height: 720 });
  const readTouchPoints = createTouchTracker(canvas);

  fire("pointerdown", { pointerType: "touch", pointerId: 5, clientX: 0, clientY: 0 });
  assert.deepEqual(readTouchPoints().map((p) => [p.x, p.y]), [[0, 0]]);

  fire("pointermove", { pointerType: "touch", pointerId: 5, clientX: 640, clientY: 360 });
  assert.deepEqual(readTouchPoints().map((p) => [p.x, p.y]), [[640, 360]]);

  fire("pointerup", { pointerType: "touch", pointerId: 5, clientX: 640, clientY: 360 });
  assert.equal(readTouchPoints().length, 0);
});

test("createTouchTracker removes a point on pointercancel too", () => {
  const { canvas, fire } = fakeCanvas({ left: 0, top: 0, width: 1280, height: 720 });
  const readTouchPoints = createTouchTracker(canvas);

  fire("pointerdown", { pointerType: "touch", pointerId: 9, clientX: 0, clientY: 0 });
  assert.equal(readTouchPoints().length, 1);

  fire("pointercancel", { pointerType: "touch", pointerId: 9, clientX: 0, clientY: 0 });
  assert.equal(readTouchPoints().length, 0);
});

test("createTouchTracker caps active touches at 10 (§18)", () => {
  const { canvas, fire } = fakeCanvas({ left: 0, top: 0, width: 1280, height: 720 });
  const readTouchPoints = createTouchTracker(canvas);

  for (let i = 0; i < 12; i++) {
    fire("pointerdown", { pointerType: "touch", pointerId: i, clientX: 0, clientY: 0 });
  }

  assert.equal(readTouchPoints().length, 10);
});
