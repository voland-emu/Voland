/**
 * NSO reader. See nso.h for the layout.
 */
#include "hle/loader/nso.h"

#include "common/log.h"
#include "third_party/lz4/lz4.h"

#include <string.h>

/* Header field offsets (bytes from the start of the file). */
#define NSO_OFFSET_VERSION ((uint64_t)0x04)
#define NSO_OFFSET_FLAGS ((uint64_t)0x0C)
#define NSO_OFFSET_TEXT_SEGMENT ((uint64_t)0x10)
#define NSO_OFFSET_MODULE_NAME_OFFSET ((uint64_t)0x1C)
#define NSO_OFFSET_RODATA_SEGMENT ((uint64_t)0x20)
#define NSO_OFFSET_MODULE_NAME_SIZE ((uint64_t)0x2C)
#define NSO_OFFSET_DATA_SEGMENT ((uint64_t)0x30)
#define NSO_OFFSET_BSS_SIZE ((uint64_t)0x3C)
#define NSO_OFFSET_MODULE_ID ((uint64_t)0x40)
#define NSO_OFFSET_TEXT_FILE_SIZE ((uint64_t)0x60)
#define NSO_OFFSET_RODATA_FILE_SIZE ((uint64_t)0x64)
#define NSO_OFFSET_DATA_FILE_SIZE ((uint64_t)0x68)
#define NSO_OFFSET_API_INFO ((uint64_t)0x88)
#define NSO_OFFSET_DYNSTR ((uint64_t)0x90)
#define NSO_OFFSET_DYNSYM ((uint64_t)0x98)

/* Segment header: file offset, memory offset, size - 4 bytes each. */
#define NSO_SEGMENT_HEADER_OFFSET_FILE_OFFSET ((uint64_t)0x0)
#define NSO_SEGMENT_HEADER_OFFSET_MEMORY_OFFSET ((uint64_t)0x4)
#define NSO_SEGMENT_HEADER_OFFSET_SIZE ((uint64_t)0x8)

/* Rodata sub-section header: offset, size - 4 bytes each. */
#define NSO_RODATA_SECTION_OFFSET_SIZE ((uint64_t)0x4)

/* Per-segment header positions and flag bits, indexed by NSO_Segment_Kind. */
static const uint64_t k_segment_header_offsets[NSO_SEGMENT_COUNT] = {
    NSO_OFFSET_TEXT_SEGMENT, NSO_OFFSET_RODATA_SEGMENT, NSO_OFFSET_DATA_SEGMENT};
static const uint64_t k_segment_file_size_offsets[NSO_SEGMENT_COUNT] = {
    NSO_OFFSET_TEXT_FILE_SIZE, NSO_OFFSET_RODATA_FILE_SIZE, NSO_OFFSET_DATA_FILE_SIZE};
static const uint32_t k_segment_compressed_flags[NSO_SEGMENT_COUNT] = {
    NSO_FLAG_TEXT_COMPRESSED, NSO_FLAG_RODATA_COMPRESSED, NSO_FLAG_DATA_COMPRESSED};
static const uint32_t k_segment_hash_flags[NSO_SEGMENT_COUNT] = {
    NSO_FLAG_TEXT_HASH, NSO_FLAG_RODATA_HASH, NSO_FLAG_DATA_HASH};
static const char *const k_segment_names[NSO_SEGMENT_COUNT] = {".text", ".rodata", ".data"};

static uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static void read_rodata_section(const uint8_t *header, uint64_t at,
                                NSO_Rodata_Section *out) {
  out->offset = byte_source_le32(header + at);
  out->size = byte_source_le32(header + at + NSO_RODATA_SECTION_OFFSET_SIZE);
}

/* A sub-section is valid when absent (size 0) or entirely inside .rodata. */
static bool rodata_section_in_bounds(const NSO_Rodata_Section *section,
                                     uint32_t rodata_size) {
  if (section->size == 0) return true;
  return section->offset <= rodata_size && section->size <= rodata_size - section->offset;
}

Error nso_open(const Byte_Source *source, NSO *out) {
  if (!source || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_open: NULL argument");
  }
  memset(out, 0, sizeof(*out));

  if (source->size < NSO_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso: source shorter than header");
  }
  uint8_t header[NSO_HEADER_SIZE];
  Error err = byte_source_read(source, 0, header, NSO_HEADER_SIZE);
  if (!error_is_ok(err)) return err;

  if (byte_source_le32(header) != NSO_MAGIC) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso: NSO0 magic missing");
  }
  const uint32_t version = byte_source_le32(header + NSO_OFFSET_VERSION);
  if (version != NSO_VERSION) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso: unsupported version");
  }
  const uint32_t flags = byte_source_le32(header + NSO_OFFSET_FLAGS);
  if (flags & NSO_FLAG_ZSTD_COMPRESSED) {
    return ERR(RESULT_NOT_IMPLEMENTED, "nso: zstd-compressed (22.0.0+) segments not supported");
  }

  /* Segments: decode, then bounds-check each against the file and the
   * caps, then check their mutual memory layout. */
  for (uint32_t i = 0; i < NSO_SEGMENT_COUNT; i++) {
    const uint8_t *raw = header + k_segment_header_offsets[i];
    NSO_Segment *segment = &out->segments[i];
    segment->file_offset = byte_source_le32(raw + NSO_SEGMENT_HEADER_OFFSET_FILE_OFFSET);
    segment->memory_offset = byte_source_le32(raw + NSO_SEGMENT_HEADER_OFFSET_MEMORY_OFFSET);
    segment->memory_size = byte_source_le32(raw + NSO_SEGMENT_HEADER_OFFSET_SIZE);
    segment->file_size = byte_source_le32(header + k_segment_file_size_offsets[i]);
    segment->is_compressed = (flags & k_segment_compressed_flags[i]) != 0;
    segment->has_hash = (flags & k_segment_hash_flags[i]) != 0;

    if (segment->memory_size > NSO_MAX_SEGMENT_BYTES) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: segment size over cap");
    }
    if (segment->file_offset > source->size ||
        segment->file_size > source->size - segment->file_offset) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: segment file range outside source");
    }
    if (!segment->is_compressed && segment->file_size != segment->memory_size) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: raw segment file size differs from memory size");
    }
    if (segment->memory_offset % NSO_SEGMENT_ALIGNMENT != 0) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: segment memory offset not page-aligned");
    }
  }
  for (uint32_t i = 1; i < NSO_SEGMENT_COUNT; i++) {
    const NSO_Segment *previous = &out->segments[i - 1];
    const NSO_Segment *current = &out->segments[i];
    const uint64_t previous_end = (uint64_t)previous->memory_offset + previous->memory_size;
    if (current->memory_offset < previous_end) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: segments overlap or are out of order in memory");
    }
  }

  out->version = version;
  out->flags = flags;
  out->bss_size = byte_source_le32(header + NSO_OFFSET_BSS_SIZE);
  out->execute_only_text = (flags & NSO_FLAG_EXECUTE_ONLY_TEXT) != 0;
  const NSO_Segment *data = &out->segments[NSO_SEGMENT_DATA];
  out->image_size = align_up((uint64_t)data->memory_offset + data->memory_size + out->bss_size,
                             NSO_SEGMENT_ALIGNMENT);

  memcpy(out->module_id, header + NSO_OFFSET_MODULE_ID, NSO_MODULE_ID_SIZE);

  const uint32_t rodata_size = out->segments[NSO_SEGMENT_RODATA].memory_size;
  read_rodata_section(header, NSO_OFFSET_API_INFO, &out->api_info);
  read_rodata_section(header, NSO_OFFSET_DYNSTR, &out->dynstr);
  read_rodata_section(header, NSO_OFFSET_DYNSYM, &out->dynsym);
  if (!rodata_section_in_bounds(&out->api_info, rodata_size) ||
      !rodata_section_in_bounds(&out->dynstr, rodata_size) ||
      !rodata_section_in_bounds(&out->dynsym, rodata_size)) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso: rodata sub-section outside .rodata");
  }

  /* Module name: a file range. Retail files carry a single NUL. */
  const uint32_t name_offset = byte_source_le32(header + NSO_OFFSET_MODULE_NAME_OFFSET);
  const uint32_t name_size = byte_source_le32(header + NSO_OFFSET_MODULE_NAME_SIZE);
  if (name_size > 0) {
    if (name_offset > source->size || name_size > source->size - name_offset) {
      return ERR(RESULT_INVALID_ARGUMENT, "nso: module name outside source");
    }
    const uint32_t copy_size =
        name_size > NSO_MAX_MODULE_NAME_BYTES ? NSO_MAX_MODULE_NAME_BYTES : name_size;
    err = byte_source_read(source, name_offset, out->module_name, copy_size);
    if (!error_is_ok(err)) return err;
    out->module_name[copy_size] = '\0';
  }

  out->source = source;
  log_debug("nso: opened '%s', image %llu bytes, flags 0x%x", out->module_name,
            (unsigned long long)out->image_size, (unsigned)flags);
  return OK;
}

Error nso_read_segment(const NSO *nso, NSO_Segment_Kind kind, Arena *scratch,
                       uint8_t *out, uint64_t out_size) {
  if (!nso || !nso->source || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: NULL argument");
  }
  if ((uint32_t)kind >= NSO_SEGMENT_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: segment kind out of range");
  }
  const NSO_Segment *segment = &nso->segments[kind];
  if (out_size < segment->memory_size) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: output smaller than segment");
  }

  if (!segment->is_compressed) {
    return byte_source_read(nso->source, segment->file_offset, out, segment->memory_size);
  }

  if (!scratch) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: compressed segment needs a scratch arena");
  }
  /* LZ4_decompress_safe takes int sizes; NSO_MAX_SEGMENT_BYTES and the
   * source bound keep both well inside range, but state it. */
  if (segment->file_size > (uint32_t)LZ4_MAX_INPUT_SIZE ||
      segment->memory_size > (uint32_t)LZ4_MAX_INPUT_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: segment exceeds LZ4 limits");
  }
  uint8_t *staging = ARENA_ALLOC_ARRAY(scratch, uint8_t, segment->file_size == 0 ? 1 : segment->file_size);
  if (!staging) {
    return ERR(RESULT_OUT_OF_MEMORY, "nso_read_segment: scratch arena exhausted");
  }
  Error err = byte_source_read(nso->source, segment->file_offset, staging, segment->file_size);
  if (!error_is_ok(err)) return err;

  const int decoded = LZ4_decompress_safe((const char *)staging, (char *)out,
                                          (int)segment->file_size, (int)segment->memory_size);
  if (decoded < 0) {
    log_warn("nso: %s LZ4 stream malformed (lz4 code %d)", k_segment_names[kind], decoded);
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: malformed LZ4 stream");
  }
  if ((uint32_t)decoded != segment->memory_size) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_read_segment: decoded length differs from segment size");
  }
  return OK;
}

Error nso_module_id_hex(const NSO *nso, char *out) {
  if (!nso || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "nso_module_id_hex: NULL argument");
  }
  static const char k_digits[] = "0123456789abcdef";
  for (uint32_t i = 0; i < NSO_MODULE_ID_SIZE; i++) {
    out[i * 2] = k_digits[nso->module_id[i] >> 4];
    out[i * 2 + 1] = k_digits[nso->module_id[i] & 0xF];
  }
  out[NSO_MODULE_ID_SIZE * 2] = '\0';
  return OK;
}
