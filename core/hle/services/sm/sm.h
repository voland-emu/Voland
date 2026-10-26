/**
 * sm: - the service manager. See docs/DESIGN.md §12 ("Services
 * implementation priority", item 1: "all other services connect through
 * this") and §25 Phase 1 ("Minimal IPC + sm: stub").
 *
 * Voland has no second guest process ever playing a service's SERVER
 * role (see svc_ipc.h's header comment): every "service" is a host-side
 * C Service_Object wired directly into this registry. Consequently sm:'s
 * registry is populated host-side, not guest-side:
 *
 *   - sm_register_service() is called once by each service module's own
 *     init routine (fsp-srv, hid:, ... as they land in later phases) at
 *     emulator/process bootstrap time - NEVER in response to a guest IPC
 *     call.
 *   - The guest-facing RegisterService/UnregisterService CMIF commands
 *     (real sm: command ids, per switchbrew) are therefore NOT
 *     implemented here and are an explicit, named omission from
 *     sm_interface's command table (§12 unimplemented-surface policy:
 *     "never silently return success by default" - an unknown command
 *     id already yields RESULT_NOT_IMPLEMENTED via ipc_dispatch, so no
 *     stub entry is needed to get that behaviour). A title legitimately
 *     calling either in Voland's single-process model is itself the bug
 *     report; the real multi-process ServerPort/AcceptSession/
 *     ReplyAndReceive mechanism those commands exist to support is out of
 *     scope for this emulator's architecture, not just this phase.
 *   - RegisterClient (real command id 0, see below) IS implemented, as an
 *     explicit stub (§12 "explicit stub allowlist"): every libnx-linked
 *     title calls it immediately after ConnectToNamedPort("sm:"), before
 *     its first GetServiceHandle, so without it nothing past "connect to
 *     sm:" ever works. It takes a PID descriptor + a reserved u64
 *     (switchbrew), neither of which Voland needs to validate or record
 *     for Phase 1 - the stub reads nothing and returns Success.
 *   - GetServiceHandle (real command id 1) is the one lookup command
 *     Phase 1 implements. For every name no module has registered yet
 *     (i.e. everything, today - no service module exists before Phase
 *     2+), it legitimately returns "service not found", matching real
 *     Horizon's honest behaviour for an unavailable service rather than
 *     a lie that unblocks boot further than Phase 1 claims to reach.
 *
 * Protocol constants below (SM_CMD_REGISTER_CLIENT=0, SM_CMD_GET_SERVICE=1,
 * SM_RESULT_NOT_FOUND) were checked against switchbrew's "sm" service
 * page and error-code module table (module 21 = SM) rather than recalled
 * from memory - read-only reference per CLAUDE.md rule 2's existing
 * carve-out for platform ABI/protocol docs, not another emulator's
 * source. One gap: switchbrew's page did not itself spell out
 * description=7 as "service not registered" for module 21; that pairing
 * is inferred from an independent reimplementation's named constant for
 * the same module, which documents the Result's MEANING, not its code -
 * reimplementing from observed behaviour (rule 2), not copying. Flagging
 * this one inference for a maintainer double-check before sm.c ships,
 * same as every other verify-before-commit note in this codebase. */
#ifndef SWITCH_HLE_SERVICES_SM_SM_H
#define SWITCH_HLE_SERVICES_SM_SM_H

#include <stdint.h>

#include "common/result.h"
#include "hle/hle.h" /* HLE_MAKE_RESULT (value-level only - same shallow
                      * dependency svc_memory.h already takes on hle.h) */
#include "hle/kernel/ipc.h"

/* Horizon's ServiceName: up to 8 bytes, NUL-padded (not necessarily NUL-
 * terminated - "fsp-srv" is exactly 7 characters + 1 pad byte). */
#define SM_SERVICE_NAME_BYTES ((uint32_t)8)

/* Arbitrary, Phase 1: one registration per service Voland will ever host
 * host-side plus headroom; revisit if a later phase's module count
 * approaches it. Real sm: has no such cap - every name instead goes
 * through a kernel port lookup. */
#define SM_MAX_REGISTRATIONS ((uint32_t)32)

/* Real sm: CMIF command ids (switchbrew "Services API", current
 * numbering - command 0 was "Initialize" pre-rename, command 1 was
 * "GetService"; see header comment). */
#define SM_CMD_REGISTER_CLIENT ((uint32_t)0)
#define SM_CMD_GET_SERVICE ((uint32_t)1)

/* Module 21 = SM (switchbrew error-code module table, verified);
 * description 7 = "service not registered" (inferred from an
 * independent reimplementation's named constant for the same module -
 * see header comment's one flagged gap). */
#define SM_RESULT_NOT_FOUND HLE_MAKE_RESULT(21, 7)

typedef struct SM_Registration {
  uint8_t name[SM_SERVICE_NAME_BYTES];
  const Service_Interface *interface;
  void *instance; /* the registered Service_Object's instance pointer;
                   * opaque to sm, owned by whoever registered it */
} SM_Registration;

typedef struct SM_Context {
  SM_Registration registrations[SM_MAX_REGISTRATIONS];
  uint32_t registration_count;
} SM_Context;

void sm_context_init(SM_Context *out);

/* Host-side registration call - see header comment. `name` need not be
 * NUL-terminated; exactly SM_SERVICE_NAME_BYTES are read.
 *   RESULT_INVALID_ARGUMENT  NULL argument
 *   RESULT_ALREADY_EXISTS    `name` is already registered
 *   RESULT_OUT_OF_MEMORY     SM_MAX_REGISTRATIONS reached */
Error sm_register_service(SM_Context *sm, const uint8_t name[SM_SERVICE_NAME_BYTES],
                          const Service_Interface *interface, void *instance);

/* Binds a Service_Object to `sm` for the session ConnectToNamedPort("sm:")
 * opens (svc_ipc.c). One per HLE_Context; sm's own command handlers read
 * `self->instance` back as `SM_Context*`. */
Service_Object sm_make_service_object(SM_Context *sm);

/* sm:'s command table. Phase 1: GetService only - see header comment for
 * why RegisterService/UnregisterService are absent rather than stubbed. */
extern const Service_Interface sm_interface;

#endif /* SWITCH_HLE_SERVICES_SM_SM_H */
