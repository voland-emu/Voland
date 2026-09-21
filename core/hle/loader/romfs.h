/**
 * RomFS reader: the title's read-only asset filesystem. See docs/DESIGN.md
 * §12 (bootstrap), §13/§15 (fsp-srv serves it in Phase 4), §19 (LayeredFS
 * overlays it in Phase 7).
 *
 * A RomFS image is a header plus four tables followed by file data. The
 * tables are loaded into the caller's arena once (they are a few hundred
 * KB for a large title; cap ROMFS_MAX_TABLE_BYTES guards against a
 * hostile header). File data is never loaded: lookups return an
 * (offset, size) the caller reads through romfs_read_file() or a slice.
 * This is the shape fsp-srv needs: path -> handle -> ranged reads over
 * a multi-GB image that lives in a browser `File` (§15).
 *
 * Layout (offsets from the start of the image):
 *
 *   header, ROMFS_HEADER_SIZE bytes, ten u64 fields:
 *     0x00 header size (== ROMFS_HEADER_SIZE)
 *     0x08 dir hash table offset    0x10 dir hash table size
 *     0x18 dir table offset         0x20 dir table size
 *     0x28 file hash table offset   0x30 file hash table size
 *     0x38 file table offset        0x40 file table size
 *     0x48 file data offset
 *
 *   dir entry (in the dir table; all u32 except the name):
 *     0x00 parent dir       0x04 next sibling dir
 *     0x08 first child dir  0x0C first child file
 *     0x10 next in hash bucket
 *     0x14 name length      0x18 name (not NUL-terminated, padded to 4)
 *
 *   file entry (in the file table):
 *     0x00 parent dir       0x04 next sibling file
 *     0x08 data offset u64  (relative to file data offset)
 *     0x10 data size   u64
 *     0x18 next in hash bucket
 *     0x1C name length      0x20 name (not NUL-terminated, padded to 4)
 *
 *   Every link is a byte offset into its own table; ROMFS_NO_ENTRY marks
 *   "none". The root directory is at dir-table offset 0 with an empty
 *   name. The hash tables are arrays of u32 bucket heads (entry offsets
 *   or ROMFS_NO_ENTRY); bucket = romfs_path_hash(parent, name) %
 *   bucket count. Lookups walk the bucket chain comparing parent and
 *   name; siblings and children support enumeration.
 *
 * Path syntax for romfs_find_*: "/" separated, leading "/" optional,
 * "." and ".." not interpreted, empty components rejected. "" or "/" is
 * the root directory. Names are byte-compared (RomFS names are UTF-8;
 * no case folding). ROMFS_MAX_PATH_BYTES bounds the input.
 *
 * Validation: table ranges must lie inside the source and not overlap
 * the header; every entry decoded is bounds-checked against its table,
 * and file data ranges against the source, at decode time - a corrupt
 * link is RESULT_INVALID_ARGUMENT, never an out-of-bounds read. Chains
 * are walked with a step bound (the table size in entries) so a cyclic
 * link terminates.
 */
#ifndef SWITCH_HLE_LOADER_ROMFS_H
#define SWITCH_HLE_LOADER_ROMFS_H

#include <stdint.h>

#include "common/arena.h"
#include "common/result.h"
#include "hle/loader/byte_source.h"

#define ROMFS_HEADER_SIZE ((uint64_t)0x50)
#define ROMFS_DIR_ENTRY_FIXED_SIZE ((uint32_t)0x18)
#define ROMFS_FILE_ENTRY_FIXED_SIZE ((uint32_t)0x20)
#define ROMFS_NAME_ALIGNMENT ((uint32_t)4)
#define ROMFS_NO_ENTRY ((uint32_t)0xFFFFFFFF)
#define ROMFS_ROOT_DIR_OFFSET ((uint32_t)0)
#define ROMFS_PATH_HASH_SEED ((uint32_t)123456789)

/* Arena budget for the four tables combined. The largest retail titles
 * come in well under 16MB; a header asking for more is treated as corrupt. */
#define ROMFS_MAX_TABLE_BYTES ((uint64_t)64 * 1024 * 1024)
#define ROMFS_MAX_NAME_BYTES ((uint32_t)0x300)   /* Horizon's path limit */
#define ROMFS_MAX_PATH_BYTES ((uint32_t)0x301)

typedef struct RomFS_Dir_Entry {
  uint32_t offset;           /* this entry's offset in the dir table */
  uint32_t parent;           /* dir-table offset */
  uint32_t next_sibling;     /* dir-table offset or ROMFS_NO_ENTRY */
  uint32_t first_child_dir;  /* dir-table offset or ROMFS_NO_ENTRY */
  uint32_t first_child_file; /* file-table offset or ROMFS_NO_ENTRY */
  const char *name;          /* into the arena table copy; NOT NUL-terminated */
  uint32_t name_length;
} RomFS_Dir_Entry;

typedef struct RomFS_File_Entry {
  uint32_t offset;           /* this entry's offset in the file table */
  uint32_t parent;           /* dir-table offset */
  uint32_t next_sibling;     /* file-table offset or ROMFS_NO_ENTRY */
  uint64_t data_offset;      /* ABSOLUTE within the source (data base added) */
  uint64_t data_size;
  const char *name;          /* into the arena table copy; NOT NUL-terminated */
  uint32_t name_length;
} RomFS_File_Entry;

typedef struct RomFS {
  const Byte_Source *source; /* caller's; must outlive this struct */
  uint64_t file_data_offset;
  const uint8_t *dir_table;  uint32_t dir_table_size;
  const uint8_t *file_table; uint32_t file_table_size;
  const uint32_t *dir_hash_table;  uint32_t dir_bucket_count;
  const uint32_t *file_hash_table; uint32_t file_bucket_count;
} RomFS;

/* ------------------------------------------------------------------ */
/* Open.                                                               */
/* ------------------------------------------------------------------ */

/* Parses the header at offset 0 and loads the four tables into `arena`.
 *   RESULT_INVALID_ARGUMENT NULL args; header size field wrong (callers
 *                           wanting the §1.6 diagnosis run
 *                           nca_probe_section first); any table outside
 *                           the source, overlapping the header, over
 *                           ROMFS_MAX_TABLE_BYTES combined, or a hash
 *                           table whose size is not a multiple of 4;
 *                           root dir entry undecodable
 *   RESULT_OUT_OF_MEMORY    arena exhausted
 *   RESULT_IO_ERROR         propagated
 * The image is otherwise validated lazily, per entry, as it is walked. */
Error romfs_open(const Byte_Source *source, Arena *arena, RomFS *out);

/* ------------------------------------------------------------------ */
/* Lookup.                                                             */
/* ------------------------------------------------------------------ */

/* Resolve a path to a directory / file entry via the hash tables.
 *   RESULT_NOT_FOUND        a component does not exist, or is the wrong
 *                           kind (file where a dir was needed and v.v.)
 *   RESULT_INVALID_ARGUMENT malformed path (empty component, too long)
 *                           or a corrupt table entry met on the way */
Error romfs_find_dir(const RomFS *fs, const char *path, RomFS_Dir_Entry *out);
Error romfs_find_file(const RomFS *fs, const char *path, RomFS_File_Entry *out);

/* Decode the entry at a table offset (for enumeration via the sibling /
 * child links). RESULT_INVALID_ARGUMENT if the offset is ROMFS_NO_ENTRY,
 * misaligned, or the entry (including its name) runs past its table, or
 * a file entry's data range runs past the source. */
Error romfs_dir_entry(const RomFS *fs, uint32_t offset, RomFS_Dir_Entry *out);
Error romfs_file_entry(const RomFS *fs, uint32_t offset, RomFS_File_Entry *out);

/* The bucket hash the format uses; exposed so LayeredFS (§19) can build
 * overlay indexes with identical keys. Hashes `name_length` bytes. */
uint32_t romfs_path_hash(uint32_t parent_dir_offset, const char *name,
                         uint32_t name_length);

/* ------------------------------------------------------------------ */
/* File data.                                                          */
/* ------------------------------------------------------------------ */

/* Reads [offset, offset + size) of the file's content. Range-checked
 * against entry->data_size (RESULT_INVALID_ARGUMENT); size == 0 no-op. */
Error romfs_read_file(const RomFS *fs, const RomFS_File_Entry *entry,
                      uint64_t offset, void *out, uint64_t size);

/* A bounded source over the file's content. `out` is caller-owned. */
Error romfs_file_source(const RomFS *fs, const RomFS_File_Entry *entry,
                        Byte_Source_Slice *out);

#endif /* SWITCH_HLE_LOADER_ROMFS_H */
