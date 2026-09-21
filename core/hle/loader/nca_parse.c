/**
 * Decrypted-NCA container parser. See nca_parse.h for the layout and the
 * §1.6 posture: this file locates plaintext structures and never touches
 * the key area or interprets a cipher mode.
 */
#include "hle/loader/nca_parse.h"

#include "common/log.h"
#include "hle/loader/exefs.h"
#include "hle/loader/romfs.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Header field offsets (bytes from the start of the NCA).             */
/* ------------------------------------------------------------------ */

#define NCA_OFFSET_DISTRIBUTION_TYPE ((uint64_t)0x204)
#define NCA_OFFSET_CONTENT_TYPE ((uint64_t)0x205)
#define NCA_OFFSET_CONTENT_SIZE ((uint64_t)0x208)
#define NCA_OFFSET_PROGRAM_ID ((uint64_t)0x210)
#define NCA_OFFSET_CONTENT_INDEX ((uint64_t)0x218)
#define NCA_OFFSET_SDK_ADDON_VERSION ((uint64_t)0x21C)
#define NCA_OFFSET_SECTION_ENTRIES ((uint64_t)0x240)
#define NCA_SECTION_ENTRY_SIZE ((uint64_t)0x10)
#define NCA_OFFSET_FS_HEADERS ((uint64_t)0x400)

/* FsHeader field offsets (bytes from the start of the FsHeader). */
#define NCA_FS_OFFSET_VERSION ((uint64_t)0x00)
#define NCA_FS_OFFSET_FS_TYPE ((uint64_t)0x02)
#define NCA_FS_OFFSET_HASH_TYPE ((uint64_t)0x03)
#define NCA_FS_OFFSET_ENCRYPTION_TYPE ((uint64_t)0x04)
#define NCA_FS_OFFSET_HASH_DATA ((uint64_t)0x08)
#define NCA_FS_OFFSET_PATCH_INFO ((uint64_t)0x100)
#define NCA_FS_PATCH_INFO_SIZE ((uint64_t)0x40)
#define NCA_FS_OFFSET_SPARSE_INFO ((uint64_t)0x148)
#define NCA_FS_SPARSE_INFO_SIZE ((uint64_t)0x30)
#define NCA_FS_OFFSET_COMPRESSION_INFO ((uint64_t)0x178)
#define NCA_FS_COMPRESSION_INFO_SIZE ((uint64_t)0x28)

/* HierarchicalSha256 hash data (from the FsHeader start). */
#define NCA_SHA256_OFFSET_LAYER_COUNT ((uint64_t)0x2C)
#define NCA_SHA256_OFFSET_LAYER_REGIONS ((uint64_t)0x30)
#define NCA_SHA256_LAYER_REGION_SIZE ((uint64_t)0x10)

/* HierarchicalIntegrity (IVFC) hash data (from the FsHeader start). */
#define NCA_IVFC_OFFSET_MAGIC ((uint64_t)0x08)
#define NCA_IVFC_OFFSET_MAX_LAYERS ((uint64_t)0x14)
#define NCA_IVFC_OFFSET_LEVELS ((uint64_t)0x18)
#define NCA_IVFC_LEVEL_SIZE ((uint64_t)0x18)
/* The data level sits below the hash levels: levels[max_layers - 2]. */
#define NCA_IVFC_DATA_LEVEL_FROM_MAX_LAYERS 2u

/* Longest structure magic nca_probe_section needs to see. */
#define NCA_PROBE_BYTES ((uint64_t)16)

#define NCA_ENCRYPTED_MESSAGE                                                   \
  "NCA header magic missing: the file is encrypted or not an NCA. Voland "     \
  "reads pre-decrypted NCA only; decrypt it with separate tools first "        \
  "(docs/DUMP.md)"

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */

static bool bytes_all_zero(const uint8_t *bytes, uint64_t size) {
  for (uint64_t i = 0; i < size; i++) {
    if (bytes[i] != 0) return false;
  }
  return true;
}

static bool content_type_is_valid(uint8_t value) {
  return value <= (uint8_t)NCA_CONTENT_PUBLIC_DATA;
}

static bool fs_type_is_valid(uint8_t value) {
  return value <= (uint8_t)NCA_FS_PARTITION_FS;
}

/* Locates the filesystem payload inside the section from the hash-layer
 * info. `data_offset`/`data_size` come back relative to the section. */
static Error locate_section_data(const uint8_t *fs_header, NCA_Hash_Type hash_type,
                                 uint64_t section_size, uint64_t *data_offset,
                                 uint64_t *data_size) {
  switch (hash_type) {
    case NCA_HASH_NONE:
      *data_offset = 0;
      *data_size = section_size;
      return OK;

    case NCA_HASH_HIERARCHICAL_SHA256:
    case NCA_HASH_HIERARCHICAL_SHA3_256: {
      const uint32_t layer_count = byte_source_le32(fs_header + NCA_SHA256_OFFSET_LAYER_COUNT);
      if (layer_count == 0 || layer_count > NCA_SHA256_MAX_LAYERS) {
        return ERR(RESULT_INVALID_ARGUMENT, "nca: SHA256 layer count out of range");
      }
      const uint8_t *region = fs_header + NCA_SHA256_OFFSET_LAYER_REGIONS +
                              (uint64_t)(layer_count - 1) * NCA_SHA256_LAYER_REGION_SIZE;
      *data_offset = byte_source_le64(region);
      *data_size = byte_source_le64(region + 8);
      return OK;
    }

    case NCA_HASH_HIERARCHICAL_INTEGRITY:
    case NCA_HASH_HIERARCHICAL_INTEGRITY_SHA3: {
      if (byte_source_le32(fs_header + NCA_IVFC_OFFSET_MAGIC) != NCA_IVFC_MAGIC) {
        return ERR(RESULT_INVALID_ARGUMENT, "nca: IVFC magic missing");
      }
      const uint32_t max_layers = byte_source_le32(fs_header + NCA_IVFC_OFFSET_MAX_LAYERS);
      if (max_layers < NCA_IVFC_DATA_LEVEL_FROM_MAX_LAYERS ||
          max_layers - NCA_IVFC_DATA_LEVEL_FROM_MAX_LAYERS >= NCA_IVFC_MAX_LEVELS) {
        return ERR(RESULT_INVALID_ARGUMENT, "nca: IVFC layer count out of range");
      }
      const uint32_t data_level = max_layers - NCA_IVFC_DATA_LEVEL_FROM_MAX_LAYERS;
      const uint8_t *level = fs_header + NCA_IVFC_OFFSET_LEVELS +
                             (uint64_t)data_level * NCA_IVFC_LEVEL_SIZE;
      *data_offset = byte_source_le64(level);
      *data_size = byte_source_le64(level + 8);
      return OK;
    }

    case NCA_HASH_AUTO:
    case NCA_HASH_AUTO_SHA3:
      return ERR(RESULT_INVALID_ARGUMENT, "nca: unresolved (auto) hash type");
  }
  return ERR(RESULT_INVALID_ARGUMENT, "nca: hash type out of range");
}

static Error parse_section(const uint8_t *header_bytes, uint32_t index,
                           NCA_Section_Info *out) {
  memset(out, 0, sizeof(*out));

  const uint8_t *entry = header_bytes + NCA_OFFSET_SECTION_ENTRIES +
                         (uint64_t)index * NCA_SECTION_ENTRY_SIZE;
  const uint64_t start_units = byte_source_le32(entry);
  const uint64_t end_units = byte_source_le32(entry + 4);
  if (start_units == 0 && end_units == 0) return OK; /* absent */
  if (end_units < start_units) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: section end precedes start");
  }

  const uint8_t *fs_header = header_bytes + NCA_OFFSET_FS_HEADERS +
                             (uint64_t)index * NCA_FS_HEADER_SIZE;
  if (byte_source_le16(fs_header + NCA_FS_OFFSET_VERSION) != NCA_FS_HEADER_VERSION) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: unsupported FsHeader version");
  }
  const uint8_t fs_type = fs_header[NCA_FS_OFFSET_FS_TYPE];
  const uint8_t hash_type = fs_header[NCA_FS_OFFSET_HASH_TYPE];
  if (!fs_type_is_valid(fs_type)) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: unknown section fs type");
  }
  if (hash_type > (uint8_t)NCA_HASH_HIERARCHICAL_INTEGRITY_SHA3) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: hash type out of range");
  }

  out->present = true;
  out->offset = start_units * NCA_MEDIA_UNIT_SIZE;
  out->size = (end_units - start_units) * NCA_MEDIA_UNIT_SIZE;
  out->fs_type = (NCA_Fs_Type)fs_type;
  out->hash_type = (NCA_Hash_Type)hash_type;
  out->encryption_type_raw = fs_header[NCA_FS_OFFSET_ENCRYPTION_TYPE];
  out->has_patch_info =
      !bytes_all_zero(fs_header + NCA_FS_OFFSET_PATCH_INFO, NCA_FS_PATCH_INFO_SIZE);
  out->has_sparse_info =
      !bytes_all_zero(fs_header + NCA_FS_OFFSET_SPARSE_INFO, NCA_FS_SPARSE_INFO_SIZE);
  out->has_compression_info = !bytes_all_zero(
      fs_header + NCA_FS_OFFSET_COMPRESSION_INFO, NCA_FS_COMPRESSION_INFO_SIZE);

  uint64_t relative_offset = 0;
  uint64_t data_size = 0;
  Error err = locate_section_data(fs_header, out->hash_type, out->size,
                                  &relative_offset, &data_size);
  if (!error_is_ok(err)) return err;
  if (relative_offset > out->size || data_size > out->size - relative_offset) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: section data region outside section");
  }
  out->data_offset = out->offset + relative_offset;
  out->data_size = data_size;
  return OK;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

Error nca_parse_header(const uint8_t *header_bytes, NCA_Header_Info *out) {
  if (!header_bytes || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca_parse_header: NULL argument");
  }
  memset(out, 0, sizeof(*out));

  const uint32_t magic = byte_source_le32(header_bytes + NCA_MAGIC_OFFSET);
  if (magic == NCA_MAGIC_NCA0) {
    return ERR(RESULT_NOT_IMPLEMENTED, "nca: NCA0 (pre-release layout) is not supported");
  }
  if (magic != NCA_MAGIC_NCA3 && magic != NCA_MAGIC_NCA2) {
    return ERR(RESULT_ENCRYPTED_INPUT, NCA_ENCRYPTED_MESSAGE);
  }
  out->magic = magic;

  const uint8_t distribution = header_bytes[NCA_OFFSET_DISTRIBUTION_TYPE];
  const uint8_t content_type = header_bytes[NCA_OFFSET_CONTENT_TYPE];
  if (distribution > (uint8_t)NCA_DISTRIBUTION_GAMECARD) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: unknown distribution type");
  }
  if (!content_type_is_valid(content_type)) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: unknown content type");
  }
  out->distribution_type = (NCA_Distribution_Type)distribution;
  out->content_type = (NCA_Content_Type)content_type;
  out->content_size = byte_source_le64(header_bytes + NCA_OFFSET_CONTENT_SIZE);
  out->program_id = byte_source_le64(header_bytes + NCA_OFFSET_PROGRAM_ID);
  out->content_index = byte_source_le32(header_bytes + NCA_OFFSET_CONTENT_INDEX);
  out->sdk_addon_version = byte_source_le32(header_bytes + NCA_OFFSET_SDK_ADDON_VERSION);

  for (uint32_t i = 0; i < NCA_SECTION_COUNT; i++) {
    Error err = parse_section(header_bytes, i, &out->sections[i]);
    if (!error_is_ok(err)) return err;
  }
  return OK;
}

Error nca_open(const Byte_Source *source, NCA_File *out) {
  if (!source || !out) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca_open: NULL argument");
  }
  memset(out, 0, sizeof(*out));
  if (source->size < NCA_HEADER_SIZE) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: file is smaller than an NCA header");
  }

  uint8_t header_bytes[NCA_HEADER_SIZE];
  Error err = byte_source_read(source, 0, header_bytes, NCA_HEADER_SIZE);
  if (!error_is_ok(err)) return err;
  err = nca_parse_header(header_bytes, &out->header);
  if (!error_is_ok(err)) return err;

  if (out->header.content_size > source->size) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca: content size exceeds file size");
  }
  out->source = source;

  for (uint32_t i = 0; i < NCA_SECTION_COUNT; i++) {
    const NCA_Section_Info *section = &out->header.sections[i];
    if (!section->present) continue;
    if (section->offset > out->header.content_size ||
        section->size > out->header.content_size - section->offset) {
      return ERR(RESULT_INVALID_ARGUMENT, "nca: section extends past content size");
    }
    err = byte_source_slice(source, section->data_offset, section->data_size,
                            &out->section_sources[i]);
    if (!error_is_ok(err)) return err;
  }
  log_debug("nca: opened type=%d program_id=%016llx sections=%d%d%d%d",
            (int)out->header.content_type,
            (unsigned long long)out->header.program_id,
            (int)out->header.sections[0].present, (int)out->header.sections[1].present,
            (int)out->header.sections[2].present, (int)out->header.sections[3].present);
  return OK;
}

static bool section_is_readable(const NCA_Section_Info *section) {
  return section->present && !section->has_patch_info && !section->has_sparse_info &&
         !section->has_compression_info;
}

const Byte_Source *nca_section_source(const NCA_File *nca, uint32_t index) {
  if (!nca || index >= NCA_SECTION_COUNT) return NULL;
  if (!section_is_readable(&nca->header.sections[index])) return NULL;
  return &nca->section_sources[index].source;
}

Error nca_probe_section(const NCA_File *nca, uint32_t index) {
  if (!nca || index >= NCA_SECTION_COUNT) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca_probe_section: bad index");
  }
  const NCA_Section_Info *section = &nca->header.sections[index];
  if (!section->present) {
    return ERR(RESULT_INVALID_ARGUMENT, "nca_probe_section: section absent");
  }
  if (!section_is_readable(section)) {
    return ERR(RESULT_NOT_IMPLEMENTED,
               "nca: patch/sparse/compressed section layouts are not supported yet");
  }

  uint8_t probe[NCA_PROBE_BYTES];
  memset(probe, 0, sizeof(probe));
  const uint64_t probe_size =
      section->data_size < NCA_PROBE_BYTES ? section->data_size : NCA_PROBE_BYTES;
  Error err = byte_source_read(&nca->section_sources[index].source, 0, probe, probe_size);
  if (!error_is_ok(err)) return err;

  switch (section->fs_type) {
    case NCA_FS_PARTITION_FS:
      if (probe_size < sizeof(uint32_t) || byte_source_le32(probe) != EXEFS_MAGIC) {
        return ERR(RESULT_ENCRYPTED_INPUT,
                   "NCA partition (ExeFS) section has no PFS0 header: the NCA header is "
                   "plaintext but its sections are still encrypted. Re-decrypt with "
                   "separate tools (docs/DUMP.md)");
      }
      return OK;
    case NCA_FS_ROMFS:
      if (probe_size < sizeof(uint64_t) || byte_source_le64(probe) != ROMFS_HEADER_SIZE) {
        return ERR(RESULT_ENCRYPTED_INPUT,
                   "NCA RomFS section has no RomFS header: the NCA header is plaintext "
                   "but its sections are still encrypted. Re-decrypt with separate "
                   "tools (docs/DUMP.md)");
      }
      return OK;
  }
  return ERR(RESULT_INVALID_ARGUMENT, "nca_probe_section: unknown fs type");
}

int nca_find_section(const NCA_File *nca, NCA_Fs_Type fs_type) {
  if (!nca) return -1;
  for (uint32_t i = 0; i < NCA_SECTION_COUNT; i++) {
    const NCA_Section_Info *section = &nca->header.sections[i];
    if (section->present && section->fs_type == fs_type) return (int)i;
  }
  return -1;
}
