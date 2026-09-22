/**
 * RomFS reader. See romfs.h for the layout and the validation policy:
 * tables are range-checked at open, entries at decode, and chain walks
 * are step-bounded so a corrupt image terminates with an error.
 */
#include "hle/loader/romfs.h"

#include "common/log.h"

#include <string.h>

/* Header field offsets (u64 each). */
#define ROMFS_HEADER_OFFSET_HEADER_SIZE ((uint64_t)0x00)
#define ROMFS_HEADER_OFFSET_DIR_HASH_OFFSET ((uint64_t)0x08)
#define ROMFS_HEADER_OFFSET_DIR_HASH_SIZE ((uint64_t)0x10)
#define ROMFS_HEADER_OFFSET_DIR_TABLE_OFFSET ((uint64_t)0x18)
#define ROMFS_HEADER_OFFSET_DIR_TABLE_SIZE ((uint64_t)0x20)
#define ROMFS_HEADER_OFFSET_FILE_HASH_OFFSET ((uint64_t)0x28)
#define ROMFS_HEADER_OFFSET_FILE_HASH_SIZE ((uint64_t)0x30)
#define ROMFS_HEADER_OFFSET_FILE_TABLE_OFFSET ((uint64_t)0x38)
#define ROMFS_HEADER_OFFSET_FILE_TABLE_SIZE ((uint64_t)0x40)
#define ROMFS_HEADER_OFFSET_FILE_DATA_OFFSET ((uint64_t)0x48)

/* Dir entry field offsets. */
#define ROMFS_DIR_OFFSET_PARENT ((uint32_t)0x00)
#define ROMFS_DIR_OFFSET_NEXT_SIBLING ((uint32_t)0x04)
#define ROMFS_DIR_OFFSET_FIRST_CHILD_DIR ((uint32_t)0x08)
#define ROMFS_DIR_OFFSET_FIRST_CHILD_FILE ((uint32_t)0x0C)
#define ROMFS_DIR_OFFSET_NEXT_IN_BUCKET ((uint32_t)0x10)
#define ROMFS_DIR_OFFSET_NAME_LENGTH ((uint32_t)0x14)

/* File entry field offsets. */
#define ROMFS_FILE_OFFSET_PARENT ((uint32_t)0x00)
#define ROMFS_FILE_OFFSET_NEXT_SIBLING ((uint32_t)0x04)
#define ROMFS_FILE_OFFSET_DATA_OFFSET ((uint32_t)0x08)
#define ROMFS_FILE_OFFSET_DATA_SIZE ((uint32_t)0x10)
#define ROMFS_FILE_OFFSET_NEXT_IN_BUCKET ((uint32_t)0x18)
#define ROMFS_FILE_OFFSET_NAME_LENGTH ((uint32_t)0x1C)

#define ROMFS_PATH_SEPARATOR '/'
#define ROMFS_PATH_TERMINATOR ((char)0)

/* ------------------------------------------------------------------ */
/* Open.                                                               */
/* ------------------------------------------------------------------ */

typedef struct Table_Range {
  uint64_t offset;
  uint64_t size;
} Table_Range;

static Error validate_table(const Byte_Source *source, Table_Range range,
                            const char *what) {
  if (range.offset < ROMFS_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: table overlaps header");
  }
  if (range.offset > source->size || range.size > source->size - range.offset) {
    log_warn("romfs: %s table outside source", what);
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: table outside source");
  }
  /* Entry links are u32 offsets into the table, so a table past 4GB could
   * never be addressed anyway. */
  if (range.size > UINT32_MAX) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: table too large");
  }
  return OK;
}

static Error load_table(const Byte_Source *source, Arena *arena, Table_Range range,
                        size_t alignment, const uint8_t **out) {
  /* A zero-size table still gets a valid (non-NULL) pointer. */
  uint8_t *buffer = (uint8_t *)arena_allocate(arena, range.size == 0 ? 1 : (size_t)range.size,
                                              alignment);
  if (!buffer) return ERR(RESULT_OUT_OF_MEMORY, "romfs: arena exhausted");
  Error err = byte_source_read(source, range.offset, buffer, range.size);
  if (!error_is_ok(err)) return err;
  *out = buffer;
  return OK;
}

/* Hash tables are u32 arrays on media; decode each bucket in place so the
 * lookup code can index them as uint32_t on any host. */
static void decode_hash_table(uint32_t *table, uint32_t bucket_count) {
  for (uint32_t i = 0; i < bucket_count; i++) {
    table[i] = byte_source_le32((const uint8_t *)&table[i]);
  }
}

Error romfs_open(const Byte_Source *source, Arena *arena, RomFS *out) {
  if (!source || !arena || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs_open: NULL argument");
  }
  memset(out, 0, sizeof(*out));

  uint8_t header[ROMFS_HEADER_SIZE];
  Error err = byte_source_read(source, 0, header, ROMFS_HEADER_SIZE);
  if (!error_is_ok(err)) return err;
  if (byte_source_le64(header + ROMFS_HEADER_OFFSET_HEADER_SIZE) != ROMFS_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: header size field mismatch");
  }

  const Table_Range dir_hash = {byte_source_le64(header + ROMFS_HEADER_OFFSET_DIR_HASH_OFFSET),
                                byte_source_le64(header + ROMFS_HEADER_OFFSET_DIR_HASH_SIZE)};
  const Table_Range dir_table = {byte_source_le64(header + ROMFS_HEADER_OFFSET_DIR_TABLE_OFFSET),
                                 byte_source_le64(header + ROMFS_HEADER_OFFSET_DIR_TABLE_SIZE)};
  const Table_Range file_hash = {byte_source_le64(header + ROMFS_HEADER_OFFSET_FILE_HASH_OFFSET),
                                 byte_source_le64(header + ROMFS_HEADER_OFFSET_FILE_HASH_SIZE)};
  const Table_Range file_table = {byte_source_le64(header + ROMFS_HEADER_OFFSET_FILE_TABLE_OFFSET),
                                  byte_source_le64(header + ROMFS_HEADER_OFFSET_FILE_TABLE_SIZE)};
  const uint64_t file_data_offset = byte_source_le64(header + ROMFS_HEADER_OFFSET_FILE_DATA_OFFSET);

  if (!error_is_ok(err = validate_table(source, dir_hash, "dir hash"))) return err;
  if (!error_is_ok(err = validate_table(source, dir_table, "dir"))) return err;
  if (!error_is_ok(err = validate_table(source, file_hash, "file hash"))) return err;
  if (!error_is_ok(err = validate_table(source, file_table, "file"))) return err;
  if (dir_hash.size % sizeof(uint32_t) != 0 || file_hash.size % sizeof(uint32_t) != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: hash table size not a multiple of 4");
  }
  if (dir_hash.size + dir_table.size + file_hash.size + file_table.size > ROMFS_MAX_TABLE_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: tables over cap");
  }
  if (file_data_offset > source->size) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: file data offset outside source");
  }

  const uint8_t *dir_hash_bytes = NULL;
  const uint8_t *file_hash_bytes = NULL;
  if (!error_is_ok(err = load_table(source, arena, dir_hash, _Alignof(uint32_t), &dir_hash_bytes))) return err;
  if (!error_is_ok(err = load_table(source, arena, dir_table, 1, &out->dir_table))) return err;
  if (!error_is_ok(err = load_table(source, arena, file_hash, _Alignof(uint32_t), &file_hash_bytes))) return err;
  if (!error_is_ok(err = load_table(source, arena, file_table, 1, &out->file_table))) return err;

  out->source = source;
  out->file_data_offset = file_data_offset;
  out->dir_table_size = (uint32_t)dir_table.size;
  out->file_table_size = (uint32_t)file_table.size;
  out->dir_bucket_count = (uint32_t)(dir_hash.size / sizeof(uint32_t));
  out->file_bucket_count = (uint32_t)(file_hash.size / sizeof(uint32_t));
  /* load_table handed us arena memory; the casts drop the const it added. */
  out->dir_hash_table = (const uint32_t *)dir_hash_bytes;
  out->file_hash_table = (const uint32_t *)file_hash_bytes;
  decode_hash_table((uint32_t *)(uintptr_t)out->dir_hash_table, out->dir_bucket_count);
  decode_hash_table((uint32_t *)(uintptr_t)out->file_hash_table, out->file_bucket_count);

  RomFS_Dir_Entry root;
  err = romfs_dir_entry(out, ROMFS_ROOT_DIR_OFFSET, &root);
  if (!error_is_ok(err)) return err;
  log_debug("romfs: opened, dir table %u bytes, file table %u bytes",
            (unsigned)out->dir_table_size, (unsigned)out->file_table_size);
  return OK;
}

/* ------------------------------------------------------------------ */
/* Entry decoding.                                                     */
/* ------------------------------------------------------------------ */

static Error check_entry_bounds(uint32_t offset, uint32_t fixed_size, uint32_t table_size,
                                uint32_t name_length) {
  if (offset == ROMFS_NO_ENTRY || offset % ROMFS_NAME_ALIGNMENT != 0) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: bad entry offset");
  }
  if (offset > table_size || fixed_size > table_size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: entry runs past table");
  }
  if (name_length > ROMFS_MAX_NAME_BYTES || name_length > table_size - offset - fixed_size) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: entry name runs past table");
  }
  return OK;
}

Error romfs_dir_entry(const RomFS *fs, uint32_t offset, RomFS_Dir_Entry *out) {
  if (!fs || !out) return ERR(RESULT_INVALID_ARGUMENT, "romfs_dir_entry: NULL argument");
  if (offset == ROMFS_NO_ENTRY || offset > fs->dir_table_size ||
      ROMFS_DIR_ENTRY_FIXED_SIZE > fs->dir_table_size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: dir entry runs past table");
  }
  const uint8_t *raw = fs->dir_table + offset;
  const uint32_t name_length = byte_source_le32(raw + ROMFS_DIR_OFFSET_NAME_LENGTH);
  Error err = check_entry_bounds(offset, ROMFS_DIR_ENTRY_FIXED_SIZE, fs->dir_table_size, name_length);
  if (!error_is_ok(err)) return err;

  out->offset = offset;
  out->parent = byte_source_le32(raw + ROMFS_DIR_OFFSET_PARENT);
  out->next_sibling = byte_source_le32(raw + ROMFS_DIR_OFFSET_NEXT_SIBLING);
  out->first_child_dir = byte_source_le32(raw + ROMFS_DIR_OFFSET_FIRST_CHILD_DIR);
  out->first_child_file = byte_source_le32(raw + ROMFS_DIR_OFFSET_FIRST_CHILD_FILE);
  out->name = (const char *)(raw + ROMFS_DIR_ENTRY_FIXED_SIZE);
  out->name_length = name_length;
  return OK;
}

Error romfs_file_entry(const RomFS *fs, uint32_t offset, RomFS_File_Entry *out) {
  if (!fs || !out) return ERR(RESULT_INVALID_ARGUMENT, "romfs_file_entry: NULL argument");
  if (offset == ROMFS_NO_ENTRY || offset > fs->file_table_size ||
      ROMFS_FILE_ENTRY_FIXED_SIZE > fs->file_table_size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: file entry runs past table");
  }
  const uint8_t *raw = fs->file_table + offset;
  const uint32_t name_length = byte_source_le32(raw + ROMFS_FILE_OFFSET_NAME_LENGTH);
  Error err = check_entry_bounds(offset, ROMFS_FILE_ENTRY_FIXED_SIZE, fs->file_table_size, name_length);
  if (!error_is_ok(err)) return err;

  const uint64_t data_offset = byte_source_le64(raw + ROMFS_FILE_OFFSET_DATA_OFFSET);
  const uint64_t data_size = byte_source_le64(raw + ROMFS_FILE_OFFSET_DATA_SIZE);
  const uint64_t data_region_size = fs->source->size - fs->file_data_offset;
  if (data_offset > data_region_size || data_size > data_region_size - data_offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: file data outside source");
  }

  out->offset = offset;
  out->parent = byte_source_le32(raw + ROMFS_FILE_OFFSET_PARENT);
  out->next_sibling = byte_source_le32(raw + ROMFS_FILE_OFFSET_NEXT_SIBLING);
  out->data_offset = fs->file_data_offset + data_offset;
  out->data_size = data_size;
  out->name = (const char *)(raw + ROMFS_FILE_ENTRY_FIXED_SIZE);
  out->name_length = name_length;
  return OK;
}

/* ------------------------------------------------------------------ */
/* Lookup.                                                             */
/* ------------------------------------------------------------------ */

uint32_t romfs_path_hash(uint32_t parent_dir_offset, const char *name,
                         uint32_t name_length) {
  uint32_t hash = parent_dir_offset ^ ROMFS_PATH_HASH_SEED;
  for (uint32_t i = 0; i < name_length; i++) {
    hash = (hash >> 5) | (hash << 27);
    hash ^= (uint8_t)name[i];
  }
  return hash;
}

/* Bucket-chain search for a child directory named `name` of `parent`.
 * RESULT_NOT_FOUND when the chain ends; RESULT_INVALID_ARGUMENT on a
 * corrupt link or a chain longer than the table could hold. */
static Error find_child_dir(const RomFS *fs, uint32_t parent, const char *name,
                            uint32_t name_length, RomFS_Dir_Entry *out) {
  if (fs->dir_bucket_count == 0) return ERR(RESULT_NOT_FOUND, "romfs: no directories");
  const uint32_t bucket = romfs_path_hash(parent, name, name_length) % fs->dir_bucket_count;
  const uint32_t max_steps = fs->dir_table_size / ROMFS_DIR_ENTRY_FIXED_SIZE + 1;
  uint32_t offset = fs->dir_hash_table[bucket];
  for (uint32_t step = 0; offset != ROMFS_NO_ENTRY; step++) {
    if (step >= max_steps) return ERR(RESULT_INVALID_ARGUMENT, "romfs: cyclic dir hash chain");
    Error err = romfs_dir_entry(fs, offset, out);
    if (!error_is_ok(err)) return err;
    if (out->parent == parent && out->name_length == name_length &&
        memcmp(out->name, name, name_length) == 0) {
      return OK;
    }
    offset = byte_source_le32(fs->dir_table + offset + ROMFS_DIR_OFFSET_NEXT_IN_BUCKET);
  }
  return ERR(RESULT_NOT_FOUND, "romfs: directory not found");
}

static Error find_child_file(const RomFS *fs, uint32_t parent, const char *name,
                             uint32_t name_length, RomFS_File_Entry *out) {
  if (fs->file_bucket_count == 0) return ERR(RESULT_NOT_FOUND, "romfs: no files");
  const uint32_t bucket = romfs_path_hash(parent, name, name_length) % fs->file_bucket_count;
  const uint32_t max_steps = fs->file_table_size / ROMFS_FILE_ENTRY_FIXED_SIZE + 1;
  uint32_t offset = fs->file_hash_table[bucket];
  for (uint32_t step = 0; offset != ROMFS_NO_ENTRY; step++) {
    if (step >= max_steps) return ERR(RESULT_INVALID_ARGUMENT, "romfs: cyclic file hash chain");
    Error err = romfs_file_entry(fs, offset, out);
    if (!error_is_ok(err)) return err;
    if (out->parent == parent && out->name_length == name_length &&
        memcmp(out->name, name, name_length) == 0) {
      return OK;
    }
    offset = byte_source_le32(fs->file_table + offset + ROMFS_FILE_OFFSET_NEXT_IN_BUCKET);
  }
  return ERR(RESULT_NOT_FOUND, "romfs: file not found");
}

/* Walks every component but the last as a directory, starting at the
 * root. On success *parent is the containing directory and *leaf and
 * *leaf_length name the final component (leaf_length == 0 means the
 * path named the root itself). */
static Error walk_to_parent(const RomFS *fs, const char *path, uint32_t *parent,
                            const char **leaf, uint32_t *leaf_length,
                            bool *had_trailing_separator) {
  /* Bounded length scan (strnlen is POSIX, not C11). */
  size_t path_length = 0;
  while (path_length <= ROMFS_MAX_PATH_BYTES && path[path_length] != ROMFS_PATH_TERMINATOR) path_length++;
  if (path_length > ROMFS_MAX_PATH_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: path too long");
  }
  const char *cursor = path;
  const char *end = path + path_length;
  if (cursor < end && *cursor == ROMFS_PATH_SEPARATOR) cursor++;

  *had_trailing_separator = (end > path && end[-1] == ROMFS_PATH_SEPARATOR && end - 1 >= cursor);
  if (*had_trailing_separator) end--;

  uint32_t current = ROMFS_ROOT_DIR_OFFSET;
  *leaf = cursor;
  *leaf_length = 0;
  while (cursor < end) {
    const char *component = cursor;
    while (cursor < end && *cursor != ROMFS_PATH_SEPARATOR) cursor++;
    const uint32_t component_length = (uint32_t)(cursor - component);
    if (component_length == 0) {
      return ERR(RESULT_INVALID_ARGUMENT, "romfs: empty path component");
    }
    if (cursor == end) {
      *leaf = component;
      *leaf_length = component_length;
      break;
    }
    RomFS_Dir_Entry dir;
    Error err = find_child_dir(fs, current, component, component_length, &dir);
    if (!error_is_ok(err)) return err;
    current = dir.offset;
    cursor++; /* skip the separator */
  }
  *parent = current;
  return OK;
}

Error romfs_find_dir(const RomFS *fs, const char *path, RomFS_Dir_Entry *out) {
  if (!fs || !path || !out) return ERR(RESULT_INVALID_ARGUMENT, "romfs_find_dir: NULL argument");
  uint32_t parent;
  const char *leaf;
  uint32_t leaf_length;
  bool trailing;
  Error err = walk_to_parent(fs, path, &parent, &leaf, &leaf_length, &trailing);
  if (!error_is_ok(err)) return err;
  if (leaf_length == 0) return romfs_dir_entry(fs, ROMFS_ROOT_DIR_OFFSET, out);
  return find_child_dir(fs, parent, leaf, leaf_length, out);
}

Error romfs_find_file(const RomFS *fs, const char *path, RomFS_File_Entry *out) {
  if (!fs || !path || !out) return ERR(RESULT_INVALID_ARGUMENT, "romfs_find_file: NULL argument");
  uint32_t parent;
  const char *leaf;
  uint32_t leaf_length;
  bool trailing;
  Error err = walk_to_parent(fs, path, &parent, &leaf, &leaf_length, &trailing);
  if (!error_is_ok(err)) return err;
  if (leaf_length == 0 || trailing) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs: file path names a directory");
  }
  return find_child_file(fs, parent, leaf, leaf_length, out);
}

/* ------------------------------------------------------------------ */
/* File data.                                                          */
/* ------------------------------------------------------------------ */

Error romfs_read_file(const RomFS *fs, const RomFS_File_Entry *entry,
                      uint64_t offset, void *out, uint64_t size) {
  if (!fs || !entry) return ERR(RESULT_INVALID_ARGUMENT, "romfs_read_file: NULL argument");
  if (offset > entry->data_size || size > entry->data_size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "romfs_read_file: range outside file");
  }
  return byte_source_read(fs->source, entry->data_offset + offset, out, size);
}

Error romfs_file_source(const RomFS *fs, const RomFS_File_Entry *entry,
                        Byte_Source_Slice *out) {
  if (!fs || !entry || !out) return ERR(RESULT_INVALID_ARGUMENT, "romfs_file_source: NULL argument");
  return byte_source_slice(fs->source, entry->data_offset, entry->data_size, out);
}
