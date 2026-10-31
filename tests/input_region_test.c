/**
 * Unit test for core/common/input_region.c (§18 Phase 1: input region +
 * seqlock writer/reader). Drives the real write/read functions over a
 * live layout_create()'d region - no mocks - and checks round-tripping,
 * slot isolation, and the touch block.
 */
#include "common/input_region.h"
#include "common/layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                   \
  do                                                                   \
  {                                                                    \
    if (!(cond))                                                       \
    {                                                                  \
      fprintf(stderr, "[input_region_test] FAIL %s:%d: %s\n", __FILE__, \
              __LINE__, #cond);                                        \
      exit(1);                                                         \
    }                                                                   \
  } while (0)

static Input_Controller_State controller_fixture(uint32_t seed)
{
  Input_Controller_State s;
  memset(&s, 0, sizeof(s));
  s.buttons = 0x1000u + seed;
  for (int i = 0; i < 8; i++) s.axes[i] = (int16_t)(seed * 10 + i);
  s.flags = 0x20u + seed;
  s.motion_sample = seed;
  for (int i = 0; i < 3; i++) s.accel[i] = (float)seed + 0.5f * (float)i;
  for (int i = 0; i < 3; i++) s.gyro[i] = -(float)seed - 0.25f * (float)i;
  return s;
}

static void check_controller_equal(const Input_Controller_State *a, const Input_Controller_State *b)
{
  CHECK(a->buttons == b->buttons);
  CHECK(memcmp(a->axes, b->axes, sizeof(a->axes)) == 0);
  CHECK(a->flags == b->flags);
  CHECK(a->motion_sample == b->motion_sample);
  CHECK(memcmp(a->accel, b->accel, sizeof(a->accel)) == 0);
  CHECK(memcmp(a->gyro, b->gyro, sizeof(a->gyro)) == 0);
}

static void test_controller_round_trip_and_isolation(void)
{
  Input_Controller_State written[LAYOUT_INPUT_REGION_MAX_CONTROLLERS];
  for (uint32_t slot = 0; slot < LAYOUT_INPUT_REGION_MAX_CONTROLLERS; slot++)
  {
    written[slot] = controller_fixture(slot + 1);
    input_region_write_controller(slot, &written[slot]);
  }

  /* Every slot reads back exactly what was written to it - writing slot
   * N must not have touched any other slot. */
  for (uint32_t slot = 0; slot < LAYOUT_INPUT_REGION_MAX_CONTROLLERS; slot++)
  {
    Input_Controller_State got;
    memset(&got, 0xAA, sizeof(got));
    input_region_read_controller(slot, &got);
    check_controller_equal(&written[slot], &got);
  }

  /* Overwriting one slot leaves its neighbors untouched. */
  Input_Controller_State replacement = controller_fixture(100);
  input_region_write_controller(3, &replacement);
  Input_Controller_State got3, got2, got4;
  input_region_read_controller(3, &got3);
  input_region_read_controller(2, &got2);
  input_region_read_controller(4, &got4);
  check_controller_equal(&replacement, &got3);
  check_controller_equal(&written[2], &got2);
  check_controller_equal(&written[4], &got4);
}

static void test_touch_round_trip(void)
{
  Input_Touch_State state;
  memset(&state, 0, sizeof(state));
  state.point_count = 3;
  state.points[0] = (Input_Touch_Point){.finger_id = 1, .x = 10.5f, .y = 20.25f};
  state.points[1] = (Input_Touch_Point){.finger_id = 2, .x = 640.0f, .y = 360.0f};
  state.points[2] = (Input_Touch_Point){.finger_id = 7, .x = 1279.0f, .y = 719.0f};

  input_region_write_touch(&state);

  Input_Touch_State got;
  memset(&got, 0xAA, sizeof(got));
  input_region_read_touch(&got);

  CHECK(got.point_count == 3);
  for (int i = 0; i < 3; i++)
  {
    CHECK(got.points[i].finger_id == state.points[i].finger_id);
    CHECK(got.points[i].x == state.points[i].x);
    CHECK(got.points[i].y == state.points[i].y);
  }
  /* Points past point_count are whatever was last written there (0, from
   * the memset above) - not load-bearing, but worth pinning down. */
  CHECK(got.points[9].finger_id == 0);

  /* An empty touch frame (no fingers down) round-trips too. */
  Input_Touch_State empty;
  memset(&empty, 0, sizeof(empty));
  input_region_write_touch(&empty);
  input_region_read_touch(&got);
  CHECK(got.point_count == 0);
}

static void test_touch_does_not_alias_controllers(void)
{
  /* The touch block sits immediately after controller slot 7; writing it
   * must not corrupt slot 7's state. */
  Input_Controller_State slot7 = controller_fixture(42);
  input_region_write_controller(7, &slot7);

  Input_Touch_State touch;
  memset(&touch, 0, sizeof(touch));
  touch.point_count = 10;
  for (uint32_t i = 0; i < 10; i++)
    touch.points[i] = (Input_Touch_Point){.finger_id = i, .x = (float)i, .y = (float)i};
  input_region_write_touch(&touch);

  Input_Controller_State got;
  input_region_read_controller(7, &got);
  check_controller_equal(&slot7, &got);
}

int main(void)
{
  CHECK(layout_create().code == RESULT_OK);

  test_controller_round_trip_and_isolation();
  test_touch_round_trip();
  test_touch_does_not_alias_controllers();

  layout_destroy();

  printf("[input_region_test] OK\n");
  return 0;
}
