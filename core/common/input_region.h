/**
 * Input region: seqlock-protected controller and touch state in the fixed
 * `input_region_base` layout region (§4/§18). One writer (the main thread
 * on web, each rAF; a native main thread once a native input backend
 * lands, §17) publishes Gamepad/WebHID/touch state; any reader (the CPU
 * Worker's hid: service, Phase 4) reads it back through seqlock.h's retry
 * loop. This header is the C-side half of that contract - readers and
 * writers that run in the core (tests today; hid: from Phase 4) go
 * through these functions rather than hand-rolling offsets, matching
 * layout.h's "nobody addresses their own shared buffer" rule.
 *
 * The web writer (platform/web/src/input.ts) does NOT call into this - it
 * is JS writing the same byte layout directly from the main thread, no
 * FFI hop per rAF. Its constants and this header's must be kept in
 * lockstep by hand, the same relationship layout.h already has with
 * bindings/layout.ts.
 *
 * Wire format (§18, v3.29 - DESIGN.md is authoritative; mirrored here):
 *
 *   Per controller slot (LAYOUT_INPUT_REGION_MAX_CONTROLLERS slots,
 *   LAYOUT_INPUT_REGION_SLOT_BYTES bytes each):
 *     +0  sequence (u32, atomic, seqlock)
 *     +4  buttons bitmask (u32)
 *     +8  axes (8 x i16)
 *     +24 flags (u32: connected, npad style, profile id - §12)
 *     +28 motion sample counter (u32; 0 = no sensor)
 *     +32 accelerometer (3 x f32, g)
 *     +44 gyroscope (3 x f32, rad/s)
 *     +56 reserved (8 bytes)
 *
 *   One touch block, immediately after the last controller slot:
 *     +0  sequence (u32, atomic, seqlock)
 *     +4  point count (u32, <= LAYOUT_INPUT_TOUCH_MAX_POINTS)
 *     +8  points, LAYOUT_INPUT_TOUCH_MAX_POINTS x 12 bytes (finger id u32,
 *         x f32, y f32, in 1280x720 screen space)
 *
 * Every function here is a value copy through the seqlock retry loop, not
 * a pointer into the region: callers never hold a pointer across a
 * possible write. Nothing here is fallible: the region is reserved at
 * layout_create() alongside guest RAM, and the only other input (slot
 * index / point count) is debug-asserted, not Error-returned.
 */
#ifndef SWITCH_COMMON_INPUT_REGION_H
#define SWITCH_COMMON_INPUT_REGION_H

#include <stdint.h>

#include "common/layout.h"

typedef struct Input_Controller_State
{
  uint32_t buttons;
  int16_t  axes[8];
  uint32_t flags;
  uint32_t motion_sample;
  float    accel[3];
  float    gyro[3];
} Input_Controller_State;

typedef struct Input_Touch_Point
{
  uint32_t finger_id;
  float    x;
  float    y;
} Input_Touch_Point;

typedef struct Input_Touch_State
{
  uint32_t          point_count; /* <= LAYOUT_INPUT_TOUCH_MAX_POINTS */
  Input_Touch_Point points[LAYOUT_INPUT_TOUCH_MAX_POINTS];
} Input_Touch_State;

/* Reads controller `slot` (< LAYOUT_INPUT_REGION_MAX_CONTROLLERS) through
 * the seqlock retry loop. Debug-asserts on an out-of-range slot (a
 * programmer error, never a runtime condition). */
void input_region_read_controller(uint32_t slot, Input_Controller_State *out);

/* Writes controller `slot`'s state through the seqlock protocol. The
 * native main-thread writer (§17, once it exists) and tests call this;
 * the web writer does not (see the header comment). */
void input_region_write_controller(uint32_t slot, const Input_Controller_State *state);

/* Reads the touch block through the seqlock retry loop. */
void input_region_read_touch(Input_Touch_State *out);

/* Writes the touch block. Debug-asserts state->point_count is in range. */
void input_region_write_touch(const Input_Touch_State *state);

#endif /* SWITCH_COMMON_INPUT_REGION_H */
