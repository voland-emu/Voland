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
 * Wire format. Verified against libnx's sf/hipc.h, sf/cmif.h, sf/tipc.h
 * and sf/service.h (switchbrew/libnx b156b47, October 2026) - the client
 * every homebrew title links - rather than against switchbrew prose
 * alone. Every field position and mask is a named constant below, and the
 * tests build their fixtures from the same constants (or as literal libnx
 * bytes), so the layout lives in exactly one place:
 *   - HIPC header (8 bytes), then the special header (4 bytes) iff
 *     word1's has_special_header bit, then the pid (8 bytes) iff the
 *     special header's send_pid bit, then copy handles, move handles, the
 *     X/A/B/W descriptors, and finally the raw data words.
 *   - The raw data section starts IMMEDIATELY after the last descriptor -
 *     not aligned - and num_data_words counts every word of it, including
 *     the alignment padding in front of the CMIF header. The CMIF header
 *     sits at the first 16-byte boundary (measured from the start of the
 *     command buffer) at or after the section start. libnx reserves a
 *     full IPC_CMIF_PADDING_BYTES for that padding in every request it
 *     sizes; responses written here do the same.
 *   - CMIF in header: magic "SFCI", version, command_id, token (4 x u32).
 *     CMIF out header: magic "SFCO", version, result, token.
 *
 * Scope for this header (Phase 1, "Minimal IPC"):
 *   - IPC_MESSAGE_TYPE_REQUEST is dispatched. IPC_MESSAGE_TYPE_CLOSE is
 *     accepted: libnx's serviceClose sends one through SendSyncRequest
 *     before every svcCloseHandle on a session it owns, so rejecting it
 *     would fail every service teardown. The other CMIF types
 *     (LegacyRequest, LegacyControl, Control and the WithContext forms -
 *     domain conversion, session cloning, pointer-buffer-size query) are
 *     recognised and rejected with RESULT_NOT_IMPLEMENTED; nothing this
 *     phase dispatches needs them, and sm: is never domain-converted.
 *   - TIPC (type IPC_MESSAGE_TYPE_TIPC_CLOSE, or IPC_MESSAGE_TYPE_TIPC_
 *     REQUEST_BASE + command id; no SFCI header) is recognised and
 *     rejected with RESULT_NOT_IMPLEMENTED - DEFERRED, not omitted. libnx
 *     talks to sm: over CMIF for RegisterClient and GetServiceHandle on
 *     every firmware (sm.c says so explicitly) and uses TIPC only for
 *     RegisterService, which Voland does not implement (sm.h). Nintendo's
 *     own SDK reaches sm: over TIPC on firmware 12+, so retail titles
 *     need TIPC before Phase 4's first boot; see DESIGN.md §12.
 *   - X/A/B/W buffer descriptors and C receive lists: rejected (count must
 *     be 0). sm:'s Phase 1 commands carry their 8-byte ServiceName in the
 *     raw data, not a buffer. The first service that needs a buffer
 *     descriptor is what pulls support for them in.
 *   - Handle descriptors in the REQUEST direction (the guest handing
 *     Voland a handle): rejected (copy/move handle counts must be 0).
 *     The send-pid bit IS parsed: RegisterClient sets it.
 *   - Handle descriptors in the RESPONSE direction: move objects only
 *     (IPC_Response::out_objects), capped at IPC_MAX_OUT_OBJECTS. Copy
 *     handles out: not produced by anything Phase 1 dispatches.
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

/* HIPC message types (libnx CmifCommandType / TipcCommandType). REQUEST
 * is dispatched and CLOSE is accepted; every other value is recognised and
 * rejected (see header comment). */
typedef enum IPC_Message_Type {
  IPC_MESSAGE_TYPE_INVALID = 0,
  IPC_MESSAGE_TYPE_LEGACY_REQUEST = 1,
  IPC_MESSAGE_TYPE_CLOSE = 2,
  IPC_MESSAGE_TYPE_LEGACY_CONTROL = 3,
  IPC_MESSAGE_TYPE_REQUEST = 4,
  IPC_MESSAGE_TYPE_CONTROL = 5,
  IPC_MESSAGE_TYPE_REQUEST_WITH_CONTEXT = 6,
  IPC_MESSAGE_TYPE_CONTROL_WITH_CONTEXT = 7,
  IPC_MESSAGE_TYPE_TIPC_CLOSE = 15,
  /* TIPC requests carry their command id in the type: base + id. */
  IPC_MESSAGE_TYPE_TIPC_REQUEST_BASE = 16,
} IPC_Message_Type;

/* ------------------------------------------------------------------ */
/* Wire layout (see header comment). Shared with the tests.           */
/* ------------------------------------------------------------------ */

#define IPC_HIPC_HEADER_BYTES 8u
#define IPC_HIPC_SPECIAL_HEADER_BYTES 4u
#define IPC_HIPC_PID_BYTES 8u
#define IPC_HIPC_HANDLE_BYTES 4u
#define IPC_HIPC_DATA_WORD_BYTES 4u

/* Header word 0. */
#define IPC_HIPC_TYPE_MASK 0xFFFFu
#define IPC_HIPC_NUM_X_SHIFT 16u /* send statics */
#define IPC_HIPC_NUM_A_SHIFT 20u /* send buffers */
#define IPC_HIPC_NUM_B_SHIFT 24u /* recv buffers */
#define IPC_HIPC_NUM_W_SHIFT 28u /* exchange buffers */
#define IPC_HIPC_DESCRIPTOR_COUNT_MASK 0xFu

/* Header word 1. */
#define IPC_HIPC_NUM_DATA_WORDS_MASK 0x3FFu
#define IPC_HIPC_RECV_STATIC_MODE_SHIFT 10u
#define IPC_HIPC_RECV_STATIC_MODE_MASK 0xFu
#define IPC_HIPC_HAS_SPECIAL_HEADER_BIT (1u << 31)

/* Special header. */
#define IPC_HIPC_SEND_PID_BIT 1u
#define IPC_HIPC_NUM_COPY_HANDLES_SHIFT 1u
#define IPC_HIPC_NUM_MOVE_HANDLES_SHIFT 5u
#define IPC_HIPC_HANDLE_COUNT_MASK 0xFu

/* CMIF header, at the first IPC_CMIF_ALIGN boundary of the raw data. */
#define IPC_CMIF_ALIGN 16u
#define IPC_CMIF_PADDING_BYTES 16u /* alignment reserve counted in num_data_words */
#define IPC_CMIF_HEADER_BYTES 16u
#define IPC_CMIF_MAGIC_OFFSET 0u
#define IPC_CMIF_VERSION_OFFSET 4u
#define IPC_CMIF_COMMAND_ID_OFFSET 8u /* request */
#define IPC_CMIF_RESULT_OFFSET 8u     /* response */
#define IPC_CMIF_TOKEN_OFFSET 12u
#define IPC_CMIF_MAGIC_SFCI 0x49434653u /* "SFCI" read little-endian */
#define IPC_CMIF_MAGIC_SFCO 0x4F434653u /* "SFCO" read little-endian */

typedef struct IPC_Request {
  IPC_Message_Type type; /* REQUEST or CLOSE; nothing else parses */
  uint32_t command_id;   /* CMIF command id; 0 for CLOSE */
  bool has_pid;          /* HIPC special header's send_pid bit */
  /* Valid iff has_pid. UNTRUSTED: these are the guest's own 8 bytes, not
   * a kernel-stamped pid - real Horizon overwrites the field in transit,
   * and libnx leaves it uninitialised. Voland has no process id until the
   * Phase 2 process object exists; until then no service may read this,
   * and the field must be overwritten in svc_ipc.c before one does. */
  uint64_t guest_pid;
  const uint8_t *data;   /* payload after the CMIF header, `data_size`
                          * bytes; points into the caller's own local copy
                          * of the command buffer (svc_ipc.c) - valid only
                          * for the parse/dispatch/write call chain's
                          * duration, never retained past it. NULL for
                          * CLOSE. */
  uint32_t data_size;    /* up to the end of the declared raw data section;
                          * may exceed the command's own payload by libnx's
                          * halfword/word rounding */
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
  /* Set by a CLOSE message: the server end is gone, so every later request
   * fails with the kernel's ConnectionClosed. The handle itself stays
   * open until CloseHandle, as on real Horizon. */
  bool server_closed;
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
 * in use; svc_ipc.c reports that to the guest as HLE_RESULT_OUT_OF_SESSIONS
 * (KernelError_OutOfSessions). */
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
 * supported subset. A CLOSE message parses to `out->type ==
 * IPC_MESSAGE_TYPE_CLOSE` with no CMIF header read.
 *   RESULT_INVALID_ARGUMENT  NULL buffer/out, or buffer_size !=
 *                            TLS_IPC_COMMAND_BUFFER_BYTES
 *   RESULT_NOT_IMPLEMENTED   type is neither REQUEST nor CLOSE (TIPC
 *                            included), any X/A/B/W descriptor count or
 *                            receive-list mode, or any request-direction
 *                            copy/move handle count, is nonzero, or the
 *                            raw data is not SFCI
 *   RESULT_MEMORY_FAULT      the declared raw data section runs past
 *                            `buffer_size`, or is too short to hold the
 *                            aligned CMIF header (a malformed guest
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
 * `buffer` (TLS_IPC_COMMAND_BUFFER_BYTES bytes). The HIPC type field is
 * written as 0: libnx's hipcParseResponse/cmifParseResponse never read
 * it. `out_handles[i]` is the
 * real guest handle already allocated (ipc_session_table_open) for
 * `res->out_objects[i]`; this function only serialises, it never opens a
 * session itself.
 *   RESULT_INVALID_ARGUMENT  NULL buffer/res, buffer_size !=
 *                            TLS_IPC_COMMAND_BUFFER_BYTES, or
 *                            out_handle_count != res->out_object_count */
Error ipc_response_write(uint8_t *buffer, uint32_t buffer_size, const IPC_Response *res,
                         const uint32_t *out_handles, uint32_t out_handle_count);

#endif /* SWITCH_HLE_KERNEL_IPC_H */
