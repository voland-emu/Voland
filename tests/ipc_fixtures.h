/**
 * IPC wire fixtures independent of core/hle/kernel/ipc.{h,c}.
 *
 * Requests: the exact command buffers libnx builds, traced by hand from
 * switchbrew/libnx b156b47 (sf/hipc.h hipcMakeRequest, sf/cmif.h
 * cmifMakeRequest/cmifMakeCloseRequest, sf/service.h serviceDispatchIn/
 * serviceClose, sf/tipc.h tipcMakeRequest, services/sm.c). libnx never
 * clears the TLS buffer and never writes the pid field or the padding in
 * front of the CMIF header, so every byte it does not write is
 * FIXTURE_FILL_BYTE here: a parser that depends on zeroed padding fails.
 * Each value is written as a literal with the libnx arithmetic beside it,
 * not computed from ipc.h's constants, so a wrong constant there cannot
 * also make the fixture wrong.
 *
 * Responses: fixture_libnx_read_response() is the reading half of libnx
 * (hipcParseResponse + cmifParseResponse, non-domain), transcribed from
 * libnx's own bitfield structs with literal offsets for the same reason.
 * A Voland response that this reader accepts is one libnx accepts.
 */
#ifndef VOLAND_TESTS_IPC_FIXTURES_H
#define VOLAND_TESTS_IPC_FIXTURES_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FIXTURE_IPC_BUFFER_BYTES 0x100u /* libnx: the TLS command buffer */
#define FIXTURE_FILL_BYTE 0xAAu
#define FIXTURE_FILL_WORD 0xAAAAAAAAu

typedef struct Fixture_IPC_Word {
  uint32_t offset;
  uint32_t value;
} Fixture_IPC_Word;

typedef struct Fixture_IPC_Message {
  const char *name;
  const Fixture_IPC_Word *words;
  uint32_t word_count;
} Fixture_IPC_Message;

/* sm RegisterClient over CMIF: _smCmifCmdInPid(0) ->
 * serviceDispatchIn(cmd 0, u64 pid_placeholder = 0, .in_send_pid = true).
 *   cmifMakeRequest: actual_size = 16 (pad) + 16 (CmifInHeader) + 8 = 40,
 *   num_data_words = 10. hipcMakeRequest: special header (send_pid), pid
 *   slot skipped unwritten, data_words at 8 + 4 + 8 = 0x14. CMIF header
 *   at align16(0x14) = 0x20; payload at 0x30; section ends 0x14 + 40 =
 *   0x3C. */
static const Fixture_IPC_Word k_fixture_register_client_words[] = {
    {0x00, 0x00000004u}, /* type = CmifCommandType_Request (4), no descriptors */
    {0x04, 0x8000000Au}, /* num_data_words = 10, has_special_header */
    {0x08, 0x00000001u}, /* send_pid = 1, no handles */
    /* 0x0C..0x13: pid slot, left as fill by libnx (the kernel stamps it) */
    /* 0x14..0x1F: alignment padding, fill */
    {0x20, 0x49434653u}, /* "SFCI" */
    {0x24, 0x00000000u}, /* version 0 (no context) */
    {0x28, 0x00000000u}, /* command_id 0 = RegisterClient */
    {0x2C, 0x00000000u}, /* token 0 */
    {0x30, 0x00000000u}, /* pid_placeholder, low */
    {0x34, 0x00000000u}, /* pid_placeholder, high */
};
#define FIXTURE_REGISTER_CLIENT_DATA_OFFSET 0x30u
#define FIXTURE_REGISTER_CLIENT_DATA_END 0x3Cu

/* sm GetServiceHandle("fsp-srv") over CMIF: smGetServiceOriginal ->
 * serviceDispatchIn(cmd 1, u64 name, .out_handle_attrs = { HipcMove }).
 *   actual_size = 16 + 16 + 8 = 40, num_data_words = 10; no special
 *   header, data_words at 8; CMIF header at align16(8) = 0x10; name at
 *   0x20; section ends 8 + 40 = 0x30. */
static const Fixture_IPC_Word k_fixture_get_service_fsp_words[] = {
    {0x00, 0x00000004u}, /* type = Request */
    {0x04, 0x0000000Au}, /* num_data_words = 10 */
    /* 0x08..0x0F: alignment padding, fill */
    {0x10, 0x49434653u}, /* "SFCI" */
    {0x14, 0x00000000u}, /* version */
    {0x18, 0x00000001u}, /* command_id 1 = GetServiceHandle */
    {0x1C, 0x00000000u}, /* token */
    {0x20, 0x2D707366u}, /* "fsp-" */
    {0x24, 0x00767273u}, /* "srv\0" */
};
#define FIXTURE_GET_SERVICE_DATA_OFFSET 0x20u
#define FIXTURE_GET_SERVICE_DATA_END 0x30u

/* serviceClose on an owned, non-domain session: cmifMakeCloseRequest(base,
 * 0) -> hipcMakeRequestInline(.type = CmifCommandType_Close). */
static const Fixture_IPC_Word k_fixture_close_words[] = {
    {0x00, 0x00000002u}, /* type = Close (2) */
    {0x04, 0x00000000u}, /* no data words, no special header */
};

/* RegisterClient over TIPC, as sm.c's _smTipcCmdInPid(0) would send it:
 * tipcMakeRequest(type = 0 + 16, data_size 0, send_pid). No SFCI. */
static const Fixture_IPC_Word k_fixture_tipc_register_client_words[] = {
    {0x00, 0x00000010u}, /* type = 16 + command 0 */
    {0x04, 0x80000000u}, /* no data words, has_special_header */
    {0x08, 0x00000001u}, /* send_pid */
};

#define FIXTURE_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

static const Fixture_IPC_Message k_fixture_register_client = {
    "RegisterClient (CMIF)", k_fixture_register_client_words,
    FIXTURE_COUNT(k_fixture_register_client_words)};
static const Fixture_IPC_Message k_fixture_get_service_fsp = {
    "GetServiceHandle fsp-srv (CMIF)", k_fixture_get_service_fsp_words,
    FIXTURE_COUNT(k_fixture_get_service_fsp_words)};
static const Fixture_IPC_Message k_fixture_close = {
    "Close (CMIF)", k_fixture_close_words, FIXTURE_COUNT(k_fixture_close_words)};
static const Fixture_IPC_Message k_fixture_tipc_register_client = {
    "RegisterClient (TIPC)", k_fixture_tipc_register_client_words,
    FIXTURE_COUNT(k_fixture_tipc_register_client_words)};

/* Fills `buf` the way a used TLS block looks, then writes the message. */
static inline void fixture_ipc_load(uint8_t *buf, const Fixture_IPC_Message *message) {
  memset(buf, FIXTURE_FILL_BYTE, FIXTURE_IPC_BUFFER_BYTES);
  for (uint32_t i = 0; i < message->word_count; i++) {
    memcpy(buf + message->words[i].offset, &message->words[i].value, sizeof(uint32_t));
  }
}

/* --- libnx's reading side (non-domain) --- */

#define FIXTURE_MAX_HANDLES 15u /* 4-bit counts */

typedef struct Fixture_Libnx_Response {
  bool has_special_header;
  uint32_t num_data_words;
  uint32_t num_copy_handles;
  uint32_t num_move_handles;
  uint32_t move_handles[FIXTURE_MAX_HANDLES];
  uint32_t data_words_offset; /* hipc.data_words - base */
  uint32_t cmif_offset;       /* cmifGetAlignedDataStart(data_words, base) - base */
  uint32_t magic;
  uint32_t result;
} Fixture_Libnx_Response;

static inline uint32_t fixture_read_u32(const uint8_t *buf, uint32_t offset) {
  uint32_t v;
  memcpy(&v, buf + offset, sizeof(v));
  return v;
}

/* hipcParseResponse then cmifParseResponse(is_domain = false). Returns
 * false where cmifParseResponse would return
 * LibnxError_InvalidCmifOutHeader. Like libnx, it reads neither the type
 * field nor num_data_words to locate anything. */
static inline bool fixture_libnx_read_response(const uint8_t *buf, Fixture_Libnx_Response *out) {
  memset(out, 0, sizeof(*out));
  const uint32_t word0 = fixture_read_u32(buf, 0);
  const uint32_t word1 = fixture_read_u32(buf, 4);
  const uint32_t num_statics = (word0 >> 16) & 0xFu; /* HipcHeader.num_send_statics */
  out->num_data_words = word1 & 0x3FFu;              /* HipcHeader.num_data_words */
  out->has_special_header = (word1 >> 31) != 0;      /* HipcHeader.has_special_header */
  uint32_t offset = 8;                               /* sizeof(HipcHeader) */
  if (out->has_special_header) {
    const uint32_t special = fixture_read_u32(buf, offset);
    offset += 4; /* sizeof(HipcSpecialHeader) */
    out->num_copy_handles = (special >> 1) & 0xFu;
    out->num_move_handles = (special >> 5) & 0xFu;
    if (special & 1u) offset += 8; /* pid */
  }
  offset += out->num_copy_handles * 4u;
  for (uint32_t i = 0; i < out->num_move_handles; i++) {
    out->move_handles[i] = fixture_read_u32(buf, offset);
    offset += 4;
  }
  offset += num_statics * 8u; /* sizeof(HipcStaticDescriptor) */
  out->data_words_offset = offset;
  out->cmif_offset = (offset + 15u) & ~15u;
  out->magic = fixture_read_u32(buf, out->cmif_offset);       /* CmifOutHeader.magic */
  out->result = fixture_read_u32(buf, out->cmif_offset + 8u); /* CmifOutHeader.result */
  return out->magic == 0x4F434653u;                           /* CMIF_OUT_HEADER_MAGIC */
}

#endif /* VOLAND_TESTS_IPC_FIXTURES_H */
