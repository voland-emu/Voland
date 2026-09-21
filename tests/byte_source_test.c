/**
 * Unit tests for core/hle/loader/byte_source.c.
 */
#define CHECK_NAME "byte_source_test"
#include "check.h"

#include "hle/loader/byte_source.h"

#include <string.h>

static int g_read_calls = 0;

static Error counting_read(void *user, uint64_t offset, void *out, uint64_t size) {
  g_read_calls++;
  memcpy(out, (const uint8_t *)user + offset, (size_t)size);
  return OK;
}

static Error failing_read(void *user, uint64_t offset, void *out, uint64_t size) {
  (void)user; (void)offset; (void)out; (void)size;
  return ERR(RESULT_IO_ERROR, "test: simulated IO failure");
}

static void test_range_checks(void) {
  uint8_t data[16];
  for (int i = 0; i < 16; i++) data[i] = (uint8_t)i;
  Byte_Source src = byte_source_from_memory(data, sizeof(data));
  uint8_t out[16];

  CHECK_OK(byte_source_read(&src, 0, out, 16));
  CHECK(memcmp(out, data, 16) == 0);
  CHECK_OK(byte_source_read(&src, 12, out, 4));
  CHECK(out[0] == 12 && out[3] == 15);

  CHECK_CODE(byte_source_read(&src, 16, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_read(&src, 15, out, 2), RESULT_INVALID_ARGUMENT);
  /* size == 0 is a no-op before any range check, per the header. */
  CHECK_OK(byte_source_read(&src, 17, out, 0));
  CHECK_CODE(byte_source_read(NULL, 0, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_read(&src, 0, NULL, 1), RESULT_INVALID_ARGUMENT);

  /* offset + size overflow must not wrap into a valid range. */
  CHECK_CODE(byte_source_read(&src, UINT64_MAX, out, 2), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_read(&src, 8, out, UINT64_MAX - 4), RESULT_INVALID_ARGUMENT);
}

static void test_zero_size_never_calls_read(void) {
  uint8_t data[4] = {1, 2, 3, 4};
  Byte_Source src = {data, sizeof(data), counting_read};
  uint8_t out[4];
  g_read_calls = 0;
  CHECK_OK(byte_source_read(&src, 0, out, 0));
  CHECK_OK(byte_source_read(&src, 4, out, 0)); /* at the very end is fine */
  CHECK(g_read_calls == 0);
  CHECK_OK(byte_source_read(&src, 1, out, 2));
  CHECK(g_read_calls == 1);
  CHECK(out[0] == 2 && out[1] == 3);
}

static void test_read_error_propagates(void) {
  uint8_t data[4] = {0};
  Byte_Source src = {data, sizeof(data), failing_read};
  uint8_t out[4];
  Error err = byte_source_read(&src, 0, out, 4);
  CHECK(err.code == RESULT_IO_ERROR);
  CHECK(err.message != NULL);
}

static void test_slices(void) {
  uint8_t data[32];
  for (int i = 0; i < 32; i++) data[i] = (uint8_t)(0x40 + i);
  Byte_Source parent = byte_source_from_memory(data, sizeof(data));

  Byte_Source_Slice slice;
  CHECK_OK(byte_source_slice(&parent, 8, 16, &slice));
  CHECK(slice.source.size == 16);
  uint8_t out[16];
  CHECK_OK(byte_source_read(&slice.source, 0, out, 16));
  CHECK(out[0] == 0x48 && out[15] == 0x57);
  CHECK_CODE(byte_source_read(&slice.source, 16, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_read(&slice.source, 15, out, 2), RESULT_INVALID_ARGUMENT);

  /* Slice of a slice composes offsets. */
  Byte_Source_Slice inner;
  CHECK_OK(byte_source_slice(&slice.source, 4, 4, &inner));
  CHECK_OK(byte_source_read(&inner.source, 1, out, 2));
  CHECK(out[0] == 0x4D && out[1] == 0x4E);

  /* Windows past the parent are rejected. */
  CHECK_CODE(byte_source_slice(&parent, 32, 1, &slice), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_slice(&parent, 31, 2, &slice), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_slice(&parent, UINT64_MAX, 1, &slice), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_slice(NULL, 0, 1, &slice), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(byte_source_slice(&parent, 0, 1, NULL), RESULT_INVALID_ARGUMENT);

  /* Zero-size window is legal and reads nothing. */
  CHECK_OK(byte_source_slice(&parent, 32, 0, &slice));
  CHECK_OK(byte_source_read(&slice.source, 0, out, 0));
  CHECK_CODE(byte_source_read(&slice.source, 0, out, 1), RESULT_INVALID_ARGUMENT);
}

static void test_le_decoders_unaligned(void) {
  uint8_t raw[9] = {0xFF, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  CHECK(byte_source_le16(raw + 1) == 0x0201);
  CHECK(byte_source_le32(raw + 1) == 0x04030201u);
  CHECK(byte_source_le64(raw + 1) == 0x0807060504030201ull);
}

int main(void) {
  test_range_checks();
  test_zero_size_never_calls_read();
  test_read_error_propagates();
  test_slices();
  test_le_decoders_unaligned();
  printf("[byte_source_test] all tests passed\n");
  return 0;
}
