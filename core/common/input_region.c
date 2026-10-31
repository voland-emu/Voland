#include "common/input_region.h"
#include "common/assert.h"
#include "common/seqlock.h"

#include <string.h>

/* Field offsets within a controller slot (§18 - see input_region.h). */
#define CONTROLLER_OFF_SEQ           0
#define CONTROLLER_OFF_BUTTONS       4
#define CONTROLLER_OFF_AXES          8
#define CONTROLLER_OFF_FLAGS         24
#define CONTROLLER_OFF_MOTION_SAMPLE 28
#define CONTROLLER_OFF_ACCEL         32
#define CONTROLLER_OFF_GYRO          44

#define TOUCH_OFF_SEQ             0
#define TOUCH_OFF_POINT_COUNT     4
#define TOUCH_OFF_POINTS          8
#define TOUCH_POINT_OFF_FINGER_ID 0
#define TOUCH_POINT_OFF_X         4
#define TOUCH_POINT_OFF_Y         8

static uint8_t *region_base(void)
{
  return (uint8_t *)(uintptr_t)layout_get()->input_region_base;
}

static uint8_t *controller_slot(uint32_t slot)
{
  SWITCH_ASSERT(slot < LAYOUT_INPUT_REGION_MAX_CONTROLLERS, "input_region: controller slot out of range");
  return region_base() + (uint64_t)slot * LAYOUT_INPUT_REGION_SLOT_BYTES;
}

static uint8_t *touch_block(void)
{
  return region_base() + LAYOUT_INPUT_REGION_MAX_CONTROLLERS * LAYOUT_INPUT_REGION_SLOT_BYTES;
}

/* Byte-exact pack/unpack rather than overlaying a C struct: field offsets
 * are the §18 wire format, which must not move with a compiler's padding
 * choices (CLAUDE.md's portability rule - cf. Trace_Event's "no packing
 * attribute" comment, §23). */
static void controller_pack(uint8_t *slot, const Input_Controller_State *state)
{
  memcpy(slot + CONTROLLER_OFF_BUTTONS, &state->buttons, sizeof(state->buttons));
  memcpy(slot + CONTROLLER_OFF_AXES, state->axes, sizeof(state->axes));
  memcpy(slot + CONTROLLER_OFF_FLAGS, &state->flags, sizeof(state->flags));
  memcpy(slot + CONTROLLER_OFF_MOTION_SAMPLE, &state->motion_sample, sizeof(state->motion_sample));
  memcpy(slot + CONTROLLER_OFF_ACCEL, state->accel, sizeof(state->accel));
  memcpy(slot + CONTROLLER_OFF_GYRO, state->gyro, sizeof(state->gyro));
}

static void controller_unpack(const uint8_t *slot, Input_Controller_State *state)
{
  memcpy(&state->buttons, slot + CONTROLLER_OFF_BUTTONS, sizeof(state->buttons));
  memcpy(state->axes, slot + CONTROLLER_OFF_AXES, sizeof(state->axes));
  memcpy(&state->flags, slot + CONTROLLER_OFF_FLAGS, sizeof(state->flags));
  memcpy(&state->motion_sample, slot + CONTROLLER_OFF_MOTION_SAMPLE, sizeof(state->motion_sample));
  memcpy(state->accel, slot + CONTROLLER_OFF_ACCEL, sizeof(state->accel));
  memcpy(state->gyro, slot + CONTROLLER_OFF_GYRO, sizeof(state->gyro));
}

void input_region_write_controller(uint32_t slot_index, const Input_Controller_State *state)
{
  uint8_t *slot = controller_slot(slot_index);
  volatile atomic_uint *seq = (volatile atomic_uint *)(slot + CONTROLLER_OFF_SEQ);
  const uint32_t odd = seqlock_write_begin(seq);
  controller_pack(slot, state);
  seqlock_write_end(seq, odd);
}

void input_region_read_controller(uint32_t slot_index, Input_Controller_State *out)
{
  uint8_t *slot = controller_slot(slot_index);
  volatile atomic_uint *seq = (volatile atomic_uint *)(slot + CONTROLLER_OFF_SEQ);
  uint32_t begin;
  do
  {
    begin = seqlock_read_begin(seq);
    controller_unpack(slot, out);
  } while (seqlock_read_retry(seq, begin));
}

static void touch_pack(uint8_t *block, const Input_Touch_State *state)
{
  memcpy(block + TOUCH_OFF_POINT_COUNT, &state->point_count, sizeof(state->point_count));
  for (uint32_t i = 0; i < LAYOUT_INPUT_TOUCH_MAX_POINTS; i++)
  {
    uint8_t *p = block + TOUCH_OFF_POINTS + i * LAYOUT_INPUT_TOUCH_POINT_BYTES;
    memcpy(p + TOUCH_POINT_OFF_FINGER_ID, &state->points[i].finger_id, sizeof(uint32_t));
    memcpy(p + TOUCH_POINT_OFF_X, &state->points[i].x, sizeof(float));
    memcpy(p + TOUCH_POINT_OFF_Y, &state->points[i].y, sizeof(float));
  }
}

static void touch_unpack(const uint8_t *block, Input_Touch_State *state)
{
  memcpy(&state->point_count, block + TOUCH_OFF_POINT_COUNT, sizeof(state->point_count));
  for (uint32_t i = 0; i < LAYOUT_INPUT_TOUCH_MAX_POINTS; i++)
  {
    const uint8_t *p = block + TOUCH_OFF_POINTS + i * LAYOUT_INPUT_TOUCH_POINT_BYTES;
    memcpy(&state->points[i].finger_id, p + TOUCH_POINT_OFF_FINGER_ID, sizeof(uint32_t));
    memcpy(&state->points[i].x, p + TOUCH_POINT_OFF_X, sizeof(float));
    memcpy(&state->points[i].y, p + TOUCH_POINT_OFF_Y, sizeof(float));
  }
}

void input_region_write_touch(const Input_Touch_State *state)
{
  SWITCH_ASSERT(state->point_count <= LAYOUT_INPUT_TOUCH_MAX_POINTS,
                "input_region: touch point_count out of range");
  uint8_t *block = touch_block();
  volatile atomic_uint *seq = (volatile atomic_uint *)(block + TOUCH_OFF_SEQ);
  const uint32_t odd = seqlock_write_begin(seq);
  touch_pack(block, state);
  seqlock_write_end(seq, odd);
}

void input_region_read_touch(Input_Touch_State *out)
{
  uint8_t *block = touch_block();
  volatile atomic_uint *seq = (volatile atomic_uint *)(block + TOUCH_OFF_SEQ);
  uint32_t begin;
  do
  {
    begin = seqlock_read_begin(seq);
    touch_unpack(block, out);
  } while (seqlock_read_retry(seq, begin));
}
