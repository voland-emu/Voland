/**
 * HIPC transport + CMIF service protocol, and the service-object dispatch
 * pattern every interface is built from. See docs/DESIGN.md §12 ("The call
 * surface: SVC -> HIPC -> CMIF") and §25 Phase 1 ("Minimal IPC + sm: stub").
 *
 * Layering, deliberately split from svc_ipc.h the same way vmm.h is split
 * from svc_memory.h: this file is the reusable protocol + dispatch
 * mechanism (parse a request, route it, write a response) with no SVC or
 * TLS knowledge; svc_ipc.h is the thin SVC-shaped layer that owns the TLS
 * command buffer and the session table and calls through to this one.
 * That split is a deviation from §2's repo-structure tree (which lists a
 * single `ipc.{h,c}`) in the same spirit as the already-shipped
 * memory.{h,c} -> svc_memory.{h,c} split (§25 Phase 1 checkbox 5); §2
 * should be updated to match in the same PR that implements this header,
 * per CLAUDE.md "deviations update the doc".
 *
 * Scope for this header (Phase 1, "Minimal IPC"):
 *   - Message type must be IPC_MESSAGE_TYPE_REQUEST. Control messages
 *     (domain conversion, session cloning, pointer-buffer-size query -
 *     HIPC types 2/3/5/6/7) are parsed far enough to be recognised and
 *     then rejected with RESULT_NOT_IMPLEMENTED; nothing dispatches
 *     through this phase needs them. sm: is conventionally never
 *     domain-converted in practice, and no other service exists yet to
 *     need domains before Phase 2+.
 *   - X/A/B/C buffer descriptors: rejected (count must be 0 in the
 *     request). sm:'s only Phase 1 command (GetService) takes its
 *     8-byte ServiceName in the raw data payload, not a buffer - real
 *     Horizon does this too. The first service that needs a buffer
 *     descriptor is what pulls support for them in.
 *   - Handle descriptors in the REQUEST direction (the guest handing
 *     Voland a handle): rejected (copy/move handle counts must be 0).
 *     Nothing in sm:'s Phase 1 command table receives one. The
 *     send-pid bit IS supported (parsed into IPC_Request::pid) since the
 *     kernel sets it unconditionally on every HIPC message's special
 *     header for connections to anything (switchbrew "IPC Marshalling");
 *     a parser that can't read it can't read a single real message.
 *   - Handle descriptors in the RESPONSE direction: move objects only
 *     (IPC_Response::out_objects), capped at IPC_MAX_OUT_OBJECTS. Copy
 *     handles out: not produced by anything Phase 1 dispatches; the
 *     wire format support is not built until a command needs it.
 *
 * KNOWN SIMPLIFICATION, flagged for review (CLAUDE.md "deviations update
 * the doc"): IPC_Session_Table below is a Phase 1 stand-in for §12's
 * generic per-process kernel-object handle table, which process.h already
 * documents as Phase 2 scope ("the Phase 2 handle table must issue
 * exactly [0x8000] to the main thread. Until it exists the constant is
 * the contract"). This table hands out session handles ONLY - no
 * threads, events, shared memory, or transfer memory - from a small flat
 * id space deliberately distinct from the real (linear_id << 15) | index
 * encoding Phase 2 will use (whose first value, 0x8000, is already
 * PROCESS_MAIN_THREAD_HANDLE). When the Phase 2 table lands, this one is
 * deleted whole, not migrated: a session becomes one more object kind in
 * that table's union, not a special case kept alongside it.
 *
 * OPEN DESIGN QUESTION for maintainer review: Service_Command_Fn's return
 * type. §12's sketch names it `HLE_ServiceResult` without defining it.
 * This header defines it as a raw Horizon Result (the same uint32_t
 * space as hle.h's HLE_RESULT_ constants and HLE_MAKE_RESULT, NOT
 * common/result.h's host-side Error) - a command handler's return value
 * becomes the response's SFCO Result verbatim, the same "write a raw code the guest
 * reads" shape every SVC handler already uses at the hle_on_svc layer.
 */
#ifndef SWITCH_HLE_KERNEL_IPC_H
#define SWITCH_HLE_KERNEL_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/result.h"
#include "hle/kernel/tls.h" /* TLS_IPC_COMMAND_BUFFER_BYTES: the single source
                             * for the command-buffer size every function
                             * below bounds-checks against (no magic numbers,
                             * CLAUDE.md rule 4) */

/* ------------------------------------------------------------------ */
/* Service-object dispatch pattern (§12 "The C service-object pattern"). */
/* ------------------------------------------------------------------ */

typedef struct Service_Interface Service_Interface; /* fwd; defined below */
/* fwd; defined below. Forward-declared here at FILE scope on purpose -
 * declaring `struct IPC_Request`/`struct IPC_Response` only inside
 * Service_Command_Fn's own parameter list would give each a scope
 * limited to that prototype (a standard, easy-to-miss C rule), making it
 * a DIFFERENT, incompatible tag from the real file-scope struct these
 * typedefs name further down - every .c file that defines a
 * Service_Command_Fn would then fail to match against ipc_dispatch's own
 * IPC_Request / IPC_Response parameters. */
typedef struct IPC_Request IPC_Request;
typedef struct IPC_Response IPC_Response;

/* One instantiated interface: a vtable pointer plus opaque per-instance
 * state. sm: has exactly one Service_Object for its own lifetime (bound
 * to the session ConnectToNamedPort opens); any OTHER service a Phase 2+
 * module registers with sm (sm.h) gets its own, created by that module,
 * not by this file. */
typedef struct Service_Object {
  const Service_Interface *interface;
  void *instance; /* e.g. SM_Context* for sm:'s own object; opaque here */
} Service_Object;

/* Raw Horizon Result (see "OPEN DESIGN QUESTION" above) - becomes the
 * response's SFCO Result verbatim. Not common/result.h's Error. */
typedef uint32_t HLE_Service_Result;

typedef HLE_Service_Result (*Service_Command_Fn)(
    Service_Object *self, const IPC_Request *req, IPC_Response *res);

typedef struct Service_Command {
  uint32_t command_id;
  Service_Command_Fn handler;
  const char *name; /* trace/coverage output (§12) */
} Service_Command;

struct Service_Interface {
  const char *name;                /* e.g. "sm:", for trace/coverage */
  const Service_Command *commands; /* sorted by command_id ascending; bsearch'd */
  size_t command_count;
};

/* ------------------------------------------------------------------ */
/* Parsed request / response views (§12 "HIPC"/"CMIF").                */
/* ------------------------------------------------------------------ */

/* HIPC message types this phase recognises (switchbrew "IPC Marshalling").
 * Only REQUEST is dispatched; every other value is a recognised-but-
 * unsupported Control/Close message (see header comment). */
typedef enum IPC_Message_Type {
  IPC_MESSAGE_TYPE_INVALID = 0,
  IPC_MESSAGE_TYPE_LEGACY_REQUEST = 1,
  IPC_MESSAGE_TYPE_CLOSE = 2,
  IPC_MESSAGE_TYPE_LEGACY_CONTROL = 3,
  IPC_MESSAGE_TYPE_REQUEST = 4,
  IPC_MESSAGE_TYPE_CONTROL = 5,
  IPC_MESSAGE_TYPE_REQUEST_WITH_CONTEXT = 6,
  IPC_MESSAGE_TYPE_CONTROL_WITH_CONTEXT = 7,
} IPC_Message_Type;

typedef struct IPC_Request {
  uint32_t command_id; /* CMIF command id, i.e. the word right after the
                        * SFCI magic in the data payload */
  bool has_pid;         /* HIPC special header's send_pid bit */
  uint64_t pid;         /* valid iff has_pid: the kernel-stamped caller pid */
  const uint8_t *data;  /* raw payload after the command id, `data_size`
                         * bytes; points into the caller's own local copy
                         * of the command buffer (svc_ipc.c) - valid only
                         * for the parse/dispatch/write call chain's
                         * duration, never retained past it */
  uint32_t data_size;
} IPC_Request;

/* Maximum handles one IPC response can move out. Phase 1's only producer
 * (sm:'s GetService) emits at most one; this is a small named cap, not a
 * derivation of the HIPC move-handle count field's real range (4 bits,
 * max 15), because nothing here needs more yet - revisit together with
 * the first service that does. */
#define IPC_MAX_OUT_OBJECTS 1u

typedef struct IPC_Response {
  HLE_Service_Result result; /* becomes the response's SFCO Result */
  Service_Object out_objects[IPC_MAX_OUT_OBJECTS];
  uint32_t out_object_count; /* <= IPC_MAX_OUT_OBJECTS; svc_ipc.c opens a
                              * session per entry and allocates the real
                              * guest handles ipc_response_write serialises */
} IPC_Response;

/* Appends `object` to `res` as a handle the guest will receive. False (res
 * unchanged) if IPC_MAX_OUT_OBJECTS is already reached - callers treat
 * that as a command-handler bug, not a guest-triggerable condition, given
 * the cap above. */
bool ipc_response_add_move_object(IPC_Response *res, Service_Object object);

/* ------------------------------------------------------------------ */
/* Session table - see "KNOWN SIMPLIFICATION" above.                   */
/* ------------------------------------------------------------------ */

#define IPC_MAX_SESSIONS ((uint32_t)32)
/* §12: "Domain state (object table) lives on the session from day one,
 * even while most services are stubs." Sized arbitrarily for Phase 1 -
 * nothing populates it until a service actually converts to a domain
 * (out of scope here; see svc_ipc.h). */
#define IPC_MAX_DOMAIN_OBJECTS ((uint32_t)16)

/* 0 is never a valid handle Voland hands out (Horizon reserves it as
 * "no handle" the same way NULL is "no pointer"); distinguishable at a
 * glance from the Phase 2 table's (linear_id << 15) | index encoding. */
#define IPC_INVALID_HANDLE ((uint32_t)0)

typedef struct IPC_Session {
  Service_Object object;
  bool is_domain; /* Control-message-driven; always false until that
                   * message type is implemented (see header comment) */
  Service_Object domain_objects[IPC_MAX_DOMAIN_OBJECTS];
  uint32_t domain_object_count;
} IPC_Session;

typedef struct IPC_Session_Table {
  IPC_Session sessions[IPC_MAX_SESSIONS];
  bool in_use[IPC_MAX_SESSIONS];
} IPC_Session_Table;

void ipc_session_table_init(IPC_Session_Table *table);

/* Binds `object` to a fresh handle. IPC_INVALID_HANDLE if every slot is
 * in use (RESULT_OUT_OF_MEMORY's IPC-layer shape - svc_ipc.c maps it to
 * the kernel's own out-of-handles Result). */
uint32_t ipc_session_table_open(IPC_Session_Table *table, Service_Object object);

/* NULL if `handle` is IPC_INVALID_HANDLE or not currently open. */
IPC_Session *ipc_session_table_get(IPC_Session_Table *table, uint32_t handle);

/* Closes `handle`. False (table unchanged) if it was not open. */
bool ipc_session_table_close(IPC_Session_Table *table, uint32_t handle);

/* ------------------------------------------------------------------ */
/* Protocol functions. Pure: no vmm, no TLS, no session table. Operate   */
/* on a caller-owned command-buffer-sized copy (svc_ipc.c owns the real   */
/* vmm_read_block/vmm_write_block trip) - directly unit-testable per      */
/* CLAUDE.md's SVC-path-test mandate without a CPU backend at all.        */
/* ------------------------------------------------------------------ */

/* Parses a HIPC+CMIF request out of `buffer`
 * (TLS_IPC_COMMAND_BUFFER_BYTES bytes). See header comment for the exact
 * supported subset.
 *   RESULT_INVALID_ARGUMENT  NULL buffer/out, or buffer_size !=
 *                            TLS_IPC_COMMAND_BUFFER_BYTES
 *   RESULT_NOT_IMPLEMENTED   type != IPC_MESSAGE_TYPE_REQUEST, or any
 *                            X/A/B/C descriptor count, or any request-
 *                            direction copy/move handle count, is nonzero
 *   RESULT_MEMORY_FAULT      data_size as declared by the header would
 *                            read past `buffer_size` (a malformed guest
 *                            message, not a vmm fault - name kept for the
 *                            same "out of bounds" meaning) */
Error ipc_request_parse(const uint8_t *buffer, uint32_t buffer_size, IPC_Request *out);

/* Dispatches `req` to `object->interface`'s command table (bsearch on
 * command_id). Unknown command id: `res->result` set to
 * HLE_RESULT_NOT_IMPLEMENTED, out_object_count 0 - the caller (svc_ipc.c)
 * is the one with trace-buffer/log access, per §12's unimplemented-
 * surface policy, so logging happens there, not here. */
void ipc_dispatch(Service_Object *object, const IPC_Request *req, IPC_Response *res);

/* Writes `res` as a CMIF response (SFCO + Result, + out handles) into
 * `buffer` (TLS_IPC_COMMAND_BUFFER_BYTES bytes). `out_handles[i]` is the
 * real guest handle already allocated (ipc_session_table_open) for
 * `res->out_objects[i]`; this function only serialises, it never opens a
 * session itself.
 *   RESULT_INVALID_ARGUMENT  NULL buffer/res, buffer_size !=
 *                            TLS_IPC_COMMAND_BUFFER_BYTES, or
 *                            out_handle_count != res->out_object_count */
Error ipc_response_write(uint8_t *buffer, uint32_t buffer_size, const IPC_Response *res,
                         const uint32_t *out_handles, uint32_t out_handle_count);

#endif /* SWITCH_HLE_KERNEL_IPC_H */
