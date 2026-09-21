/**
 * ExeFS reader: a PFS0 partition holding a title's executables. See
 * docs/DESIGN.md §12 ("Process bootstrap").
 *
 * The ExeFS is the PARTITION_FS section of a PROGRAM NCA. Its files are
 * `main.npdm` (npdm.h), and the NSOs `rtld`, `main`, `subsdk0`..`subsdk9`,
 * `sdk` (nso.h, next header). PFS0 is a flat, unordered directory; this
 * reader parses the directory into an arena and hands out bounded sources
 * for file contents - it never reads file data itself except through
 * exefs_read().
 *
 * PFS0 layout (offsets from the start of the partition):
 *
 *   0x00  magic               u32   "PFS0"
 *   0x04  file count          u32   <= EXEFS_MAX_FILE_COUNT
 *   0x08  string table size   u32   <= EXEFS_MAX_STRING_TABLE_BYTES
 *   0x0C  reserved            u32
 *   0x10  entries             file count x 0x18:
 *           0x00 data offset  u64   relative to the data region
 *           0x08 data size    u64
 *           0x10 name offset  u32   into the string table
 *           0x14 reserved     u32
 *         string table        NUL-terminated names, packed
 *         data region         starts at 0x10 + entries + string table
 *
 * Validation (the input is untrusted for memory safety, §19): every
 * entry's [data offset, data offset + size) must lie inside the source;
 * every name offset must point inside the string table and the name must
 * be NUL-terminated within it. Duplicate names are not rejected (PFS0
 * does not forbid them); exefs_find returns the first.
 *
 * Memory: the entry table and string table are copied into the caller's
 * arena (at most EXEFS_MAX_FILE_COUNT * sizeof(ExeFS_Entry) +
 * EXEFS_MAX_STRING_TABLE_BYTES). The source must outlive the ExeFS.
 */
#ifndef SWITCH_HLE_LOADER_EXEFS_H
#define SWITCH_HLE_LOADER_EXEFS_H

#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"

#define EXEFS_MAGIC ((uint32_t)0x30534650) /* "PFS0" little-endian */
#define EXEFS_HEADER_SIZE ((uint64_t)0x10)
#define EXEFS_ENTRY_SIZE ((uint64_t)0x18)

/* A real ExeFS holds 13 files at most (npdm + rtld + main + 10 subsdk +
 * sdk); the cap leaves headroom without letting a hostile count drive
 * the arena allocation. */
#define EXEFS_MAX_FILE_COUNT 64u
#define EXEFS_MAX_STRING_TABLE_BYTES ((uint64_t)0x10000)

/* Well-known ExeFS file names (§12). */
#define EXEFS_FILE_NPDM "main.npdm"
#define EXEFS_FILE_RTLD "rtld"
#define EXEFS_FILE_MAIN "main"
#define EXEFS_FILE_SDK "sdk"
#define EXEFS_FILE_SUBSDK_PREFIX "subsdk" /* followed by a single digit 0-9 */
#define EXEFS_SUBSDK_COUNT 10u

typedef struct ExeFS_Entry {
  const char *name;       /* NUL-terminated, lives in the arena copy */
  uint64_t offset;        /* ABSOLUTE within the source (data region added) */
  uint64_t size;
} ExeFS_Entry;

typedef struct ExeFS {
  const Byte_Source *source; /* caller's; must outlive this struct */
  uint32_t file_count;
  ExeFS_Entry *entries;      /* file_count entries, in on-media order */
  uint64_t data_region_offset;
} ExeFS;

/* Parses the PFS0 directory at offset 0 of `source`.
 *   RESULT_INVALID_ARGUMENT NULL args; magic missing (callers that want
 *                           the §1.6 diagnosis run nca_probe_section
 *                           first); file count or string table over the
 *                           caps; source too small for the directory;
 *                           an entry's data range or name is out of
 *                           bounds
 *   RESULT_OUT_OF_MEMORY    arena exhausted
 *   RESULT_IO_ERROR         propagated
 * On error `out` is unspecified and the arena may have been advanced. */
Error exefs_open(const Byte_Source *source, Arena *arena, ExeFS *out);

/* First entry whose name equals `name` exactly (case-sensitive), or NULL. */
const ExeFS_Entry *exefs_find(const ExeFS *fs, const char *name);

/* Reads [offset, offset + size) of the entry's content. Range-checked
 * against entry->size (RESULT_INVALID_ARGUMENT); size == 0 is a no-op. */
Error exefs_read(const ExeFS *fs, const ExeFS_Entry *entry, uint64_t offset,
                 void *out, uint64_t size);

/* A bounded source over the entry's content, for handing an NSO to
 * nso.h without copying. `out` is caller-owned storage. */
Error exefs_entry_source(const ExeFS *fs, const ExeFS_Entry *entry,
                         Byte_Source_Slice *out);

#endif /* SWITCH_HLE_LOADER_EXEFS_H */
