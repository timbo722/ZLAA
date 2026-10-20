/*
 * ipc.c - read IPC request messages, run the service's command, write the reply.
 * Layout of a message (all little-endian 32-bit words), switchbrew.org/wiki/IPC_Marshalling:
 *
 *   word 0      type | #X bufs<<16 | #A<<20 | #B<<24 | #W<<28
 *   word 1      #raw data words | C buffer mode<<10 | has handle descriptor<<31
 *   [handle descriptor: send pid, #copy handles, #move handles; then pid, handles]
 *   X descriptors (2 words each), A, B, W descriptors (3 words each)
 *   raw data, starting at the next 16-byte boundary:
 *       [domain header, 16 bytes, if the session is a domain]
 *       CMIF header "SFCI", version, command id, token; then the arguments
 *       [ids of objects passed in (domain only)]
 *   C descriptors (2 words each): where the game wants "pointer" outputs written
 */
#include "ipc.h"
#include "kernel.h"
#include "guest_memory.h"
#include "services/services.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

int ipc_trace;
/* Services are called from whichever game thread sends the request; one lock keeps two
 * threads from changing sessions and service state at the same time. */
static pthread_mutex_t ipc_lock = PTHREAD_MUTEX_INITIALIZER;

#define SFCI 0x49434653u   /* "SFCI": CMIF request */
#define SFCO 0x4F434653u   /* "SFCO": CMIF response */
#define MAX_DOMAIN_OBJECTS 64

enum { MSG_CLOSE = 2, MSG_REQUEST = 4, MSG_CONTROL = 5, MSG_REQUEST_CTX = 6, MSG_CONTROL_CTX = 7 };

/* A domain: a table of objects, numbered from 1. Cloned sessions share it. */
typedef struct {
    ServiceObject *objs[MAX_DOMAIN_OBJECTS];   /* object id k is objs[k] */
    int refs;                                  /* sessions using it */
} Domain;

/* A session: the game's end of a connection. Its object is `root`; once converted to a
 * domain, requests name an object in `domain` instead. */
typedef struct {
    ServiceObject *root;
    Domain *domain;                            /* NULL until ConvertCurrentObjectToDomain */
} Session;

/* ================================================================ objects */
ServiceObject *ipc_new_object(const ServiceType *type, void *state) {
    ServiceObject *o = calloc(1, sizeof *o);
    if (!o) { fprintf(stderr, "ipc: out of memory\n"); exit(1); }
    o->type = type;
    o->state = state;
    return o;
}

static void obj_ref(ServiceObject *o) { o->refs++; }
static void obj_unref(ServiceObject *o) {
    if (--o->refs > 0) return;
    if (o->type->destroy) o->type->destroy(o);
    free(o);
}

static uint32_t new_session(ServiceObject *root, Domain *domain) {
    Session *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->root = root;
    obj_ref(root);
    s->domain = domain;
    if (domain) domain->refs++;
    uint32_t h = khandle_new(KOBJ_SESSION, s);
    if (!h) { obj_unref(root); if (domain) domain->refs--; free(s); }
    return h;
}

void ipc_close_session(void *session) {
    Session *s = session;
    pthread_mutex_lock(&ipc_lock);
    if (s->domain && --s->domain->refs == 0) {
        for (int k = 1; k < MAX_DOMAIN_OBJECTS; k++)
            if (s->domain->objs[k]) obj_unref(s->domain->objs[k]);
        free(s->domain);
    }
    obj_unref(s->root);
    free(s);
    pthread_mutex_unlock(&ipc_lock);
}

static int domain_add(Domain *d, ServiceObject *o) {
    for (int k = 1; k < MAX_DOMAIN_OBJECTS; k++) {
        if (!d->objs[k]) { d->objs[k] = o; obj_ref(o); return k; }
    }
    fprintf(stderr, "ipc: domain is full\n");
    exit(1);
}

/* ================================================================ helpers for services */
uint64_t ipc_in64(const IpcCall *call, uint32_t off) {
    uint64_t v = 0;
    if (off + 8 <= call->in_size) memcpy(&v, call->in + off, 8);
    return v;
}
uint32_t ipc_in32(const IpcCall *call, uint32_t off) {
    uint32_t v = 0;
    if (off + 4 <= call->in_size) memcpy(&v, call->in + off, 4);
    return v;
}

static void out_raw(IpcCall *call, const void *p, uint32_t n, uint32_t align) {
    uint32_t at = (call->out_size + align - 1) & ~(align - 1);
    if (at + n > sizeof call->out) { fprintf(stderr, "ipc: reply too big\n"); exit(1); }
    memset(call->out + call->out_size, 0, at - call->out_size);
    memcpy(call->out + at, p, n);
    call->out_size = at + n;
}
void ipc_out32(IpcCall *call, uint32_t v) { out_raw(call, &v, 4, 4); }
void ipc_out64(IpcCall *call, uint64_t v) { out_raw(call, &v, 8, 8); }
void ipc_out_bytes(IpcCall *call, const void *p, uint32_t n) { out_raw(call, p, n, 1); }

void ipc_out_handle(IpcCall *call, uint32_t handle, int move) {
    if (move) call->out_move[call->n_out_move++] = handle;
    else call->out_copy[call->n_out_copy++] = handle;
}

void ipc_out_object(IpcCall *call, ServiceObject *obj) {
    if (call->in_domain) {
        call->out_objects[call->n_out_objects++] = obj;   /* numbered when the reply is written */
    } else {
        uint32_t h = new_session(obj, NULL);
        if (!h) { fprintf(stderr, "ipc: out of handles\n"); exit(1); }
        ipc_out_handle(call, h, 1);
    }
}

IpcBuffer ipc_out_buffer(const IpcCall *call, int index) {
    if (index < call->n_b && call->b[index].size) return call->b[index];
    if (index < call->n_c) return call->c[index];
    return (IpcBuffer){ 0, 0 };
}
IpcBuffer ipc_in_buffer(const IpcCall *call, int index) {
    if (index < call->n_a && call->a[index].size) return call->a[index];
    if (index < call->n_x) return call->x[index];
    return (IpcBuffer){ 0, 0 };
}

void *ipc_guest_ptr(uint64_t addr, uint64_t size) {
    if (addr > guest_mem_size || size > guest_mem_size - addr) return NULL;
    return guest_mem + addr;
}

void ipc_unknown_command(ServiceObject *self, IpcCall *call) {
    fprintf(stderr, "\nipc: %s command %u is not implemented yet (%u bytes of arguments)\n",
            self->type->name, call->cmd, call->in_size);
    kernel_dump_state();
    exit(1);
}

/* ================================================================ named ports */
uint32_t ipc_connect_named_port(const char *name, uint32_t *out_handle) {
    if (strcmp(name, "sm:") != 0) {
        fprintf(stderr, "ipc: no named port '%s'\n", name);
        return R_NOT_FOUND;
    }
    pthread_mutex_lock(&ipc_lock);
    *out_handle = new_session(service_sm_create(), NULL);
    pthread_mutex_unlock(&ipc_lock);
    return *out_handle ? R_SUCCESS : R_OUT_OF_RANGE;
}

/* ================================================================ messages */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Control messages: commands to the session itself, not to the service */
static void control(Session *s, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                   /* ConvertCurrentObjectToDomain -> object id */
        if (!s->domain) {
            s->domain = calloc(1, sizeof *s->domain);
            if (!s->domain) exit(1);
            s->domain->refs = 1;
            s->domain->objs[1] = s->root;
            obj_ref(s->root);
        }
        ipc_out32(call, 1);
        break;
    case 1: {                                 /* CopyFromCurrentDomain(id) -> session handle */
        uint32_t id = ipc_in32(call, 0);
        if (!s->domain || id == 0 || id >= MAX_DOMAIN_OBJECTS || !s->domain->objs[id]) {
            call->result = R_NOT_FOUND;
            break;
        }
        ipc_out_handle(call, new_session(s->domain->objs[id], NULL), 1);
        break;
    }
    case 2: case 4:                           /* CloneCurrentObject(Ex) -> session handle */
        ipc_out_handle(call, new_session(s->root, s->domain), 1);   /* a domain stays a domain */
        break;
    case 3:                                   /* QueryPointerBufferSize -> u16 */
        ipc_out32(call, 0x8000);
        break;
    default:
        fprintf(stderr, "ipc: control command %u not implemented\n", call->cmd);
        exit(1);
    }
}

static uint32_t send_locked(CPU *c, uint32_t handle, uint64_t msg_addr) {
    KHandle *h = khandle_get(handle);
    if (!h || h->type != KOBJ_SESSION) return R_INVALID_HANDLE;
    Session *s = h->obj;
    if (msg_addr > guest_mem_size - 0x100) return R_INVALID_ADDRESS;
    uint8_t *msg = guest_mem + msg_addr;

    /* ---- parse the request ---- */
    static __thread IpcCall call;
    memset(&call, 0, sizeof call);
    call.cpu = c;
    uint32_t w0 = rd32(msg), w1 = rd32(msg + 4);
    uint32_t type = w0 & 0xFFFF;
    int nx = (w0 >> 16) & 15, na = (w0 >> 20) & 15, nb = (w0 >> 24) & 15, nw = (w0 >> 28) & 15;
    uint32_t raw_words = w1 & 0x3FF, c_mode = (w1 >> 10) & 15;
    uint32_t off = 8;

    if (type == MSG_CLOSE) return R_SUCCESS;   /* the game closes the handle right after */

    if (w1 >> 31) {                            /* handle descriptor */
        uint32_t hd = rd32(msg + off);
        off += 4;
        call.n_copy = (hd >> 1) & 15;
        call.n_move = (hd >> 5) & 15;
        if (hd & 1) { call.pid = 0x51; off += 8; }   /* the kernel fills in the real pid: ours */
        if (call.n_copy > IPC_MAX_HANDLES || call.n_move > IPC_MAX_HANDLES) return R_OUT_OF_RANGE;
        for (int k = 0; k < call.n_copy; k++, off += 4) call.copy_handles[k] = rd32(msg + off);
        for (int k = 0; k < call.n_move; k++, off += 4) call.move_handles[k] = rd32(msg + off);
    }
    if (nx > IPC_MAX_BUFS || na > IPC_MAX_BUFS || nb > IPC_MAX_BUFS) return R_OUT_OF_RANGE;
    for (int k = 0; k < nx; k++, off += 8) {
        uint32_t d0 = rd32(msg + off), d1 = rd32(msg + off + 4);
        call.x[k].addr = d1 | ((uint64_t)((d0 >> 12) & 15) << 32) | ((uint64_t)((d0 >> 6) & 7) << 36);
        call.x[k].size = d0 >> 16;
    }
    IpcBuffer *abw[3] = { call.a, call.b, NULL };
    int counts[3] = { na, nb, nw };
    for (int t = 0; t < 3; t++) {
        for (int k = 0; k < counts[t]; k++, off += 12) {
            uint32_t d0 = rd32(msg + off), d1 = rd32(msg + off + 4), d2 = rd32(msg + off + 8);
            IpcBuffer b = { d1 | ((uint64_t)(d2 >> 28) << 32) | ((uint64_t)((d2 >> 2) & 7) << 36),
                            d0 | ((uint64_t)((d2 >> 24) & 15) << 32) };
            if (abw[t]) abw[t][k] = b;
            else { fprintf(stderr, "ipc: W (exchange) buffers not supported\n"); exit(1); }
        }
    }
    call.n_x = nx; call.n_a = na; call.n_b = nb;

    uint32_t raw_start = off;
    uint32_t payload = (off + 15) & ~15u;
    uint32_t c_off = raw_start + raw_words * 4;
    int nc = c_mode > 2 ? (int)c_mode - 2 : c_mode == 2 ? 1 : 0;
    if (nc > IPC_MAX_BUFS || c_off + nc * 8 > 0x100) return R_OUT_OF_RANGE;
    for (int k = 0; k < nc; k++) {
        uint32_t d0 = rd32(msg + c_off + 8 * k), d1 = rd32(msg + c_off + 8 * k + 4);
        call.c[k].addr = d0 | ((uint64_t)(d1 & 0xFFFF) << 32);
        call.c[k].size = d1 >> 16;
    }
    call.n_c = nc;

    /* ---- which object, which command ---- */
    ServiceObject *target = s->root;
    int is_request = type == MSG_REQUEST || type == MSG_REQUEST_CTX;
    if (!is_request && type != MSG_CONTROL && type != MSG_CONTROL_CTX) {
        fprintf(stderr, "ipc: message type %u not supported\n", type);
        exit(1);
    }
    uint32_t cmif = payload;
    if (is_request && s->domain) {
        uint8_t dcmd = msg[payload], n_in = msg[payload + 1];
        uint16_t dsize;
        memcpy(&dsize, msg + payload + 2, 2);
        uint32_t id = rd32(msg + payload + 4);
        if (id == 0 || id >= MAX_DOMAIN_OBJECTS || !s->domain->objs[id]) {
            fprintf(stderr, "ipc: domain object %u does not exist\n", id);
            return R_INVALID_HANDLE;
        }
        if (dcmd == 2) {                       /* CloseVirtualHandle: drop the object */
            obj_unref(s->domain->objs[id]);
            s->domain->objs[id] = NULL;
            call.result = R_SUCCESS;
            goto reply;
        }
        target = s->domain->objs[id];
        call.in_domain = 1;
        cmif = payload + 0x10;
        for (int k = 0; k < n_in && k < IPC_MAX_HANDLES; k++) {
            uint32_t oid = rd32(msg + cmif + dsize + 4 * k);
            call.in_objects[k] = oid < MAX_DOMAIN_OBJECTS ? s->domain->objs[oid] : NULL;
            call.n_in_objects++;
        }
    }
    if (rd32(msg + cmif) != SFCI) {
        fprintf(stderr, "ipc: request to %s has no SFCI header (message format not understood)\n",
                target->type->name);
        kernel_dump_state();
        exit(1);
    }
    call.cmd = rd32(msg + cmif + 8);
    call.in = msg + cmif + 16;
    uint32_t end = raw_start + raw_words * 4;
    call.in_size = end > cmif + 16 ? end - (cmif + 16) : 0;

    if (ipc_trace)
        fprintf(stderr, "ipc: %s%s cmd %u\n", type >= MSG_CONTROL && !is_request ? "control " : "",
                is_request ? target->type->name : "", call.cmd);
    if (is_request) target->type->dispatch(target, &call);
    else control(s, &call);

    /* ---- write the reply over the request ---- */
reply:;
    uint8_t buf[0x100];
    memset(buf, 0, sizeof buf);
    off = 8;
    int has_handles = call.n_out_copy || call.n_out_move;
    if (has_handles) {
        wr32(buf + off, (uint32_t)(call.n_out_copy << 1 | call.n_out_move << 5));
        off += 4;
        for (int k = 0; k < call.n_out_copy; k++, off += 4) wr32(buf + off, call.out_copy[k]);
        for (int k = 0; k < call.n_out_move; k++, off += 4) wr32(buf + off, call.out_move[k]);
    }
    uint32_t p = (off + 15) & ~15u;
    if (call.in_domain || (s->domain && is_request)) {
        wr32(buf + p, (uint32_t)call.n_out_objects);   /* domain reply header */
        p += 16;
    }
    wr32(buf + p, SFCO);
    wr32(buf + p + 8, call.result);
    p += 16;
    if (p + call.out_size + 4 * call.n_out_objects > sizeof buf) { fprintf(stderr, "ipc: reply too big\n"); exit(1); }
    memcpy(buf + p, call.out, call.out_size);
    p += call.out_size;
    for (int k = 0; k < call.n_out_objects; k++, p += 4)
        wr32(buf + p, (uint32_t)domain_add(s->domain, call.out_objects[k]));
    wr32(buf, 0);
    /* raw data size counts from the end of the handles: the alignment padding, then the data */
    wr32(buf + 4, (p - off + 3) / 4 | (has_handles ? 1u << 31 : 0));
    memcpy(msg, buf, sizeof buf);
    return R_SUCCESS;
}

uint32_t ipc_send_sync_request(CPU *c, uint32_t handle, uint64_t msg_addr) {
    pthread_mutex_lock(&ipc_lock);
    uint32_t r = send_locked(c, handle, msg_addr);
    pthread_mutex_unlock(&ipc_lock);
    return r;
}
