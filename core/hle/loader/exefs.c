/**
 * ExeFS (PFS0) reader. See exefs.h for the layout.
 */
#include "hle/loader/exefs.h"

#include "common/log.h"

#include <string.h>

/* Entry field offsets (bytes from the start of the entry). */
#define EXEFS_ENTRY_OFFSET_DATA_OFFSET ((uint64_t)0x00)
#define EXEFS_ENTRY_OFFSET_DATA_SIZE ((uint64_t)0x08)
#define EXEFS_ENTRY_OFFSET_NAME_OFFSET ((uint64_t)0x10)

/* Header field offsets. */
#define EXEFS_HEADER_OFFSET_FILE_COUNT ((uint64_t)0x04)
#define EXEFS_HEADER_OFFSET_STRING_TABLE_SIZE ((uint64_t)0x08)

Error exefs_open(const Byte_Source *source, Arena *arena, ExeFS *out) {
  if (!source || !arena || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs_open: NULL argument");
  }
  memset(out, 0, sizeof(*out));

  uint8_t header[EXEFS_HEADER_SIZE];
  Error err = byte_source_read(source, 0, header, EXEFS_HEADER_SIZE);
  if (!error_is_ok(err)) return err;
  if (byte_source_le32(header) != EXEFS_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs: PFS0 magic missing");
  }
  const uint32_t file_count = byte_source_le32(header + EXEFS_HEADER_OFFSET_FILE_COUNT);
  const uint32_t string_table_size =
      byte_source_le32(header + EXEFS_HEADER_OFFSET_STRING_TABLE_SIZE);
  if (file_count > EXEFS_MAX_FILE_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs: file count over cap");
  }
  if (string_table_size > EXEFS_MAX_STRING_TABLE_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs: string table over cap");
  }

  const uint64_t entry_table_size = (uint64_t)file_count * EXEFS_ENTRY_SIZE;
  const uint64_t string_table_offset = EXEFS_HEADER_SIZE + entry_table_size;
  const uint64_t data_region_offset = string_table_offset + string_table_size;
  if (data_region_offset > source->size) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs: source too small for directory");
  }

  /* Pull the entry table and the string table into the arena. The string
   * table gets one extra byte so an unterminated final name cannot run
   * off the end while we validate it. */
  uint8_t *raw_entries = ARENA_ALLOC_ARRAY(arena, uint8_t, entry_table_size == 0 ? 1 : entry_table_size);
  char *string_table = ARENA_ALLOC_ARRAY(arena, char, (size_t)string_table_size + 1);
  ExeFS_Entry *entries = ARENA_ALLOC_ARRAY(arena, ExeFS_Entry, file_count == 0 ? 1 : file_count);
  if (!raw_entries || !string_table || !entries) {
    return ERR(RESULT_OUT_OF_MEMORY, "exefs: arena exhausted");
  }
  err = byte_source_read(source, EXEFS_HEADER_SIZE, raw_entries, entry_table_size);
  if (!error_is_ok(err)) return err;
  err = byte_source_read(source, string_table_offset, string_table, string_table_size);
  if (!error_is_ok(err)) return err;
  string_table[string_table_size] = '\0';

  const uint64_t data_region_size = source->size - data_region_offset;
  for (uint32_t i = 0; i < file_count; i++) {
    const uint8_t *raw = raw_entries + (uint64_t)i * EXEFS_ENTRY_SIZE;
    const uint64_t data_offset = byte_source_le64(raw + EXEFS_ENTRY_OFFSET_DATA_OFFSET);
    const uint64_t data_size = byte_source_le64(raw + EXEFS_ENTRY_OFFSET_DATA_SIZE);
    const uint32_t name_offset = byte_source_le32(raw + EXEFS_ENTRY_OFFSET_NAME_OFFSET);

    if (data_offset > data_region_size || data_size > data_region_size - data_offset) {
      return ERR(RESULT_INVALID_ARGUMENT, "exefs: entry data outside source");
    }
    if (name_offset >= string_table_size) {
      return ERR(RESULT_INVALID_ARGUMENT, "exefs: entry name offset outside string table");
    }
    /* The name must terminate inside the real table, not at our pad byte. */
    if (memchr(string_table + name_offset, '\0', string_table_size - name_offset) == NULL) {
      return ERR(RESULT_INVALID_ARGUMENT, "exefs: entry name not NUL-terminated");
    }

    entries[i].name = string_table + name_offset;
    entries[i].offset = data_region_offset + data_offset;
    entries[i].size = data_size;
  }

  out->source = source;
  out->file_count = file_count;
  out->entries = entries;
  out->data_region_offset = data_region_offset;
  log_debug("exefs: opened, %u files", (unsigned)file_count);
  return OK;
}

const ExeFS_Entry *exefs_find(const ExeFS *fs, const char *name) {
  if (!fs || !name) return NULL;
  for (uint32_t i = 0; i < fs->file_count; i++) {
    if (strcmp(fs->entries[i].name, name) == 0) return &fs->entries[i];
  }
  return NULL;
}

Error exefs_read(const ExeFS *fs, const ExeFS_Entry *entry, uint64_t offset,
                 void *out, uint64_t size) {
  if (!fs || !entry) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs_read: NULL argument");
  }
  if (offset > entry->size || size > entry->size - offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs_read: range outside entry");
  }
  return byte_source_read(fs->source, entry->offset + offset, out, size);
}

Error exefs_entry_source(const ExeFS *fs, const ExeFS_Entry *entry,
                         Byte_Source_Slice *out) {
  if (!fs || !entry || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "exefs_entry_source: NULL argument");
  }
  return byte_source_slice(fs->source, entry->offset, entry->size, out);
}
