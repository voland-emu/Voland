/**
 * core/hle/services/sm unit test: the host-side registry
 * (sm_register_service) and sm_interface's two commands, driven directly
 * through ipc_dispatch - no vmm/CPU backend needed (sm.h's handlers only
 * touch IPC_Request/IPC_Response and SM_Context).
 */
#define CHECK_NAME "sm_test"
#include "check.h"

#include "hle/hle.h"
#include "hle/services/sm/sm.h"

#include <string.h>

static const Service_Interface fake_fsp_interface = {
    .name = "fsp-srv", .commands = NULL, .command_count = 0};
static int fake_fsp_instance_marker = 0;

static void name8(uint8_t out[SM_SERVICE_NAME_BYTES], const char *s) {
  memset(out, 0, SM_SERVICE_NAME_BYTES);
  memcpy(out, s, strlen(s) < SM_SERVICE_NAME_BYTES ? strlen(s) : SM_SERVICE_NAME_BYTES);
}

static void test_register_service(void) {
  SM_Context sm;
  sm_context_init(&sm);
  uint8_t name[SM_SERVICE_NAME_BYTES];
  name8(name, "fsp-srv");

  CHECK_OK(sm_register_service(&sm, name, &fake_fsp_interface, &fake_fsp_instance_marker));
  CHECK(sm.registration_count == 1);

  /* Duplicate name rejected. */
  CHECK_CODE(sm_register_service(&sm, name, &fake_fsp_interface, &fake_fsp_instance_marker),
            RESULT_ALREADY_EXISTS);

  CHECK_CODE(sm_register_service(NULL, name, &fake_fsp_interface, NULL), RESULT_INVALID_ARGUMENT);
}

static void test_register_service_exhaustion(void) {
  SM_Context sm;
  sm_context_init(&sm);
  for (uint32_t i = 0; i < SM_MAX_REGISTRATIONS; i++) {
    uint8_t name[SM_SERVICE_NAME_BYTES];
    char label[16];
    snprintf(label, sizeof(label), "svc%u", i);
    name8(name, label);
    CHECK_OK(sm_register_service(&sm, name, &fake_fsp_interface, NULL));
  }
  uint8_t one_more[SM_SERVICE_NAME_BYTES];
  name8(one_more, "overflow");
  CHECK_CODE(sm_register_service(&sm, one_more, &fake_fsp_interface, NULL), RESULT_OUT_OF_MEMORY);
}

/* Builds an IPC_Request carrying `payload` as the raw data - sm's
 * handlers only read IPC_Request::data/data_size/command_id, never the
 * wire bytes around them, so a hand-built struct is enough; the wire
 * format itself is ipc_test.c's job. */
static IPC_Request make_request(uint32_t command_id, const void *payload, uint32_t payload_size) {
  IPC_Request req;
  memset(&req, 0, sizeof(req));
  req.command_id = command_id;
  req.data = (const uint8_t *)payload;
  req.data_size = payload_size;
  return req;
}

static void test_register_client_stub_succeeds(void) {
  SM_Context sm;
  sm_context_init(&sm);
  Service_Object sm_object = sm_make_service_object(&sm);

  const IPC_Request req = make_request(SM_CMD_REGISTER_CLIENT, NULL, 0);
  IPC_Response res;
  ipc_dispatch(&sm_object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_SUCCESS);
  CHECK(res.out_object_count == 0);
}

static void test_get_service_found(void) {
  SM_Context sm;
  sm_context_init(&sm);
  Service_Object sm_object = sm_make_service_object(&sm);

  uint8_t name[SM_SERVICE_NAME_BYTES];
  name8(name, "fsp-srv");
  CHECK_OK(sm_register_service(&sm, name, &fake_fsp_interface, &fake_fsp_instance_marker));

  const IPC_Request req = make_request(SM_CMD_GET_SERVICE, name, SM_SERVICE_NAME_BYTES);
  IPC_Response res;
  ipc_dispatch(&sm_object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_SUCCESS);
  CHECK(res.out_object_count == 1);
  CHECK(res.out_objects[0].interface == &fake_fsp_interface);
  CHECK(res.out_objects[0].instance == &fake_fsp_instance_marker);
}

static void test_get_service_not_found(void) {
  SM_Context sm;
  sm_context_init(&sm);
  Service_Object sm_object = sm_make_service_object(&sm);

  uint8_t name[SM_SERVICE_NAME_BYTES];
  name8(name, "nope-srv");
  const IPC_Request req = make_request(SM_CMD_GET_SERVICE, name, SM_SERVICE_NAME_BYTES);
  IPC_Response res;
  ipc_dispatch(&sm_object, &req, &res);
  CHECK((uint32_t)res.result == SM_RESULT_NOT_FOUND);
  CHECK(res.out_object_count == 0);
}

static void test_get_service_short_payload_treated_as_not_found(void) {
  SM_Context sm;
  sm_context_init(&sm);
  Service_Object sm_object = sm_make_service_object(&sm);
  uint8_t name[SM_SERVICE_NAME_BYTES];
  name8(name, "fsp-srv");
  CHECK_OK(sm_register_service(&sm, name, &fake_fsp_interface, NULL));

  /* A request with fewer than SM_SERVICE_NAME_BYTES of payload zero-pads
   * (sm.c) rather than erroring - it just will not match "fsp-srv\0". */
  const uint8_t short_payload[3] = {'f', 's', 'p'};
  const IPC_Request req = make_request(SM_CMD_GET_SERVICE, short_payload, sizeof(short_payload));
  IPC_Response res;
  ipc_dispatch(&sm_object, &req, &res);
  CHECK((uint32_t)res.result == SM_RESULT_NOT_FOUND);
}

static void test_unknown_command_not_implemented(void) {
  SM_Context sm;
  sm_context_init(&sm);
  Service_Object sm_object = sm_make_service_object(&sm);
  const IPC_Request req = make_request(99, NULL, 0); /* RegisterService et al: not implemented, see sm.h */
  IPC_Response res;
  ipc_dispatch(&sm_object, &req, &res);
  CHECK((uint32_t)res.result == HLE_RESULT_NOT_IMPLEMENTED);
}

int main(void) {
  test_register_service();
  test_register_service_exhaustion();
  test_register_client_stub_succeeds();
  test_get_service_found();
  test_get_service_not_found();
  test_get_service_short_payload_treated_as_not_found();
  test_unknown_command_not_implemented();
  printf("[" CHECK_NAME "] all checks passed\n");
  return 0;
}
