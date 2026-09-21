/**
 * Unit tests for core/hle/loader/exefs.c (PFS0).
 */
#define CHECK_NAME "exefs_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/exefs.h"
#include "loader_fixtures.h"

#include <string.h>

#define ARENA_BYTES (256 * 1024)

static const char k_npdm[] = "npdm-bytes";
static const char k_main[] = "MAIN-EXECUTABLE-CONTENT";
static const char k_rtld[] = "";

static const Fixture_File k_files[] = {
    {EXEFS_FILE_NPDM, k_npdm, sizeof(k_npdm) - 1},
    {EXEFS_FILE_MAIN, k_main, sizeof(k_main) - 1},
    {EXEFS_FILE_RTLD, k_rtld, 0},
};
#define FILE_COUNT (sizeof(k_files) / sizeof(k_files[0]))

static void test_open_find_read(void) {
  Fixture_Buffer image;
  fixture_build_pfs0(k_files, FILE_COUNT, &image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, ARENA_BYTES));

  ExeFS fs;
  CHECK_OK(exefs_open(&src, &arena, &fs));
  CHECK(fs.file_count == FILE_COUNT);
  CHECK(fs.data_region_offset ==
        FIXTURE_PFS0_HEADER_SIZE + FILE_COUNT * FIXTURE_PFS0_ENTRY_SIZE +
            (strlen(EXEFS_FILE_NPDM) + 1 + strlen(EXEFS_FILE_MAIN) + 1 + strlen(EXEFS_FILE_RTLD) + 1));

  const ExeFS_Entry *main_entry = exefs_find(&fs, EXEFS_FILE_MAIN);
  CHECK(main_entry != NULL);
  CHECK(strcmp(main_entry->name, EXEFS_FILE_MAIN) == 0);
  CHECK(main_entry->size == sizeof(k_main) - 1);
  /* main follows npdm in the data region; offsets are absolute. */
  CHECK(main_entry->offset == fs.data_region_offset + (sizeof(k_npdm) - 1));
  CHECK(memcmp(image.bytes + main_entry->offset, k_main, main_entry->size) == 0);

  char out[64];
  CHECK_OK(exefs_read(&fs, main_entry, 0, out, main_entry->size));
  CHECK(memcmp(out, k_main, main_entry->size) == 0);
  CHECK_OK(exefs_read(&fs, main_entry, 5, out, 4));
  CHECK(memcmp(out, "EXEC", 4) == 0);
  CHECK_OK(exefs_read(&fs, main_entry, main_entry->size, out, 0));
  CHECK_CODE(exefs_read(&fs, main_entry, main_entry->size, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(exefs_read(&fs, main_entry, 1, out, main_entry->size), RESULT_INVALID_ARGUMENT);

  const ExeFS_Entry *rtld = exefs_find(&fs, EXEFS_FILE_RTLD);
  CHECK(rtld != NULL && rtld->size == 0);
  CHECK_OK(exefs_read(&fs, rtld, 0, out, 0));

  CHECK(exefs_find(&fs, "subsdk0") == NULL);
  CHECK(exefs_find(&fs, "MAIN") == NULL); /* case-sensitive */
  CHECK(exefs_find(&fs, "mai") == NULL);  /* exact, not prefix */

  Byte_Source_Slice slice;
  CHECK_OK(exefs_entry_source(&fs, main_entry, &slice));
  CHECK(slice.source.size == main_entry->size);
  CHECK_OK(byte_source_read(&slice.source, 0, out, 4));
  CHECK(memcmp(out, "MAIN", 4) == 0);

  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

static void test_empty_partition(void) {
  Fixture_Buffer image;
  fixture_build_pfs0(NULL, 0, &image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, ARENA_BYTES));
  ExeFS fs;
  CHECK_OK(exefs_open(&src, &arena, &fs));
  CHECK(fs.file_count == 0);
  CHECK(exefs_find(&fs, EXEFS_FILE_MAIN) == NULL);
  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

/* Builds the image, applies `mutate`, and expects `expected` from open. */
static void expect_open_failure(void (*mutate)(Fixture_Buffer *), Result expected) {
  Fixture_Buffer image;
  fixture_build_pfs0(k_files, FILE_COUNT, &image);
  mutate(&image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, ARENA_BYTES));
  ExeFS fs;
  CHECK_CODE(exefs_open(&src, &arena, &fs), expected);
  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

static void mutate_magic(Fixture_Buffer *b) { b->bytes[0] = 'X'; }
static void mutate_count_over_cap(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_PFS0_OFFSET_FILE_COUNT, EXEFS_MAX_FILE_COUNT + 1);
}
static void mutate_string_table_over_cap(Fixture_Buffer *b) {
  fixture_put_le32(b->bytes + FIXTURE_PFS0_OFFSET_STRING_TABLE_SIZE,
                   (uint32_t)EXEFS_MAX_STRING_TABLE_BYTES + 1);
}
static void mutate_directory_past_source(Fixture_Buffer *b) {
  /* Claim a string table that runs past the end of the image. */
  fixture_put_le32(b->bytes + FIXTURE_PFS0_OFFSET_STRING_TABLE_SIZE, (uint32_t)b->size);
}
static void mutate_entry_data_past_source(Fixture_Buffer *b) {
  uint8_t *entry1 = b->bytes + FIXTURE_PFS0_HEADER_SIZE + 1 * FIXTURE_PFS0_ENTRY_SIZE;
  fixture_put_le64(entry1 + 0x08, 0x1000); /* size far beyond the data region */
}
static void mutate_entry_data_overflow(Fixture_Buffer *b) {
  uint8_t *entry1 = b->bytes + FIXTURE_PFS0_HEADER_SIZE + 1 * FIXTURE_PFS0_ENTRY_SIZE;
  fixture_put_le64(entry1 + 0x00, UINT64_MAX - 2);
  fixture_put_le64(entry1 + 0x08, 8);
}
static void mutate_name_offset_past_table(Fixture_Buffer *b) {
  uint8_t *entry0 = b->bytes + FIXTURE_PFS0_HEADER_SIZE;
  fixture_put_le32(entry0 + 0x10, 0xFFFF);
}
static void mutate_name_unterminated(Fixture_Buffer *b) {
  /* Overwrite the final NUL of the last name (rtld) with a letter. */
  const size_t string_table_offset = FIXTURE_PFS0_HEADER_SIZE + FILE_COUNT * FIXTURE_PFS0_ENTRY_SIZE;
  const uint32_t string_table_size = fixture_get_le32(b->bytes + FIXTURE_PFS0_OFFSET_STRING_TABLE_SIZE);
  b->bytes[string_table_offset + string_table_size - 1] = 'z';
}

static void test_rejections(void) {
  expect_open_failure(mutate_magic, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_count_over_cap, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_string_table_over_cap, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_directory_past_source, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_entry_data_past_source, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_entry_data_overflow, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_name_offset_past_table, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_name_unterminated, RESULT_INVALID_ARGUMENT);
}

static void test_arena_exhaustion(void) {
  Fixture_Buffer image;
  fixture_build_pfs0(k_files, FILE_COUNT, &image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, 8)); /* far too small for the entry table */
  ExeFS fs;
  CHECK_CODE(exefs_open(&src, &arena, &fs), RESULT_OUT_OF_MEMORY);
  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

static void test_null_arguments(void) {
  Byte_Source src = byte_source_from_memory(NULL, 0);
  Arena arena;
  CHECK(arena_create(&arena, 64));
  ExeFS fs;
  CHECK_CODE(exefs_open(NULL, &arena, &fs), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(exefs_open(&src, NULL, &fs), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(exefs_open(&src, &arena, NULL), RESULT_INVALID_ARGUMENT);
  /* An empty source is too small for even the header. */
  CHECK_CODE(exefs_open(&src, &arena, &fs), RESULT_INVALID_ARGUMENT);
  CHECK(exefs_find(NULL, "x") == NULL);
  arena_destroy(&arena);
}

int main(void) {
  test_open_find_read();
  test_empty_partition();
  test_rejections();
  test_arena_exhaustion();
  test_null_arguments();
  printf("[exefs_test] all tests passed\n");
  return 0;
}
