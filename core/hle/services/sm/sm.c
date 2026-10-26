/**
 * sm: service. See sm.h for scope (host-side registry, no guest-facing
 * RegisterService/UnregisterService) and the flagged protocol constants.
 */
#include "hle/services/sm/sm.h"

#include <string.h>

static HLE_Service_Result sm_cmd_register_client(Service_Object *self, const IPC_Request *req,
                                                  IPC_Response *res) {
  (void)self;
  (void)req;
  (void)res;
  /* Explicit stub (§12 "explicit stub allowlist"): every libnx-linked
   * title calls this once right after ConnectToNamedPort("sm:"), before
   * its first GetServiceHandle - see sm.h. The request's PID descriptor
   * and reserved u64 are read by nothing here; Voland has no per-client
   * state to attach them to in Phase 1. */
  return HLE_RESULT_SUCCESS;
}

static HLE_Service_Result sm_cmd_get_service(Service_Object *self, const IPC_Request *req,
                                             IPC_Response *res) {
  SM_Context *sm = (SM_Context *)self->instance;

  /* Zero-padded 8-byte ServiceName (sm.h). A request whose data_size is
   * short of SM_SERVICE_NAME_BYTES is malformed, but harmlessly so: the
   * zero-padded name just will not match any registration, and falls
   * through to SM_RESULT_NOT_FOUND like any other unregistered name -
   * no separate error path needed for it. */
  uint8_t name[SM_SERVICE_NAME_BYTES];
  memset(name, 0, sizeof(name));
  const uint32_t copy_bytes =
      req->data_size < SM_SERVICE_NAME_BYTES ? req->data_size : SM_SERVICE_NAME_BYTES;
  memcpy(name, req->data, copy_bytes);

  for (uint32_t i = 0; i < sm->registration_count; i++) {
    if (memcmp(sm->registrations[i].name, name, SM_SERVICE_NAME_BYTES) != 0) continue;
    const Service_Object object = {
        .interface = sm->registrations[i].interface,
        .instance = sm->registrations[i].instance,
    };
    if (!ipc_response_add_move_object(res, object)) {
      /* IPC_MAX_OUT_OBJECTS is 1 and this is the only object this
       * handler ever adds - unreachable from any guest input, defensive
       * only. */
      return HLE_RESULT_NOT_IMPLEMENTED;
    }
    return HLE_RESULT_SUCCESS;
  }
  return SM_RESULT_NOT_FOUND;
}

/* Sorted ascending by command_id (ipc_dispatch's bsearch contract, ipc.h). */
static const Service_Command sm_commands[] = {
    {SM_CMD_REGISTER_CLIENT, sm_cmd_register_client, "RegisterClient"},
    {SM_CMD_GET_SERVICE, sm_cmd_get_service, "GetServiceHandle"},
};

const Service_Interface sm_interface = {
    .name = "sm:",
    .commands = sm_commands,
    .command_count = sizeof(sm_commands) / sizeof(sm_commands[0]),
};

void sm_context_init(SM_Context *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
}

Error sm_register_service(SM_Context *sm, const uint8_t name[SM_SERVICE_NAME_BYTES],
                          const Service_Interface *interface, void *instance) {
  if (!sm || !name || !interface) {
    return ERR(RESULT_INVALID_ARGUMENT, "sm_register_service: NULL argument");
  }
  for (uint32_t i = 0; i < sm->registration_count; i++) {
    if (memcmp(sm->registrations[i].name, name, SM_SERVICE_NAME_BYTES) == 0) {
      return ERR(RESULT_ALREADY_EXISTS, "sm_register_service: name already registered");
    }
  }
  if (sm->registration_count >= SM_MAX_REGISTRATIONS) {
    return ERR(RESULT_OUT_OF_MEMORY, "sm_register_service: SM_MAX_REGISTRATIONS reached");
  }
  SM_Registration *slot = &sm->registrations[sm->registration_count++];
  memcpy(slot->name, name, SM_SERVICE_NAME_BYTES);
  slot->interface = interface;
  slot->instance = instance;
  return OK;
}

Service_Object sm_make_service_object(SM_Context *sm) {
  return (Service_Object){.interface = &sm_interface, .instance = sm};
}
