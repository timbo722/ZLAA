/*
 * ipc.h - talking to OS services the way a Switch game does.
 *
 * A game uses a service (filesystem, controllers, display...) through a *session*: a
 * handle it got from sm: ("service manager"). To call a command, it writes a message into
 * its thread-local storage and calls svcSendSyncRequest(session). The service handles it
 * and writes the reply into the same 0x100 bytes. Here the "service" is C code that runs
 * right there, inside the SVC.
 *
 * Message = HIPC header (sizes; handles and buffers passed along) + CMIF payload ("SFCI",
 * command id, arguments). Format: switchbrew.org/wiki/IPC_Marshalling.
 *
 * A *domain* lets one session carry many objects (each a small integer id) instead of
 * one session per object; games switch to it to save handles. Both kinds work here.
 */
#ifndef IPC_H
#define IPC_H

#include "recomp_runtime.h"
#include <stdint.h>

#define IPC_MAX_BUFS 8
#define IPC_MAX_HANDLES 8

/* Results services return: (description << 9) | module */
#define MAKE_RESULT(module, desc) (((desc) << 9) | (module))
#define RESULT_NOT_IMPLEMENTED MAKE_RESULT(10, 1)   /* "cmif: unknown command" style */

typedef struct ServiceObject ServiceObject;
typedef struct IpcCall IpcCall;

/* What a service is: a name for logs, and a function that runs one command. */
typedef struct {
    const char *name;
    void (*dispatch)(ServiceObject *self, IpcCall *call);
    void (*destroy)(ServiceObject *self);   /* optional: free `state` when the last reference closes */
} ServiceType;

/* One live object: a session's main object, or one of the objects in a domain. */
struct ServiceObject {
    const ServiceType *type;
    void *state;           /* the service's own data, if it needs any */
    int refs;
};

typedef struct { uint64_t addr, size; } IpcBuffer;

/* One request being handled. Services read the in_* fields and use the ipc_* helpers
 * below to fill in the reply. */
struct IpcCall {
    CPU *cpu;
    uint32_t cmd;                      /* command id */
    uint64_t pid;                      /* sender's process id, if it sent one */

    /* input */
    const uint8_t *in;                 /* argument bytes after the CMIF header */
    uint32_t in_size;
    uint32_t copy_handles[IPC_MAX_HANDLES], move_handles[IPC_MAX_HANDLES];
    int n_copy, n_move;
    ServiceObject *in_objects[IPC_MAX_HANDLES];   /* objects passed in (domains only) */
    int n_in_objects;
    IpcBuffer x[IPC_MAX_BUFS], a[IPC_MAX_BUFS], b[IPC_MAX_BUFS], c[IPC_MAX_BUFS];
    int n_x, n_a, n_b, n_c;            /* X = small in, A = in, B = out, C = small out ("pointer") */

    /* output, filled by the service */
    uint32_t result;
    uint8_t out[0xC0];
    uint32_t out_size;
    uint32_t out_copy[IPC_MAX_HANDLES], out_move[IPC_MAX_HANDLES];
    int n_out_copy, n_out_move;
    ServiceObject *out_objects[IPC_MAX_HANDLES];
    int n_out_objects;
    int in_domain;
};

/* ---- helpers for services ---- */
/* Argument at byte offset `off` of the input (0 if the game sent fewer bytes). */
uint64_t ipc_in64(const IpcCall *call, uint32_t off);
uint32_t ipc_in32(const IpcCall *call, uint32_t off);
/* Append a value to the reply (each aligned to its own size, like the C struct the game reads). */
void ipc_out32(IpcCall *call, uint32_t v);
void ipc_out64(IpcCall *call, uint64_t v);
void ipc_out_bytes(IpcCall *call, const void *p, uint32_t n);
void ipc_out_handle(IpcCall *call, uint32_t handle, int move);  /* copy (0) or move (1) */
/* Return a new object: a domain gets an object id, a plain session gets a new session handle. */
void ipc_out_object(IpcCall *call, ServiceObject *obj);
/* Any buffer the game passed for output (B, or C for "pointer" outputs): where and how big. */
IpcBuffer ipc_out_buffer(const IpcCall *call, int index);
IpcBuffer ipc_in_buffer(const IpcCall *call, int index);       /* A, or X */

/* Host pointer to guest memory [addr, addr+size), or NULL if that's outside guest memory. */
void *ipc_guest_ptr(uint64_t addr, uint64_t size);

/* Make a new object of a type (refs = 0 until something holds it). */
ServiceObject *ipc_new_object(const ServiceType *type, void *state);
/* Called by a service for a command it doesn't know: logs it and stops the program. */
void ipc_unknown_command(ServiceObject *self, IpcCall *call);

/* ---- used by svc.c ---- */
uint32_t ipc_connect_named_port(const char *name, uint32_t *out_handle);   /* result */
uint32_t ipc_send_sync_request(CPU *c, uint32_t handle, uint64_t msg_addr); /* result */
void ipc_close_session(void *session);

extern int ipc_trace;   /* LAA_TRACE_IPC */

#endif
