/**
 * HIPC + CMIF framing, service-object dispatch, and the Phase 1 session
 * table. See ipc.h for scope and the flagged simplifications.
 *
 * Wire layout (word0/word1 bit packing, descriptor ordering) is per
 * switchbrew's "HIPC" page, cross-checked while writing this file. The
 * CMIF inner header (SFCI/SFCO magic, version, 64-bit command_id/result)
 * and the response message's own `type` field (written here as 0 - real
 * captures show clients do not gate on it, but this is the one wire
 * detail NOT independently re-verified in this pass) are flagged for a
 * libnx/Ryujinx/yuzu cross-reference test before this is trusted against
 * a real homebrew NRO (Phase 2's proof of life) - see svc_ipc_test.c's
 * test plan.
 */
#include "hle/kernel/ipc.h"

#include "hle/hle.h" /* HLE_RESULT_* / HLE_MAKE_RESULT (hle.h is not
                      * ipc.h's dependency - only ipc.c's, for these
                      * value-level constants; see ipc.h's header comment) */

#include <string.h>

/* ------------------------------------------------------------------ */
/* Wire-format constants. Not in ipc.h: callers never need them, only   */
/* parse/write do.                                                      */
/* ------------------------------------------------------------------ */

#define IPC_RAW_DATA_ALIGN 16u
#define IPC_CMIF_HEADER_BYTES 16u /* magic(4) + version(4) + command_id/result(8) */
#define IPC_CMIF_MAGIC_SFCI 0x49434653u /* "SFCI" read little-endian */
#define IPC_CMIF_MAGIC_SFCO 0x4F434653u /* "SFCO" read little-endian */

static uint32_t read_u32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static uint64_t read_u64(const uint8_t *p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static void write_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static void write_u64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }

static uint32_t align_up(uint32_t value, uint32_t align) {
  return (value + align - 1u) & ~(align - 1u);
}

/* ------------------------------------------------------------------ */
/* IPC_Response.                                                        */
/* ------------------------------------------------------------------ */

bool ipc_response_add_move_object(IPC_Response *res, Service_Object object) {
  if (!res || res->out_object_count >= IPC_MAX_OUT_OBJECTS) return false;
  res->out_objects[res->out_object_count++] = object;
  return true;
}

/* ------------------------------------------------------------------ */
/* Session table.                                                       */
/* ------------------------------------------------------------------ */

void ipc_session_table_init(IPC_Session_Table *table) {
  if (!table) return;
  memset(table, 0, sizeof(*table));
}

uint32_t ipc_session_table_open(IPC_Session_Table *table, Service_Object object) {
  if (!table) return IPC_INVALID_HANDLE;
  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) {
    if (table->in_use[i]) continue;
    table->in_use[i] = true;
    memset(&table->sessions[i], 0, sizeof(table->sessions[i]));
    table->sessions[i].object = object;
    /* Handle value: slot index + 1, so 0 stays IPC_INVALID_HANDLE and the
     * space is visibly distinct from the Phase 2 table's
     * (linear_id << 15) | index encoding (ipc.h's "KNOWN SIMPLIFICATION"). */
    return i + 1u;
  }
  return IPC_INVALID_HANDLE;
}

IPC_Session *ipc_session_table_get(IPC_Session_Table *table, uint32_t handle) {
  if (!table || handle == IPC_INVALID_HANDLE || handle > IPC_MAX_SESSIONS) return NULL;
  const uint32_t index = handle - 1u;
  if (!table->in_use[index]) return NULL;
  return &table->sessions[index];
}

bool ipc_session_table_close(IPC_Session_Table *table, uint32_t handle) {
  if (!table || handle == IPC_INVALID_HANDLE || handle > IPC_MAX_SESSIONS) return false;
  const uint32_t index = handle - 1u;
  if (!table->in_use[index]) return false;
  table->in_use[index] = false;
  memset(&table->sessions[index], 0, sizeof(table->sessions[index]));
  return true;
}

/* ------------------------------------------------------------------ */
/* Parse.                                                               */
/* ------------------------------------------------------------------ */

Error ipc_request_parse(const uint8_t *buffer, uint32_t buffer_size, IPC_Request *out) {
  if (!buffer || !out || buffer_size != TLS_IPC_COMMAND_BUFFER_BYTES) {
    return ERR(RESULT_INVALID_ARGUMENT, "ipc_request_parse: NULL argument or bad buffer_size");
  }
  memset(out, 0, sizeof(*out));

  const uint32_t word0 = read_u32(buffer + 0);
  const uint32_t word1 = read_u32(buffer + 4);

  const uint32_t type = word0 & 0xFFFFu;
  const uint32_t num_x = (word0 >> 16) & 0xFu;
  const uint32_t num_a = (word0 >> 20) & 0xFu;
  const uint32_t num_b = (word0 >> 24) & 0xFu;
  const uint32_t num_w = (word0 >> 28) & 0xFu;
  const uint32_t data_size_words = word1 & 0x3FFu;
  const uint32_t c_flags = (word1 >> 10) & 0xFu;
  const bool has_special_header = (word1 >> 31) != 0;

  if (type != (uint32_t)IPC_MESSAGE_TYPE_REQUEST) {
    return ERR(RESULT_NOT_IMPLEMENTED,
              "ipc_request_parse: only IPC_MESSAGE_TYPE_REQUEST is supported (Phase 1)");
  }
  if (num_x != 0 || num_a != 0 || num_b != 0 || num_w != 0 || c_flags != 0) {
    return ERR(RESULT_NOT_IMPLEMENTED,
              "ipc_request_parse: X/A/B/C buffer descriptors are not supported (Phase 1)");
  }

  uint32_t offset = 8;
  bool has_pid = false;
  uint64_t pid = 0;
  if (has_special_header) {
    if (offset + 4u > buffer_size) {
      return ERR(RESULT_MEMORY_FAULT, "ipc_request_parse: special header truncated");
    }
    const uint32_t special = read_u32(buffer + offset);
    offset += 4;
    const bool send_pid = (special & 0x1u) != 0;
    const uint32_t copy_handle_count = (special >> 1) & 0xFu;
    const uint32_t move_handle_count = (special >> 5) & 0xFu;
    if (copy_handle_count != 0 || move_handle_count != 0) {
      return ERR(RESULT_NOT_IMPLEMENTED,
                "ipc_request_parse: request-direction handle descriptors are not supported (Phase 1)");
    }
    if (send_pid) {
      if (offset + 8u > buffer_size) {
        return ERR(RESULT_MEMORY_FAULT, "ipc_request_parse: pid truncated");
      }
      pid = read_u64(buffer + offset);
      offset += 8;
      has_pid = true;
    }
  }

  const uint32_t raw_data_offset = align_up(offset, IPC_RAW_DATA_ALIGN);
  const uint32_t raw_data_bytes = data_size_words * 4u;
  if (raw_data_bytes < IPC_CMIF_HEADER_BYTES) {
    return ERR(RESULT_MEMORY_FAULT, "ipc_request_parse: data_size smaller than the CMIF header");
  }
  if (raw_data_offset + raw_data_bytes > buffer_size) {
    return ERR(RESULT_MEMORY_FAULT, "ipc_request_parse: declared data_size runs past the command buffer");
  }

  const uint32_t magic = read_u32(buffer + raw_data_offset);
  if (magic != IPC_CMIF_MAGIC_SFCI) {
    return ERR(RESULT_NOT_IMPLEMENTED,
              "ipc_request_parse: raw data is not an SFCI (CMIF) request");
  }
  /* buffer + raw_data_offset + 4: CMIF version word, ignored - no real
   * service inspects it. */
  const uint64_t command_id_64 = read_u64(buffer + raw_data_offset + 8);

  out->command_id = (uint32_t)command_id_64; /* ipc.h: low 32 bits only -
                                              * every real command id fits */
  out->has_pid = has_pid;
  out->pid = pid;
  out->data = buffer + raw_data_offset + IPC_CMIF_HEADER_BYTES;
  out->data_size = raw_data_bytes - IPC_CMIF_HEADER_BYTES;
  return OK;
}

/* ------------------------------------------------------------------ */
/* Dispatch.                                                            */
/* ------------------------------------------------------------------ */

void ipc_dispatch(Service_Object *object, const IPC_Request *req, IPC_Response *res) {
  memset(res, 0, sizeof(*res));
  if (!object || !object->interface || !req) {
    res->result = HLE_RESULT_NOT_IMPLEMENTED;
    return;
  }

  const Service_Interface *iface = object->interface;
  size_t lo = 0;
  size_t hi = iface->command_count;
  const Service_Command *found = NULL;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const Service_Command *candidate = &iface->commands[mid];
    if (candidate->command_id == req->command_id) {
      found = candidate;
      break;
    }
    if (candidate->command_id < req->command_id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  if (!found) {
    /* §12 unimplemented-surface policy: unknown command id is logged and
     * fed to the trace buffer by the caller (svc_ipc.c), which has the
     * log/trace access and the interface name; this function stays
     * pure. */
    res->result = HLE_RESULT_NOT_IMPLEMENTED;
    return;
  }

  res->result = found->handler(object, req, res);
}

/* ------------------------------------------------------------------ */
/* Write.                                                               */
/* ------------------------------------------------------------------ */

Error ipc_response_write(uint8_t *buffer, uint32_t buffer_size, const IPC_Response *res,
                         const uint32_t *out_handles, uint32_t out_handle_count) {
  if (!buffer || !res || buffer_size != TLS_IPC_COMMAND_BUFFER_BYTES ||
      out_handle_count != res->out_object_count ||
      (out_handle_count > 0 && !out_handles)) {
    return ERR(RESULT_INVALID_ARGUMENT,
              "ipc_response_write: NULL argument, bad buffer_size, or handle count mismatch");
  }
  memset(buffer, 0, buffer_size);

  const bool has_special_header = out_handle_count > 0;
  uint32_t offset = 8;
  if (has_special_header) {
    /* send_pid=0 (bit 0), copy_handle_count=0 (bits 1-4), move_handle_count
     * in bits 5-8 - a response never needs to send its own pid, and
     * Phase 1 never produces a copy handle (ipc.h). */
    const uint32_t special = out_handle_count << 5;
    write_u32(buffer + offset, special);
    offset += 4;
    for (uint32_t i = 0; i < out_handle_count; i++) {
      write_u32(buffer + offset, out_handles[i]);
      offset += 4;
    }
  }

  const uint32_t raw_data_offset = align_up(offset, IPC_RAW_DATA_ALIGN);
  const uint32_t raw_data_bytes = IPC_CMIF_HEADER_BYTES; /* Phase 1 commands
                                                          * produce no extra
                                                          * output data */
  if (raw_data_offset + raw_data_bytes > buffer_size) {
    return ERR(RESULT_INVALID_ARGUMENT, "ipc_response_write: response does not fit the command buffer");
  }

  write_u32(buffer + raw_data_offset, IPC_CMIF_MAGIC_SFCO);
  write_u32(buffer + raw_data_offset + 4, 0); /* CMIF version */
  write_u64(buffer + raw_data_offset + 8, (uint64_t)res->result);

  /* Response message type: written 0 here, NOT re-verified in this pass
   * (see file header comment) - flagged for a cross-reference test
   * before this is trusted against real client-side response parsing. */
  const uint32_t word0 = 0;
  const uint32_t data_size_words = raw_data_bytes / 4u;
  const uint32_t word1 = (data_size_words & 0x3FFu) | (has_special_header ? (1u << 31) : 0u);
  write_u32(buffer + 0, word0);
  write_u32(buffer + 4, word1);

  return OK;
}
