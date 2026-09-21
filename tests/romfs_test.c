/**
 * Unit tests for core/hle/loader/romfs.c.
 */
#define CHECK_NAME "romfs_test"
#include "check.h"

#include "common/arena.h"
#include "hle/loader/romfs.h"
#include "loader_fixtures.h"

#include <string.h>

#define ARENA_BYTES (1024 * 1024)

static const char k_link[] = "LINK-BFRES";
static const char k_zelda[] = "ZELDA-BFRES";
static const char k_readme[] = "readme at root";
static const char k_deep[] = "deep";

/* A tree with a root file, two levels of directories, an empty directory,
 * and enough names that a tiny bucket count forces chain collisions. */
static const Fixture_RomFS_Entry k_tree[] = {
    {"/readme.txt", false, k_readme, sizeof(k_readme) - 1},
    {"/actor", true, NULL, 0},
    {"/actor/Link.bfres", false, k_link, sizeof(k_link) - 1},
    {"/actor/Zelda.bfres", false, k_zelda, sizeof(k_zelda) - 1},
    {"/actor/sub", true, NULL, 0},
    {"/actor/sub/deep.bin", false, k_deep, sizeof(k_deep) - 1},
    {"/empty", true, NULL, 0},
    {"/actor/empty.bin", false, k_deep, 0},
};
#define TREE_COUNT (sizeof(k_tree) / sizeof(k_tree[0]))

typedef struct Opened {
  Fixture_Buffer image;
  Byte_Source src;
  Arena arena;
  RomFS fs;
} Opened;

static void open_tree(Opened *o, uint32_t bucket_count) {
  fixture_build_romfs(k_tree, TREE_COUNT, bucket_count, &o->image);
  o->src = byte_source_from_memory(o->image.bytes, o->image.size);
  CHECK(arena_create(&o->arena, ARENA_BYTES));
  CHECK_OK(romfs_open(&o->src, &o->arena, &o->fs));
}

static void close_tree(Opened *o) {
  arena_destroy(&o->arena);
  fixture_buffer_free(&o->image);
}

static bool name_is(const char *name, uint32_t length, const char *expected) {
  return strlen(expected) == length && memcmp(name, expected, length) == 0;
}

static void check_lookups(const RomFS *fs) {
  RomFS_File_Entry file;
  RomFS_Dir_Entry dir;
  char out[32];

  CHECK_OK(romfs_find_file(fs, "/actor/Link.bfres", &file));
  CHECK(name_is(file.name, file.name_length, "Link.bfres"));
  CHECK(file.data_size == sizeof(k_link) - 1);
  CHECK_OK(romfs_read_file(fs, &file, 0, out, file.data_size));
  CHECK(memcmp(out, k_link, file.data_size) == 0);

  /* Leading slash optional; same entry either way. */
  RomFS_File_Entry again;
  CHECK_OK(romfs_find_file(fs, "actor/Link.bfres", &again));
  CHECK(again.offset == file.offset);

  CHECK_OK(romfs_find_file(fs, "/readme.txt", &file));
  CHECK_OK(romfs_read_file(fs, &file, 7, out, 7));
  CHECK(memcmp(out, "at root", 7) == 0);

  CHECK_OK(romfs_find_file(fs, "/actor/sub/deep.bin", &file));
  CHECK(file.data_size == 4);
  CHECK_OK(romfs_find_file(fs, "/actor/empty.bin", &file));
  CHECK(file.data_size == 0);
  CHECK_OK(romfs_read_file(fs, &file, 0, out, 0));

  /* Directories, including the root under both spellings and a trailing slash. */
  CHECK_OK(romfs_find_dir(fs, "", &dir));
  CHECK(dir.offset == ROMFS_ROOT_DIR_OFFSET && dir.name_length == 0);
  CHECK_OK(romfs_find_dir(fs, "/", &dir));
  CHECK(dir.offset == ROMFS_ROOT_DIR_OFFSET);
  CHECK_OK(romfs_find_dir(fs, "/actor", &dir));
  CHECK(name_is(dir.name, dir.name_length, "actor"));
  CHECK(dir.parent == ROMFS_ROOT_DIR_OFFSET);
  CHECK_OK(romfs_find_dir(fs, "/actor/sub/", &dir));
  CHECK(name_is(dir.name, dir.name_length, "sub"));
  CHECK_OK(romfs_find_dir(fs, "/empty", &dir));
  CHECK(dir.first_child_dir == ROMFS_NO_ENTRY && dir.first_child_file == ROMFS_NO_ENTRY);

  /* Misses and wrong kinds. */
  CHECK_CODE(romfs_find_file(fs, "/actor/Ganon.bfres", &file), RESULT_NOT_FOUND);
  CHECK_CODE(romfs_find_file(fs, "/nope/Link.bfres", &file), RESULT_NOT_FOUND);
  CHECK_CODE(romfs_find_file(fs, "/actor", &file), RESULT_NOT_FOUND);       /* a dir */
  CHECK_CODE(romfs_find_dir(fs, "/readme.txt", &dir), RESULT_NOT_FOUND);    /* a file */
  CHECK_CODE(romfs_find_file(fs, "/actor/link.bfres", &file), RESULT_NOT_FOUND); /* case */
  CHECK_CODE(romfs_find_file(fs, "/actor//Link.bfres", &file), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_find_file(fs, "/actor/Link.bfres/", &file), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_find_file(fs, "/", &file), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_find_dir(fs, "//actor", &dir), RESULT_INVALID_ARGUMENT);
}

static void test_lookups_default_buckets(void) {
  Opened o;
  open_tree(&o, 0);
  check_lookups(&o.fs);
  close_tree(&o);
}

/* One bucket: every entry lands in the same chain, so lookup must walk
 * past non-matching entries (and past same-name entries in other dirs). */
static void test_lookups_colliding_buckets(void) {
  Opened o;
  open_tree(&o, 1);
  CHECK(o.fs.dir_bucket_count == 1 && o.fs.file_bucket_count == 1);
  check_lookups(&o.fs);
  close_tree(&o);
}

static void test_enumeration(void) {
  Opened o;
  open_tree(&o, 0);
  RomFS_Dir_Entry actor, child;
  RomFS_File_Entry file;
  CHECK_OK(romfs_find_dir(&o.fs, "/actor", &actor));

  /* Child dirs of /actor: just "sub". */
  CHECK_OK(romfs_dir_entry(&o.fs, actor.first_child_dir, &child));
  CHECK(name_is(child.name, child.name_length, "sub"));
  CHECK(child.next_sibling == ROMFS_NO_ENTRY);

  /* Child files of /actor in list order: Link, Zelda, empty.bin. */
  CHECK_OK(romfs_file_entry(&o.fs, actor.first_child_file, &file));
  CHECK(name_is(file.name, file.name_length, "Link.bfres"));
  CHECK_OK(romfs_file_entry(&o.fs, file.next_sibling, &file));
  CHECK(name_is(file.name, file.name_length, "Zelda.bfres"));
  CHECK_OK(romfs_file_entry(&o.fs, file.next_sibling, &file));
  CHECK(name_is(file.name, file.name_length, "empty.bin"));
  CHECK(file.next_sibling == ROMFS_NO_ENTRY);

  /* Root's child dirs: actor then empty. */
  RomFS_Dir_Entry root;
  CHECK_OK(romfs_dir_entry(&o.fs, ROMFS_ROOT_DIR_OFFSET, &root));
  CHECK_OK(romfs_dir_entry(&o.fs, root.first_child_dir, &child));
  CHECK(name_is(child.name, child.name_length, "actor"));
  CHECK_OK(romfs_dir_entry(&o.fs, child.next_sibling, &child));
  CHECK(name_is(child.name, child.name_length, "empty"));
  CHECK(child.next_sibling == ROMFS_NO_ENTRY);

  /* Bad entry offsets are rejected, not dereferenced. */
  CHECK_CODE(romfs_dir_entry(&o.fs, ROMFS_NO_ENTRY, &child), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_dir_entry(&o.fs, o.fs.dir_table_size, &child), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_dir_entry(&o.fs, 2, &child), RESULT_INVALID_ARGUMENT); /* misaligned */
  CHECK_CODE(romfs_file_entry(&o.fs, o.fs.file_table_size - 4, &file), RESULT_INVALID_ARGUMENT);
  close_tree(&o);
}

static void test_file_source_and_ranges(void) {
  Opened o;
  open_tree(&o, 0);
  RomFS_File_Entry file;
  CHECK_OK(romfs_find_file(&o.fs, "/actor/Zelda.bfres", &file));
  Byte_Source_Slice slice;
  CHECK_OK(romfs_file_source(&o.fs, &file, &slice));
  CHECK(slice.source.size == sizeof(k_zelda) - 1);
  char out[16];
  CHECK_OK(byte_source_read(&slice.source, 6, out, 5));
  CHECK(memcmp(out, "BFRES", 5) == 0);
  CHECK_CODE(romfs_read_file(&o.fs, &file, file.data_size, out, 1), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_read_file(&o.fs, &file, 1, out, file.data_size), RESULT_INVALID_ARGUMENT);
  close_tree(&o);
}

static void test_path_hash_vectors(void) {
  /* Hand-computed from the definition: seed ^ parent, rotate-right 5, xor byte. */
  CHECK(romfs_path_hash(0, "", 0) == 123456789u);
  uint32_t h = 123456789u;
  h = (h >> 5) | (h << 27); h ^= 'a';
  CHECK(romfs_path_hash(0, "a", 1) == h);
  CHECK(romfs_path_hash(0x18, "a", 1) != romfs_path_hash(0, "a", 1));
  CHECK(romfs_path_hash(0, "ab", 2) == fixture_romfs_hash(0, "ab", 2));
}

/* ---- corrupt images ---------------------------------------------- */

static void expect_open_failure(void (*mutate)(Fixture_Buffer *), Result expected) {
  Fixture_Buffer image;
  fixture_build_romfs(k_tree, TREE_COUNT, 0, &image);
  mutate(&image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, ARENA_BYTES));
  RomFS fs;
  CHECK_CODE(romfs_open(&src, &arena, &fs), expected);
  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

static void mutate_header_size(Fixture_Buffer *b) { fixture_put_le64(b->bytes, 0x58); }
static void mutate_table_past_source(Fixture_Buffer *b) {
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_FILE_TABLE_SIZE, b->size);
}
static void mutate_table_overflow(Fixture_Buffer *b) {
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_TABLE_OFFSET, UINT64_MAX - 8);
}
static void mutate_table_over_cap(Fixture_Buffer *b) {
  /* Inside a fake huge source the parser would still refuse the arena hit;
   * here the size also exceeds the source, so either check fires first. */
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_TABLE_SIZE, ROMFS_MAX_TABLE_BYTES + 1);
}
static void mutate_table_in_header(Fixture_Buffer *b) {
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_HASH_OFFSET, 0x10);
}
static void mutate_hash_misaligned(Fixture_Buffer *b) {
  const uint64_t size = fixture_get_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_HASH_SIZE);
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_HASH_SIZE, size - 1);
}
static void mutate_data_offset_past_source(Fixture_Buffer *b) {
  fixture_put_le64(b->bytes + FIXTURE_ROMFS_OFFSET_FILE_DATA_OFFSET, b->size + 1);
}
static void mutate_root_name_runs_off(Fixture_Buffer *b) {
  const uint64_t dir_table = fixture_get_le64(b->bytes + FIXTURE_ROMFS_OFFSET_DIR_TABLE_OFFSET);
  fixture_put_le32(b->bytes + dir_table + 0x14, 0x7FFFFFFF); /* root name length */
}

static void test_open_rejections(void) {
  expect_open_failure(mutate_header_size, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_table_past_source, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_table_overflow, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_table_over_cap, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_table_in_header, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_hash_misaligned, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_data_offset_past_source, RESULT_INVALID_ARGUMENT);
  expect_open_failure(mutate_root_name_runs_off, RESULT_INVALID_ARGUMENT);
}

/* Corruption that only surfaces during a walk. */
static void test_walk_rejections(void) {
  Opened o;
  open_tree(&o, 1); /* one bucket: every lookup walks the whole chain */
  RomFS_File_Entry file;
  RomFS_Dir_Entry dir;

  /* Make the first file in the chain point back at itself: cyclic. */
  uint8_t *file_table = (uint8_t *)(uintptr_t)o.fs.file_table;
  const uint32_t head = o.fs.file_hash_table[0];
  fixture_put_le32(file_table + head + 0x18, head);
  CHECK_CODE(romfs_find_file(&o.fs, "/does-not-exist", &file), RESULT_INVALID_ARGUMENT);

  /* A file whose data range escapes the source is rejected at decode. */
  fixture_put_le64(file_table + head + 0x10, UINT64_MAX);
  CHECK_CODE(romfs_file_entry(&o.fs, head, &file), RESULT_INVALID_ARGUMENT);

  /* A dir chain link past the table. */
  uint8_t *dir_table = (uint8_t *)(uintptr_t)o.fs.dir_table;
  const uint32_t dir_head = o.fs.dir_hash_table[0];
  fixture_put_le32(dir_table + dir_head + 0x10, o.fs.dir_table_size + 4);
  CHECK_CODE(romfs_find_dir(&o.fs, "/does-not-exist", &dir), RESULT_INVALID_ARGUMENT);

  /* Path length cap. */
  char long_path[ROMFS_MAX_PATH_BYTES + 2];
  memset(long_path, 'a', sizeof(long_path) - 1);
  long_path[0] = '/';
  long_path[sizeof(long_path) - 1] = '\0';
  CHECK_CODE(romfs_find_file(&o.fs, long_path, &file), RESULT_INVALID_ARGUMENT);
  close_tree(&o);
}

static void test_arena_exhaustion_and_nulls(void) {
  Fixture_Buffer image;
  fixture_build_romfs(k_tree, TREE_COUNT, 0, &image);
  Byte_Source src = byte_source_from_memory(image.bytes, image.size);
  Arena arena;
  CHECK(arena_create(&arena, 16));
  RomFS fs;
  CHECK_CODE(romfs_open(&src, &arena, &fs), RESULT_OUT_OF_MEMORY);
  CHECK_CODE(romfs_open(NULL, &arena, &fs), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_open(&src, NULL, &fs), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(romfs_open(&src, &arena, NULL), RESULT_INVALID_ARGUMENT);
  arena_destroy(&arena);
  fixture_buffer_free(&image);
}

int main(void) {
  test_lookups_default_buckets();
  test_lookups_colliding_buckets();
  test_enumeration();
  test_file_source_and_ranges();
  test_path_hash_vectors();
  test_open_rejections();
  test_walk_rejections();
  test_arena_exhaustion_and_nulls();
  printf("[romfs_test] all tests passed\n");
  return 0;
}
