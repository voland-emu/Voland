#include "loader_fixtures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Buffer.                                                             */
/* ------------------------------------------------------------------ */

static void fixture_fatal(const char *what) {
  fprintf(stderr, "[loader_fixtures] %s\n", what);
  exit(1);
}

void fixture_buffer_init(Fixture_Buffer *buffer) {
  buffer->bytes = NULL;
  buffer->size = 0;
  buffer->capacity = 0;
}

void fixture_buffer_free(Fixture_Buffer *buffer) {
  free(buffer->bytes);
  fixture_buffer_init(buffer);
}

static void fixture_reserve(Fixture_Buffer *buffer, size_t extra) {
  if (buffer->size + extra <= buffer->capacity) return;
  size_t capacity = buffer->capacity ? buffer->capacity : 256;
  while (capacity < buffer->size + extra) capacity *= 2;
  uint8_t *bytes = (uint8_t *)realloc(buffer->bytes, capacity);
  if (!bytes) fixture_fatal("out of memory");
  buffer->bytes = bytes;
  buffer->capacity = capacity;
}

size_t fixture_append(Fixture_Buffer *buffer, const void *data, size_t size) {
  fixture_reserve(buffer, size);
  const size_t at = buffer->size;
  if (size) memcpy(buffer->bytes + at, data, size);
  buffer->size += size;
  return at;
}

size_t fixture_append_zeros(Fixture_Buffer *buffer, size_t size) {
  fixture_reserve(buffer, size);
  const size_t at = buffer->size;
  if (size) memset(buffer->bytes + at, 0, size);
  buffer->size += size;
  return at;
}

void fixture_align(Fixture_Buffer *buffer, size_t alignment) {
  const size_t remainder = buffer->size % alignment;
  if (remainder) fixture_append_zeros(buffer, alignment - remainder);
}

void fixture_put_le16(uint8_t *at, uint16_t value) {
  at[0] = (uint8_t)value;
  at[1] = (uint8_t)(value >> 8);
}

void fixture_put_le32(uint8_t *at, uint32_t value) {
  for (int i = 0; i < 4; i++) at[i] = (uint8_t)(value >> (8 * i));
}

void fixture_put_le64(uint8_t *at, uint64_t value) {
  for (int i = 0; i < 8; i++) at[i] = (uint8_t)(value >> (8 * i));
}

uint32_t fixture_get_le32(const uint8_t *at) {
  uint32_t value = 0;
  for (int i = 3; i >= 0; i--) value = (value << 8) | at[i];
  return value;
}

uint64_t fixture_get_le64(const uint8_t *at) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; i--) value = (value << 8) | at[i];
  return value;
}

/* ------------------------------------------------------------------ */
/* PFS0.                                                               */
/* ------------------------------------------------------------------ */

void fixture_build_pfs0(const Fixture_File *files, uint32_t count, Fixture_Buffer *out) {
  fixture_buffer_init(out);

  uint32_t string_table_size = 0;
  for (uint32_t i = 0; i < count; i++) string_table_size += (uint32_t)strlen(files[i].name) + 1;

  uint8_t header[FIXTURE_PFS0_HEADER_SIZE] = {'P', 'F', 'S', '0'};
  fixture_put_le32(header + FIXTURE_PFS0_OFFSET_FILE_COUNT, count);
  fixture_put_le32(header + FIXTURE_PFS0_OFFSET_STRING_TABLE_SIZE, string_table_size);
  fixture_append(out, header, sizeof(header));

  uint64_t data_offset = 0;
  uint32_t name_offset = 0;
  for (uint32_t i = 0; i < count; i++) {
    uint8_t entry[FIXTURE_PFS0_ENTRY_SIZE] = {0};
    fixture_put_le64(entry + 0x00, data_offset);
    fixture_put_le64(entry + 0x08, files[i].size);
    fixture_put_le32(entry + 0x10, name_offset);
    fixture_append(out, entry, sizeof(entry));
    data_offset += files[i].size;
    name_offset += (uint32_t)strlen(files[i].name) + 1;
  }
  for (uint32_t i = 0; i < count; i++) {
    fixture_append(out, files[i].name, strlen(files[i].name) + 1);
  }
  for (uint32_t i = 0; i < count; i++) {
    fixture_append(out, files[i].data, files[i].size);
  }
}

/* ------------------------------------------------------------------ */
/* RomFS.                                                              */
/* ------------------------------------------------------------------ */

uint32_t fixture_romfs_hash(uint32_t parent, const char *name, uint32_t length) {
  uint32_t hash = parent ^ 123456789u;
  for (uint32_t i = 0; i < length; i++) {
    hash = (hash >> 5) | (hash << 27);
    hash ^= (uint8_t)name[i];
  }
  return hash;
}

typedef struct RomFS_Record {
  const char *full_path;   /* dirs: for parent matching */
  const char *name;
  uint32_t name_length;
  uint32_t parent_index;   /* index into the dir records */
  uint32_t offset;         /* offset within its table */
  uint32_t entry_size;
  uint32_t next_sibling;
  uint32_t first_child_dir;  /* dirs only */
  uint32_t first_child_file; /* dirs only */
  uint32_t next_in_bucket;
  const void *data;        /* files only */
  size_t data_size;
  uint64_t data_offset;
} RomFS_Record;

static uint32_t align4(uint32_t value) { return (value + 3u) & ~3u; }

static uint32_t find_parent_dir(const RomFS_Record *dirs, uint32_t dir_count, const char *path,
                                const char **leaf) {
  const char *slash = strrchr(path, '/');
  if (!slash || path[0] != '/') fixture_fatal("romfs fixture paths must be absolute");
  *leaf = slash + 1;
  const size_t parent_length = (size_t)(slash - path);
  for (uint32_t i = 0; i < dir_count; i++) {
    if (strlen(dirs[i].full_path) == parent_length &&
        memcmp(dirs[i].full_path, path, parent_length) == 0) {
      return i;
    }
  }
  fixture_fatal("romfs fixture: parent directory not listed before child");
  return 0;
}

static void link_records(RomFS_Record *records, uint32_t count, uint32_t bucket_count,
                         uint32_t *buckets, const RomFS_Record *dirs, bool is_dir) {
  for (uint32_t i = 0; i < count; i++) {
    records[i].next_sibling = FIXTURE_ROMFS_NO_ENTRY;
    records[i].first_child_dir = FIXTURE_ROMFS_NO_ENTRY;
    records[i].first_child_file = FIXTURE_ROMFS_NO_ENTRY;
    records[i].next_in_bucket = FIXTURE_ROMFS_NO_ENTRY;
  }
  /* Siblings: chain records sharing a parent in list order. The root
   * (dirs[0]) has no siblings and is skipped as a "child". */
  for (uint32_t i = 0; i < count; i++) {
    if (is_dir && i == 0) continue;
    for (uint32_t j = i + 1; j < count; j++) {
      if (records[j].parent_index == records[i].parent_index) {
        records[i].next_sibling = records[j].offset;
        break;
      }
    }
  }
  /* Hash buckets: prepend, so walk order is reverse list order. */
  for (uint32_t i = 0; i < bucket_count; i++) buckets[i] = FIXTURE_ROMFS_NO_ENTRY;
  for (uint32_t i = count; i-- > 0;) {
    const uint32_t parent_offset = dirs[records[i].parent_index].offset;
    const uint32_t bucket =
        fixture_romfs_hash(parent_offset, records[i].name, records[i].name_length) % bucket_count;
    records[i].next_in_bucket = buckets[bucket];
    buckets[bucket] = records[i].offset;
  }
}

void fixture_build_romfs(const Fixture_RomFS_Entry *entries, uint32_t count,
                         uint32_t bucket_count, Fixture_Buffer *out) {
  static RomFS_Record dirs[FIXTURE_ROMFS_MAX_ENTRIES + 1];
  static RomFS_Record files[FIXTURE_ROMFS_MAX_ENTRIES];
  static uint32_t dir_buckets[FIXTURE_ROMFS_MAX_ENTRIES + 1];
  static uint32_t file_buckets[FIXTURE_ROMFS_MAX_ENTRIES + 1];
  if (count > FIXTURE_ROMFS_MAX_ENTRIES) fixture_fatal("romfs fixture: too many entries");
  memset(dirs, 0, sizeof(dirs));
  memset(files, 0, sizeof(files));

  uint32_t dir_count = 1;
  uint32_t file_count = 0;
  dirs[0].full_path = "";
  dirs[0].name = "";
  dirs[0].name_length = 0;
  dirs[0].parent_index = 0;
  dirs[0].entry_size = FIXTURE_ROMFS_DIR_ENTRY_FIXED_SIZE;

  for (uint32_t i = 0; i < count; i++) {
    const char *leaf = NULL;
    const uint32_t parent = find_parent_dir(dirs, dir_count, entries[i].path, &leaf);
    RomFS_Record *record = entries[i].is_dir ? &dirs[dir_count++] : &files[file_count++];
    record->full_path = entries[i].path;
    record->name = leaf;
    record->name_length = (uint32_t)strlen(leaf);
    record->parent_index = parent;
    record->entry_size = (entries[i].is_dir ? FIXTURE_ROMFS_DIR_ENTRY_FIXED_SIZE
                                            : FIXTURE_ROMFS_FILE_ENTRY_FIXED_SIZE) +
                         align4(record->name_length);
    record->data = entries[i].data;
    record->data_size = entries[i].size;
  }

  /* Table offsets. */
  uint32_t dir_table_size = 0;
  for (uint32_t i = 0; i < dir_count; i++) {
    dirs[i].offset = dir_table_size;
    dir_table_size += dirs[i].entry_size;
  }
  uint32_t file_table_size = 0;
  uint64_t data_size = 0;
  for (uint32_t i = 0; i < file_count; i++) {
    files[i].offset = file_table_size;
    file_table_size += files[i].entry_size;
    files[i].data_offset = data_size;
    data_size += files[i].data_size;
  }

  /* Children: first dir / file whose parent is this dir. */
  const uint32_t dir_bucket_count = bucket_count ? bucket_count : dir_count;
  const uint32_t file_bucket_count = bucket_count ? bucket_count : (file_count ? file_count : 1);
  link_records(dirs, dir_count, dir_bucket_count, dir_buckets, dirs, true);
  link_records(files, file_count, file_bucket_count, file_buckets, dirs, false);
  for (uint32_t d = 0; d < dir_count; d++) {
    for (uint32_t i = 1; i < dir_count; i++) {
      if (dirs[i].parent_index == d) { dirs[d].first_child_dir = dirs[i].offset; break; }
    }
    for (uint32_t i = 0; i < file_count; i++) {
      if (files[i].parent_index == d) { dirs[d].first_child_file = files[i].offset; break; }
    }
  }

  /* Serialize. */
  fixture_buffer_init(out);
  fixture_append_zeros(out, FIXTURE_ROMFS_HEADER_SIZE);
  const uint64_t dir_hash_offset = out->size;
  for (uint32_t i = 0; i < dir_bucket_count; i++) {
    uint8_t raw[4];
    fixture_put_le32(raw, dir_buckets[i]);
    fixture_append(out, raw, 4);
  }
  const uint64_t dir_table_offset = out->size;
  for (uint32_t i = 0; i < dir_count; i++) {
    uint8_t fixed[FIXTURE_ROMFS_DIR_ENTRY_FIXED_SIZE] = {0};
    fixture_put_le32(fixed + 0x00, dirs[dirs[i].parent_index].offset);
    fixture_put_le32(fixed + 0x04, dirs[i].next_sibling);
    fixture_put_le32(fixed + 0x08, dirs[i].first_child_dir);
    fixture_put_le32(fixed + 0x0C, dirs[i].first_child_file);
    fixture_put_le32(fixed + 0x10, dirs[i].next_in_bucket);
    fixture_put_le32(fixed + 0x14, dirs[i].name_length);
    fixture_append(out, fixed, sizeof(fixed));
    fixture_append(out, dirs[i].name, dirs[i].name_length);
    fixture_align(out, 4);
  }
  const uint64_t file_hash_offset = out->size;
  for (uint32_t i = 0; i < file_bucket_count; i++) {
    uint8_t raw[4];
    fixture_put_le32(raw, file_buckets[i]);
    fixture_append(out, raw, 4);
  }
  const uint64_t file_table_offset = out->size;
  for (uint32_t i = 0; i < file_count; i++) {
    uint8_t fixed[FIXTURE_ROMFS_FILE_ENTRY_FIXED_SIZE] = {0};
    fixture_put_le32(fixed + 0x00, dirs[files[i].parent_index].offset);
    fixture_put_le32(fixed + 0x04, files[i].next_sibling);
    fixture_put_le64(fixed + 0x08, files[i].data_offset);
    fixture_put_le64(fixed + 0x10, files[i].data_size);
    fixture_put_le32(fixed + 0x18, files[i].next_in_bucket);
    fixture_put_le32(fixed + 0x1C, files[i].name_length);
    fixture_append(out, fixed, sizeof(fixed));
    fixture_append(out, files[i].name, files[i].name_length);
    fixture_align(out, 4);
  }
  fixture_align(out, 16);
  const uint64_t file_data_offset = out->size;
  for (uint32_t i = 0; i < file_count; i++) {
    fixture_append(out, files[i].data, files[i].data_size);
  }

  uint8_t *header = out->bytes;
  fixture_put_le64(header + 0x00, FIXTURE_ROMFS_HEADER_SIZE);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_DIR_HASH_OFFSET, dir_hash_offset);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_DIR_HASH_SIZE, (uint64_t)dir_bucket_count * 4);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_DIR_TABLE_OFFSET, dir_table_offset);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_DIR_TABLE_SIZE, dir_table_size);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_FILE_HASH_OFFSET, file_hash_offset);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_FILE_HASH_SIZE, (uint64_t)file_bucket_count * 4);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_FILE_TABLE_OFFSET, file_table_offset);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_FILE_TABLE_SIZE, file_table_size);
  fixture_put_le64(header + FIXTURE_ROMFS_OFFSET_FILE_DATA_OFFSET, file_data_offset);
}

/* ------------------------------------------------------------------ */
/* NPDM.                                                               */
/* ------------------------------------------------------------------ */

static void put_fixed_string(uint8_t *at, const char *text, size_t size) {
  memset(at, 0, size);
  if (text) memcpy(at, text, strlen(text) < size ? strlen(text) : size);
}

void fixture_build_npdm(const Fixture_NPDM_Params *p, Fixture_Buffer *out) {
  fixture_buffer_init(out);

  /* META */
  uint8_t meta[FIXTURE_NPDM_META_SIZE] = {'M', 'E', 'T', 'A'};
  meta[0x0C] = p->flags;
  meta[0x0E] = p->main_thread_priority;
  meta[0x0F] = p->main_thread_core;
  fixture_put_le32(meta + 0x14, p->system_resource_size);
  fixture_put_le32(meta + 0x18, p->version);
  fixture_put_le32(meta + 0x1C, p->main_thread_stack_size);
  put_fixed_string(meta + 0x20, p->name, 0x10);
  put_fixed_string(meta + 0x30, p->product_code, 0x10);
  fixture_append(out, meta, sizeof(meta));

  /* ACID */
  const uint32_t acid_caps_size = p->acid_capability_count * 4;
  const uint32_t acid_size = FIXTURE_NPDM_ACID_HEADER_SIZE + acid_caps_size;
  const uint32_t acid_offset = (uint32_t)out->size;
  uint8_t acid[FIXTURE_NPDM_ACID_HEADER_SIZE] = {0};
  memcpy(acid + 0x200, "ACID", 4);
  fixture_put_le32(acid + 0x204, acid_size);
  fixture_put_le32(acid + 0x20C, p->acid_flags);
  fixture_put_le64(acid + 0x210, p->program_id_min);
  fixture_put_le64(acid + 0x218, p->program_id_max);
  fixture_put_le32(acid + 0x220, FIXTURE_NPDM_ACID_HEADER_SIZE); /* fs access: empty */
  fixture_put_le32(acid + 0x224, 0);
  fixture_put_le32(acid + 0x228, FIXTURE_NPDM_ACID_HEADER_SIZE); /* service access: empty */
  fixture_put_le32(acid + 0x22C, 0);
  fixture_put_le32(acid + 0x230, FIXTURE_NPDM_ACID_HEADER_SIZE);
  fixture_put_le32(acid + 0x234, acid_caps_size);
  fixture_append(out, acid, sizeof(acid));
  for (uint32_t i = 0; i < p->acid_capability_count; i++) {
    uint8_t raw[4];
    fixture_put_le32(raw, p->acid_capabilities[i]);
    fixture_append(out, raw, 4);
  }

  /* ACI0 */
  const uint32_t aci0_caps_size = p->aci0_capability_count * 4;
  const uint32_t aci0_offset = (uint32_t)out->size;
  const uint32_t fs_offset = FIXTURE_NPDM_ACI0_HEADER_SIZE;
  const uint32_t sac_offset = fs_offset + p->fs_access_size;
  const uint32_t caps_offset = sac_offset + p->service_access_size;
  const uint32_t aci0_size = caps_offset + aci0_caps_size;
  uint8_t aci0[FIXTURE_NPDM_ACI0_HEADER_SIZE] = {'A', 'C', 'I', '0'};
  fixture_put_le64(aci0 + 0x10, p->program_id);
  fixture_put_le32(aci0 + 0x20, fs_offset);
  fixture_put_le32(aci0 + 0x24, p->fs_access_size);
  fixture_put_le32(aci0 + 0x28, sac_offset);
  fixture_put_le32(aci0 + 0x2C, p->service_access_size);
  fixture_put_le32(aci0 + 0x30, caps_offset);
  fixture_put_le32(aci0 + 0x34, aci0_caps_size);
  fixture_append(out, aci0, sizeof(aci0));
  fixture_append_zeros(out, p->fs_access_size);
  fixture_append_zeros(out, p->service_access_size);
  for (uint32_t i = 0; i < p->aci0_capability_count; i++) {
    uint8_t raw[4];
    fixture_put_le32(raw, p->aci0_capabilities[i]);
    fixture_append(out, raw, 4);
  }

  fixture_put_le32(out->bytes + FIXTURE_NPDM_OFFSET_ACI0_OFFSET, aci0_offset);
  fixture_put_le32(out->bytes + FIXTURE_NPDM_OFFSET_ACI0_SIZE, aci0_size);
  fixture_put_le32(out->bytes + FIXTURE_NPDM_OFFSET_ACID_OFFSET, acid_offset);
  fixture_put_le32(out->bytes + FIXTURE_NPDM_OFFSET_ACID_SIZE, acid_size);
}

/* ------------------------------------------------------------------ */
/* NCA.                                                                */
/* ------------------------------------------------------------------ */

uint64_t fixture_nca_section_offset(const Fixture_Buffer *nca, uint32_t index) {
  const uint8_t *entry = nca->bytes + FIXTURE_NCA_OFFSET_SECTION_ENTRIES + index * 0x10;
  return (uint64_t)fixture_get_le32(entry) * FIXTURE_NCA_MEDIA_UNIT;
}

void fixture_build_nca(uint32_t magic, uint8_t content_type, uint64_t program_id,
                       const Fixture_NCA_Section sections[4], Fixture_Buffer *out) {
  fixture_buffer_init(out);
  fixture_append_zeros(out, FIXTURE_NCA_HEADER_SIZE);

  uint8_t *header = out->bytes; /* re-fetched after appends below */
  fixture_put_le32(header + FIXTURE_NCA_OFFSET_MAGIC, magic);
  header[FIXTURE_NCA_OFFSET_DISTRIBUTION] = 0;
  header[FIXTURE_NCA_OFFSET_CONTENT_TYPE] = content_type;
  fixture_put_le64(header + FIXTURE_NCA_OFFSET_PROGRAM_ID, program_id);
  fixture_put_le32(header + FIXTURE_NCA_OFFSET_CONTENT_INDEX, 0);
  fixture_put_le32(header + FIXTURE_NCA_OFFSET_SDK_VERSION, 0x000C0000);

  for (uint32_t i = 0; i < 4; i++) {
    const Fixture_NCA_Section *s = &sections[i];
    if (!s->present) continue;
    fixture_align(out, FIXTURE_NCA_MEDIA_UNIT);
    const uint64_t section_start = out->size;
    fixture_append_zeros(out, (size_t)s->hash_layer_bytes);
    fixture_append(out, s->payload->bytes, s->payload->size);
    fixture_align(out, FIXTURE_NCA_MEDIA_UNIT);
    const uint64_t section_end = out->size;

    header = out->bytes;
    uint8_t *entry = header + FIXTURE_NCA_OFFSET_SECTION_ENTRIES + i * 0x10;
    fixture_put_le32(entry + 0, (uint32_t)(section_start / FIXTURE_NCA_MEDIA_UNIT));
    fixture_put_le32(entry + 4, (uint32_t)(section_end / FIXTURE_NCA_MEDIA_UNIT));

    uint8_t *fs = header + FIXTURE_NCA_OFFSET_FS_HEADERS + i * FIXTURE_NCA_FS_HEADER_SIZE;
    fixture_put_le16(fs + FIXTURE_NCA_FS_OFFSET_VERSION, 2);
    fs[FIXTURE_NCA_FS_OFFSET_FS_TYPE] = s->fs_type;
    fs[FIXTURE_NCA_FS_OFFSET_HASH_TYPE] = s->hash_type;
    fs[FIXTURE_NCA_FS_OFFSET_ENCRYPTION_TYPE] = s->encryption_type_raw;
    switch (s->hash_type) {
      case FIXTURE_NCA_HASH_SHA256:
        fixture_put_le32(fs + FIXTURE_NCA_FS_OFFSET_SHA256_LAYER_COUNT, 2);
        fixture_put_le64(fs + FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS + 0x00, 0);
        fixture_put_le64(fs + FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS + 0x08, s->hash_layer_bytes);
        fixture_put_le64(fs + FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS + 0x10, s->hash_layer_bytes);
        fixture_put_le64(fs + FIXTURE_NCA_FS_OFFSET_SHA256_REGIONS + 0x18, s->payload->size);
        break;
      case FIXTURE_NCA_HASH_IVFC: {
        memcpy(fs + FIXTURE_NCA_FS_OFFSET_IVFC_MAGIC, "IVFC", 4);
        fixture_put_le32(fs + FIXTURE_NCA_FS_OFFSET_IVFC_MAGIC + 4, 0x20000);
        fixture_put_le32(fs + FIXTURE_NCA_FS_OFFSET_IVFC_MAX_LAYERS, 7);
        uint8_t *level = fs + FIXTURE_NCA_FS_OFFSET_IVFC_LEVELS + 5 * FIXTURE_NCA_FS_IVFC_LEVEL_SIZE;
        fixture_put_le64(level + 0x00, s->hash_layer_bytes);
        fixture_put_le64(level + 0x08, s->payload->size);
        fixture_put_le32(level + 0x10, 14);
        break;
      }
      default:
        break; /* NONE: whole section is data */
    }
  }

  header = out->bytes;
  fixture_put_le64(header + FIXTURE_NCA_OFFSET_CONTENT_SIZE, out->size);
}
