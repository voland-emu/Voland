/**
 * core/hle/kernel/ipc unit test: HIPC+CMIF parse/write, ipc_dispatch's
 * bsearch, and the Phase 1 session table - all pure, no vmm/CPU backend
 * (ipc.h's design goal).
 *
 * Two independent sources of truth, so the tests cannot simply agree with
 * the implementation's own reading of the wire format:
 *   - ipc_fixtures.h: byte-exact libnx request buffers and libnx's
 *     response reader, written without ipc.h's constants.
 *   - build_request below: synthetic requests for the edge cases libnx
 *     never sends, built from ipc.h's named layout constants (the single
 *     definition of the layout, shared with ipc.c).
 */
#define CHECK_NAME "ipc_test"
#include "check.h"

#include "hle/hle.h"
#include "hle/kernel/ipc.h"
#include "ipc_fixtures.h"

#include <string.h>

#define BUF_BYTES TLS_IPC_COMMAND_BUFFER_BYTES

static void write_u32_at(uint8_t *buf, uint32_t offset, uint32_t v) { memcpy(buf + offset, &v, 4); }

static uint32_t align16(uint32_t value) { return (value + IPC_CMIF_ALIGN - 1u) & ~(IPC_CMIF_ALIGN - 1u); }

/* A Request with an optional pid, sized the way libnx sizes one: the raw
 * data section starts right after the pid and its word count includes
 * the IPC_CMIF_PADDING_BYTES reserve. `extra_data_words` grows the
 * declared section past the payload (to reach a near-full buffer). */
static void build_request(uint8_t *buf, bool send_pid, uint64_t pid, uint32_t command_id,
                          const void *payload, uint32_t payload_size, uint32_t extra_data_words) {
  memset(buf, FIXTURE_FILL_BYTE, BUF_BYTES);
  uint32_t offset = IPC_HIPC_HEADER_BYTES;
  uint32_t word1_flags = 0;
  if (send_pid) {
    write_u32_at(buf, offset, IPC_HIPC_SEND_PID_BIT);
    offset += IPC_HIPC_SPECIAL_HEADER_BYTES;
    memcpy(buf + offset, &pid, sizeof(pid));
    offset += IPC_HIPC_PID_BYTES;
    word1_flags = IPC_HIPC_HAS_SPECIAL_HEADER_BIT;
  }
  const uint32_t cmif_offset = align16(offset);
  write_u32_at(buf, cmif_offset + IPC_CMIF_MAGIC_OFFSET, IPC_CMIF_MAGIC_SFCI);
  write_u32_at(buf, cmif_offset + IPC_CMIF_VERSION_OFFSET, 0);
  write_u32_at(buf, cmif_offset + IPC_CMIF_COMMAND_ID_OFFSET, command_id);
  write_u32_at(buf, cmif_offset + IPC_CMIF_TOKEN_OFFSET, 0);
  if (payload_size > 0) memcpy(buf + cmif_offset + IPC_CMIF_HEADER_BYTES, payload, payload_size);

  const uint32_t data_bytes = IPC_CMIF_PADDING_BYTES + IPC_CMIF_HEADER_BYTES + payload_size;
  const uint32_t data_words =
      (data_bytes + IPC_HIPC_DATA_WORD_BYTES - 1u) / IPC_HIPC_DATA_WORD_BYTES + extra_data_words;
  write_u32_at(buf, 0, (uint32_t)IPC_MESSAGE_TYPE_REQUEST);
  write_u32_at(buf, 4, (data_words & IPC_HIPC_NUM_DATA_WORDS_MASK) | word1_flags);
}

static void set_word0(uint8_t *buf, uint32_t word0) { write_u32_at(buf, 0, word0); }

/* --- parse: libnx fixtures --- */

static void test_parse_libnx_register_client(void) {
  uint8_t buf[BUF_BYTES];
  fixture_ipc_load(buf, &k_fixture_register_client);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.type == IPC_MESSAGE_TYPE_REQUEST);
  CHECK(req.command_id == 0);
  CHECK(req.has_pid);
  /* libnx never writes the pid slot: whatever the guest left there comes
   * through, which is why ipc.h marks it untrusted. */
  CHECK(req.guest_pid == ((uint64_t)FIXTURE_FILL_WORD << 32 | FIXTURE_FILL_WORD));
  CHECK(req.data == buf + FIXTURE_REGISTER_CLIENT_DATA_OFFSET);
  CHECK(req.data_size == FIXTURE_REGISTER_CLIENT_DATA_END - FIXTURE_REGISTER_CLIENT_DATA_OFFSET);
}

static void test_parse_libnx_get_service(void) {
  uint8_t buf[BUF_BYTES];
  fixture_ipc_load(buf, &k_fixture_get_service_fsp);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.type == IPC_MESSAGE_TYPE_REQUEST);
  CHECK(req.command_id == 1);
  CHECK(!req.has_pid);
  CHECK(req.data == buf + FIXTURE_GET_SERVICE_DATA_OFFSET);
  CHECK(req.data_size == FIXTURE_GET_SERVICE_DATA_END - FIXTURE_GET_SERVICE_DATA_OFFSET);
  CHECK(memcmp(req.data, "fsp-srv", 8) == 0);
}

static void test_parse_libnx_close(void) {
  uint8_t buf[BUF_BYTES];
  fixture_ipc_load(buf, &k_fixture_close);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.type == IPC_MESSAGE_TYPE_CLOSE);
  CHECK(req.data == NULL);
  CHECK(req.data_size == 0);
}

static void test_parse_rejects_libnx_tipc(void) {
  uint8_t buf[BUF_BYTES];
  fixture_ipc_load(buf, &k_fixture_tipc_register_client);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);

  set_word0(buf, (uint32_t)IPC_MESSAGE_TYPE_TIPC_CLOSE);
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

/* --- parse: synthetic edge cases --- */

static void test_parse_near_full_buffer(void) {
  /* No special header: the data section starts at 8, so 62 words end
   * exactly at the end of the 0x100-byte buffer. Measuring the section
   * from the aligned CMIF header instead would reject this. */
  uint8_t buf[BUF_BYTES];
  const uint32_t max_words = (BUF_BYTES - IPC_HIPC_HEADER_BYTES) / IPC_HIPC_DATA_WORD_BYTES;
  build_request(buf, false, 0, 7, NULL, 0, 0);
  write_u32_at(buf, 4, max_words);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.command_id == 7);
  CHECK(req.data == buf + align16(IPC_HIPC_HEADER_BYTES) + IPC_CMIF_HEADER_BYTES);
  CHECK(req.data + req.data_size == buf + BUF_BYTES);

  write_u32_at(buf, 4, max_words + 1u);
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_MEMORY_FAULT);
}

static void test_parse_rejects_section_too_short_for_header(void) {
  /* The smallest valid section runs from 8 to the end of the aligned
   * header at 16..32 (6 words); one word fewer cuts the header short. */
  uint8_t buf[BUF_BYTES];
  build_request(buf, false, 0, 0, NULL, 0, 0);
  const uint32_t words_to_header_end =
      (align16(IPC_HIPC_HEADER_BYTES) + IPC_CMIF_HEADER_BYTES - IPC_HIPC_HEADER_BYTES) /
      IPC_HIPC_DATA_WORD_BYTES;
  write_u32_at(buf, 4, words_to_header_end);
  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.data_size == 0);

  write_u32_at(buf, 4, words_to_header_end - 1u);
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_MEMORY_FAULT);
}

static void test_parse_with_pid(void) {
  uint8_t buf[BUF_BYTES];
  const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  build_request(buf, true, 0xDEADBEEFull, 3, payload, sizeof(payload), 0);

  IPC_Request req;
  CHECK_OK(ipc_request_parse(buf, BUF_BYTES, &req));
  CHECK(req.command_id == 3);
  CHECK(req.has_pid);
  CHECK(req.guest_pid == 0xDEADBEEFull);
  CHECK(req.data_size >= sizeof(payload));
  CHECK(memcmp(req.data, payload, sizeof(payload)) == 0);
}

static void test_parse_rejects_bad_buffer_size(void) {
  uint8_t buf[BUF_BYTES];
  build_request(buf, false, 0, 0, NULL, 0, 0);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES - 1, &req), RESULT_INVALID_ARGUMENT);
  CHECK_CODE(ipc_request_parse(NULL, BUF_BYTES, &req), RESULT_INVALID_ARGUMENT);
}

static void test_parse_rejects_unsupported_cmif_types(void) {
  const IPC_Message_Type rejected[] = {
      IPC_MESSAGE_TYPE_INVALID,         IPC_MESSAGE_TYPE_LEGACY_REQUEST,
      IPC_MESSAGE_TYPE_LEGACY_CONTROL,  IPC_MESSAGE_TYPE_CONTROL,
      IPC_MESSAGE_TYPE_REQUEST_WITH_CONTEXT, IPC_MESSAGE_TYPE_CONTROL_WITH_CONTEXT,
  };
  uint8_t buf[BUF_BYTES];
  IPC_Request req;
  for (uint32_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
    build_request(buf, false, 0, 0, NULL, 0, 0);
    set_word0(buf, (uint32_t)rejected[i]);
    CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
  }
}

static void test_parse_rejects_buffer_descriptors(void) {
  const uint32_t shifts[] = {IPC_HIPC_NUM_X_SHIFT, IPC_HIPC_NUM_A_SHIFT, IPC_HIPC_NUM_B_SHIFT,
                             IPC_HIPC_NUM_W_SHIFT};
  uint8_t buf[BUF_BYTES];
  IPC_Request req;
  for (uint32_t i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
    build_request(buf, false, 0, 0, NULL, 0, 0);
    /* The count alone must reject, before any descriptor is read. */
    set_word0(buf, (uint32_t)IPC_MESSAGE_TYPE_REQUEST | (1u << shifts[i]));
    CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
  }

  build_request(buf, false, 0, 0, NULL, 0, 0);
  uint32_t word1;
  memcpy(&word1, buf + 4, 4);
  write_u32_at(buf, 4, word1 | (1u << IPC_HIPC_RECV_STATIC_MODE_SHIFT));
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

static void test_parse_rejects_request_handles(void) {
  const uint32_t shifts[] = {IPC_HIPC_NUM_COPY_HANDLES_SHIFT, IPC_HIPC_NUM_MOVE_HANDLES_SHIFT};
  uint8_t buf[BUF_BYTES];
  IPC_Request req;
  for (uint32_t i = 0; i < sizeof(shifts) / sizeof(shifts[0]); i++) {
    build_request(buf, true, 0, 0, NULL, 0, 0);
    write_u32_at(buf, IPC_HIPC_HEADER_BYTES, IPC_HIPC_SEND_PID_BIT | (1u << shifts[i]));
    CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
  }
}

static void test_parse_rejects_bad_magic(void) {
  uint8_t buf[BUF_BYTES];
  fixture_ipc_load(buf, &k_fixture_get_service_fsp);
  write_u32_at(buf, align16(IPC_HIPC_HEADER_BYTES) + IPC_CMIF_MAGIC_OFFSET, 0xFFFFFFFFu);
  IPC_Request req;
  CHECK_CODE(ipc_request_parse(buf, BUF_BYTES, &req), RESULT_NOT_IMPLEMENTED);
}

/* --- ipc_dispatch --- */

static HLE_Service_Result fake_cmd_a(Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)self;
  (void)req;
  (void)res;
  return HLE_RESULT_SUCCESS;
}

static HLE_Service_Result fake_cmd_b(Service_Object *self, const IPC_Request *req,
                                     IPC_Response *res) {
  (void)self;
  (void)req;
  ipc_response_add_move_object(res, (Service_Object){.interface = NULL, .instance = self});
  return HLE_RESULT_SUCCESS;
}

static const Service_Command fake_commands[] = {
    {0, fake_cmd_a, "A"},
    {5, fake_cmd_b, "B"},
};
static const Service_Interface fake_interface = {
    .name = "fake:", .commands = fake_commands, .command_count = 2};

static void test_dispatch_known_and_unknown_command(void) {
  Service_Object object = {.interface = &fake_interface, .instance = NULL};
  IPC_Request req;
  memset(&req, 0, sizeof(req));

  req.command_id = 5;
  IPC_Response res;
  ipc_dispatch(&object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_SUCCESS);
  CHECK(res.out_object_count == 1);

  req.command_id = 3; /* not in fake_commands */
  ipc_dispatch(&object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_NOT_IMPLEMENTED);
  CHECK(res.out_object_count == 0);
}

static void test_response_add_move_object_caps_at_max(void) {
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  CHECK(ipc_response_add_move_object(&res, (Service_Object){0}));
  CHECK(res.out_object_count == IPC_MAX_OUT_OBJECTS);
  /* IPC_MAX_OUT_OBJECTS is 1 (ipc.h) - a second add must fail, not grow
   * past the array. */
  CHECK(!ipc_response_add_move_object(&res, (Service_Object){0}));
  CHECK(res.out_object_count == IPC_MAX_OUT_OBJECTS);
}

/* --- ipc_response_write, read back the way libnx reads it --- */

static void test_response_write_no_handles(void) {
  uint8_t buf[BUF_BYTES];
  memset(buf, FIXTURE_FILL_BYTE, sizeof(buf));
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.result = 0x0E15u; /* any non-success value: sm NotRegistered */

  CHECK_OK(ipc_response_write(buf, BUF_BYTES, &res, NULL, 0));

  Fixture_Libnx_Response view;
  CHECK(fixture_libnx_read_response(buf, &view));
  CHECK(!view.has_special_header);
  CHECK(view.num_move_handles == 0);
  CHECK(view.cmif_offset == 0x10u);
  CHECK(view.result == 0x0E15u);
  /* The declared section starts where libnx's data_words does and covers
   * the whole header. */
  CHECK(view.data_words_offset + view.num_data_words * 4u >= view.cmif_offset + 16u);
}

static void test_response_write_one_handle(void) {
  uint8_t buf[BUF_BYTES];
  memset(buf, FIXTURE_FILL_BYTE, sizeof(buf));
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.result = HLE_RESULT_SUCCESS;
  res.out_object_count = 1;
  const uint32_t handle = 7;

  CHECK_OK(ipc_response_write(buf, BUF_BYTES, &res, &handle, 1));

  Fixture_Libnx_Response view;
  CHECK(fixture_libnx_read_response(buf, &view));
  CHECK(view.has_special_header);
  CHECK(view.num_copy_handles == 0);
  CHECK(view.num_move_handles == 1);
  CHECK(view.move_handles[0] == handle);
  CHECK(view.data_words_offset == 0x10u); /* after header, special header, one handle */
  CHECK(view.cmif_offset == 0x10u);
  CHECK(view.result == HLE_RESULT_SUCCESS);
  CHECK(view.data_words_offset + view.num_data_words * 4u >= view.cmif_offset + 16u);
}

static void test_response_write_rejects_handle_count_mismatch(void) {
  uint8_t buf[BUF_BYTES];
  IPC_Response res;
  memset(&res, 0, sizeof(res));
  res.out_object_count = 1;
  const uint32_t handle = 1;
  CHECK_CODE(ipc_response_write(buf, BUF_BYTES, &res, &handle, 0), RESULT_INVALID_ARGUMENT);
}

/* --- session table --- */

static void test_session_table_open_get_close(void) {
  IPC_Session_Table table;
  ipc_session_table_init(&table);

  const Service_Object object = {.interface = &fake_interface, .instance = (void *)0x1234};
  const uint32_t handle = ipc_session_table_open(&table, object);
  CHECK(handle != IPC_INVALID_HANDLE);

  IPC_Session *session = ipc_session_table_get(&table, handle);
  CHECK(session != NULL);
  CHECK(session->object.interface == &fake_interface);
  CHECK(session->object.instance == (void *)0x1234);
  CHECK(!session->is_domain);

  CHECK(ipc_session_table_get(&table, handle + 1000) == NULL);
  CHECK(ipc_session_table_get(&table, IPC_INVALID_HANDLE) == NULL);

  CHECK(ipc_session_table_close(&table, handle));
  CHECK(ipc_session_table_get(&table, handle) == NULL);
  CHECK(!ipc_session_table_close(&table, handle)); /* already closed */

  /* The freed slot is reused by the next open. */
  const uint32_t reopened = ipc_session_table_open(&table, object);
  CHECK(reopened == handle);
}

static void test_session_table_exhaustion(void) {
  IPC_Session_Table table;
  ipc_session_table_init(&table);
  const Service_Object object = {0};

  for (uint32_t i = 0; i < IPC_MAX_SESSIONS; i++) {
    CHECK(ipc_session_table_open(&table, object) != IPC_INVALID_HANDLE);
  }
  CHECK(ipc_session_table_open(&table, object) == IPC_INVALID_HANDLE);
}

int main(void) {
  test_parse_libnx_register_client();
  test_parse_libnx_get_service();
  test_parse_libnx_close();
  test_parse_rejects_libnx_tipc();
  test_parse_near_full_buffer();
  test_parse_rejects_section_too_short_for_header();
  test_parse_with_pid();
  test_parse_rejects_bad_buffer_size();
  test_parse_rejects_unsupported_cmif_types();
  test_parse_rejects_buffer_descriptors();
  test_parse_rejects_request_handles();
  test_parse_rejects_bad_magic();
  test_dispatch_known_and_unknown_command();
  test_response_add_move_object_caps_at_max();
  test_response_write_no_handles();
  test_response_write_one_handle();
  test_response_write_rejects_handle_count_mismatch();
  test_session_table_open_get_close();
  test_session_table_exhaustion();
  printf("[" CHECK_NAME "] all checks passed\n");
  return 0;
}
