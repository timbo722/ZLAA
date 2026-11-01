/*
 * vi.c - "vi:u" / "vi:s" / "vi:m", the display: how finished frames reach the screen.
 *
 * The game opens a display and a layer (a window on it), then talks to the layer's
 * *BufferQueue* through IHOSBinderDriver: the Android graphics protocol the Switch's display
 * stack (nvnflinger) is based on. Each call is a binder transaction with a "parcel":
 *
 *   parcel = header (data size, data offset, objects size, objects offset), then data.
 *   Requests start with an interface token: u32 strict-mode policy, then the UTF-16 name
 *   "android.gui.IGraphicBufferProducer". Replies end with a s32 status (0 = OK).
 *
 * The frame loop: SET_PREALLOCATED_BUFFER (once per framebuffer), then DEQUEUE_BUFFER
 * (get a free slot to draw into) and QUEUE_BUFFER (show it). We have no screen yet, so
 * showing a frame just counts it and frees the slot again.
 * switchbrew.org/wiki/Display_services, Android's IGraphicBufferProducer.cpp
 */
#include "services.h"
#include "kernel.h"
#include "nvn/nvn.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 720
#define BINDER_ID 1                 /* our single layer's buffer queue */
#define LAYER_ID 1
#define MAX_SLOTS 64

enum { TX_REQUEST_BUFFER = 1, TX_SET_BUFFER_COUNT = 2, TX_DEQUEUE_BUFFER = 3, TX_DETACH_BUFFER = 4,
       TX_QUEUE_BUFFER = 7, TX_CANCEL_BUFFER = 8, TX_QUERY = 9, TX_CONNECT = 10, TX_DISCONNECT = 11,
       TX_ALLOCATE_BUFFERS = 13, TX_SET_PREALLOCATED_BUFFER = 14 };
enum { SLOT_FREE, SLOT_DEQUEUED };

static struct {
    int state, has_buffer;
    uint8_t buffer[0x200];          /* the game's NvGraphicBuffer description, as it sent it */
    uint32_t buffer_len;
} slots[MAX_SLOTS];
static uint64_t frames;
static KEvent *vsync_event, *buffer_event;

/* ---- vsync: a host thread signals the event 60 times a second ---- */
static void *vsync_thread(void *arg) {
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (kernel_wait_resumed()) clock_gettime(CLOCK_MONOTONIC, &next);   /* (the app was in the background) */
        kevent_signal(vsync_event);
        if (nvn_sync_tracing()) fprintf(stderr, "sync: %.2f vsync\n", next.tv_sec * 1e3 + next.tv_nsec / 1e6);
    }
    return NULL;
}

static void start_vsync(void) {
    if (vsync_event) return;
    vsync_event = kevent_new("vi: vsync");
    buffer_event = kevent_new("vi: buffer released");
    kevent_signal(buffer_event);    /* "a buffer is free": at the start, all of them are */
    pthread_t t;
    pthread_create(&t, NULL, vsync_thread, NULL);
    pthread_detach(t);
}

/* ================================================================ parcels */
typedef struct { const uint8_t *p; uint32_t size, at; } ParcelIn;
typedef struct { uint8_t data[0x400]; uint32_t size; } ParcelOut;

static uint32_t pin32(ParcelIn *in) {
    uint32_t v = 0;
    if (in->at + 4 <= in->size) memcpy(&v, in->p + in->at, 4);
    in->at += 4;
    return v;
}

/* Skip the interface token: strict-mode word, then a UTF-16 string (length, chars + NUL,
 * padded to 4 bytes). */
static void skip_interface_token(ParcelIn *in) {
    pin32(in);
    uint32_t len = pin32(in);
    in->at += ((len + 1) * 2 + 3) & ~3u;
}

static void pout(ParcelOut *out, const void *p, uint32_t n) {
    if (out->size + n > sizeof out->data) { fprintf(stderr, "vi: reply parcel too big\n"); exit(1); }
    memcpy(out->data + out->size, p, n);
    out->size += (n + 3) & ~3u;                 /* parcels keep everything 4-byte aligned */
}
static void pout32(ParcelOut *out, uint32_t v) { pout(out, &v, 4); }

/* A "flattened object": non-null flag, length, number of file descriptors (0), the bytes */
static void pout_flattened(ParcelOut *out, const void *p, uint32_t n) {
    pout32(out, 1);
    pout32(out, n);
    pout32(out, 0);
    pout(out, p, n);
}

/* NvMultiFence: how many fences, then 4 x (syncpoint id, value). Zero fences = "ready now". */
static void pout_no_fence(ParcelOut *out) {
    uint8_t fence[0x24] = { 0 };
    pout_flattened(out, fence, sizeof fence);
}

/* QueueBufferOutput: width, height, transform hint, buffers still queued */
static void pout_queue_output(ParcelOut *out) {
    pout32(out, DISPLAY_WIDTH);
    pout32(out, DISPLAY_HEIGHT);
    pout32(out, 0);
    pout32(out, 0);
}

static int transact(uint32_t code, ParcelIn *in, ParcelOut *out) {
    skip_interface_token(in);
    switch (code) {
    case TX_SET_PREALLOCATED_BUFFER: {          /* slot, then the buffer (flattened) */
        uint32_t slot = pin32(in), nonnull = pin32(in), len = pin32(in);
        pin32(in);                              /* fd count */
        if (slot >= MAX_SLOTS || len > sizeof slots[0].buffer) return -22;
        slots[slot].has_buffer = nonnull != 0;
        slots[slot].buffer_len = len;
        if (in->at + len <= in->size) memcpy(slots[slot].buffer, in->p + in->at, len);
        slots[slot].state = SLOT_FREE;
        if (ipc_trace) fprintf(stderr, "vi: buffer for slot %u (%u bytes)\n", slot, len);
        return 0;
    }
    case TX_REQUEST_BUFFER: {                   /* slot -> the buffer we were given for it */
        uint32_t slot = pin32(in);
        if (slot >= MAX_SLOTS || !slots[slot].has_buffer) { pout32(out, 0); return -22; }
        pout_flattened(out, slots[slot].buffer, slots[slot].buffer_len);
        return 0;
    }
    case TX_DEQUEUE_BUFFER: {                   /* async, w, h, format, usage -> slot, fence */
        for (int k = 0; k < MAX_SLOTS; k++) {
            if (slots[k].has_buffer && slots[k].state == SLOT_FREE) {
                slots[k].state = SLOT_DEQUEUED;
                pout32(out, (uint32_t)k);
                pout_no_fence(out);
                return 0;
            }
        }
        fprintf(stderr, "vi: DEQUEUE_BUFFER with no free buffer\n");
        pout32(out, ~0u);
        return -11;                             /* -EAGAIN: "would block" */
    }
    case TX_QUEUE_BUFFER: {                     /* slot, QueueBufferInput -> QueueBufferOutput */
        uint32_t slot = pin32(in);
        if (slot >= MAX_SLOTS || slots[slot].state != SLOT_DEQUEUED) return -22;
        /* No screen yet: the frame is "shown" at once and its buffer is free again */
        slots[slot].state = SLOT_FREE;
        kevent_signal(buffer_event);
        frames++;
        if (frames == 1 || frames % 300 == 0) fprintf(stderr, "vi: frame %" PRIu64 " presented\n", frames);
        pout_queue_output(out);
        return 0;
    }
    case TX_CANCEL_BUFFER: {
        uint32_t slot = pin32(in);
        if (slot < MAX_SLOTS) slots[slot].state = SLOT_FREE;
        kevent_signal(buffer_event);
        return 0;
    }
    case TX_QUERY: {                            /* what -> value */
        uint32_t what = pin32(in), v = 0;
        switch (what) {
        case 0: case 6: v = DISPLAY_WIDTH; break;     /* (default) width */
        case 1: case 7: v = DISPLAY_HEIGHT; break;    /* (default) height */
        case 2: v = 1; break;                         /* format: RGBA8888 */
        case 3: v = 1; break;                         /* min undequeued buffers */
        default: v = 0; break;
        }
        pout32(out, v);
        return 0;
    }
    case TX_CONNECT:                            /* listener?, api, controlled by app -> output */
        pout_queue_output(out);
        return 0;
    case TX_DISCONNECT: case TX_SET_BUFFER_COUNT: case TX_DETACH_BUFFER: case TX_ALLOCATE_BUFFERS:
        return 0;
    }
    fprintf(stderr, "\nvi: binder transaction %u is not implemented yet\n", code);
    kernel_dump_state();
    exit(1);
}

/* ---- IHOSBinderDriver ---- */
static void binder_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: case 3: {                           /* TransactParcel(Auto)(id, code, in parcel, out parcel, flags) */
        uint32_t code = ipc_in32(call, 4);
        IpcBuffer ib = ipc_in_buffer(call, 0), ob = ipc_out_buffer(call, 0);
        const uint8_t *ip = ipc_guest_ptr(ib.addr, ib.size);
        uint8_t *op = ipc_guest_ptr(ob.addr, ob.size);
        if (!ip || ib.size < 16) { call->result = MAKE_RESULT(114, 1); break; }
        uint32_t data_size, data_off;
        memcpy(&data_size, ip, 4);
        memcpy(&data_off, ip + 4, 4);
        if (data_off > ib.size || data_size > ib.size - data_off) { call->result = MAKE_RESULT(114, 1); break; }
        ParcelIn in = { ip + data_off, data_size, 0 };
        static ParcelOut out;
        out.size = 0;
        int32_t status = transact(code, &in, &out);
        if (ipc_trace && code != TX_DEQUEUE_BUFFER && code != TX_QUEUE_BUFFER)
            fprintf(stderr, "vi: transaction %u -> status %d\n", code, status);
        pout32(&out, (uint32_t)status);
        /* reply parcel: header, then the data */
        uint32_t header[4] = { out.size, 0x10, 0, 0x10 + out.size };
        if (!op || ob.size < 0x10 + out.size) { call->result = MAKE_RESULT(114, 1); break; }
        memcpy(op, header, 16);
        memcpy(op + 16, out.data, out.size);
        break;
    }
    case 1:                                     /* AdjustRefcount */
        break;
    case 2:                                     /* GetNativeHandle(id, type) -> "buffer released" event */
        ipc_out_handle(call, kevent_readable_handle(buffer_event), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType binder_type = { "vi:IHOSBinderDriver", binder_dispatch, NULL };

/* The "native window" parcel the game gets for a layer: a binder object naming our buffer
 * queue. A parcel that carries a binder object also lists where it is, in the objects table
 * after the data (one u32: offset 0); Android's readStrongBinder checks that. */
static uint64_t write_native_window(IpcCall *call) {
    IpcBuffer b = ipc_out_buffer(call, 0);
    uint8_t *p = ipc_guest_ptr(b.addr, b.size);
    uint32_t parcel[4 + 10 + 1] = { 0x28, 0x10, 4, 0x38,         /* header: data size/offset, objects size/offset */
                                    2, 1, BINDER_ID, 0, 0, 0,    /* type, flags, binder id, pad */
                                    0x70736964, 0x00767264, 0, 0, /* "dispdrv\0" (service name), pad */
                                    0 };                          /* objects: binder object at data offset 0 */
    if (ipc_trace) fprintf(stderr, "vi: native window parcel -> buffer 0x%" PRIx64 " size 0x%" PRIx64 " (b %d c %d)\n",
                           b.addr, b.size, call->n_b, call->n_c);
    if (!p || b.size < sizeof parcel) return 0;
    memcpy(p, parcel, sizeof parcel);
    return sizeof parcel;
}

/* ---- IManagerDisplayService, ISystemDisplayService: layer housekeeping ---- */
static void manager_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 2010:                                  /* CreateManagedLayer -> layer id */
        ipc_out64(call, LAYER_ID);
        break;
    case 2011: case 6000: case 6002:            /* DestroyManagedLayer, AddToLayerStack, SetLayerVisibility */
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType manager_type = { "vi:IManagerDisplayService", manager_dispatch, NULL };

static void system_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1200:                                  /* GetZOrderCountMin */
        ipc_out64(call, 0);
        break;
    case 1202:                                  /* GetZOrderCountMax */
        ipc_out64(call, 255);
        break;
    case 2205: case 2207:                       /* SetLayerZ, SetLayerVisibility */
        break;
    case 3200: {                                /* GetDisplayMode -> width, height, refresh rate, unknown */
        float hz = 60.0f;
        ipc_out32(call, DISPLAY_WIDTH);
        ipc_out32(call, DISPLAY_HEIGHT);
        ipc_out_bytes(call, &hz, 4);
        ipc_out32(call, 0);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType system_type = { "vi:ISystemDisplayService", system_dispatch, NULL };

/* ---- IApplicationDisplayService ---- */
static void app_display_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 100: case 103:                         /* GetRelayService, GetIndirectDisplayTransactionService */
        ipc_out_object(call, ipc_new_object(&binder_type, NULL));
        break;
    case 101:                                   /* GetSystemDisplayService */
        ipc_out_object(call, ipc_new_object(&system_type, NULL));
        break;
    case 102:                                   /* GetManagerDisplayService */
        ipc_out_object(call, ipc_new_object(&manager_type, NULL));
        break;
    case 1000: {                                /* ListDisplays -> count; DisplayInfo (0x60 bytes) in B buffer */
        IpcBuffer b = ipc_out_buffer(call, 0);
        uint8_t *p = ipc_guest_ptr(b.addr, 0x60);
        if (p && b.size >= 0x60) {
            uint64_t layers = 1, w = DISPLAY_WIDTH, h = DISPLAY_HEIGHT;
            memset(p, 0, 0x60);
            strcpy((char *)p, "Default");
            p[0x40] = 1;                        /* has a layer limit */
            memcpy(p + 0x48, &layers, 8);
            memcpy(p + 0x50, &w, 8);
            memcpy(p + 0x58, &h, 8);
        }
        ipc_out64(call, 1);
        break;
    }
    case 1010:                                  /* OpenDisplay(name) -> display id */
    case 1011:                                  /* OpenDefaultDisplay -> display id */
        ipc_out64(call, 1);
        break;
    case 1020: case 1101:                       /* CloseDisplay, SetDisplayEnabled */
    case 2021: case 2031:                       /* CloseLayer, DestroyStrayLayer */
    case 2101:                                  /* SetLayerScalingMode */
        break;
    case 1102:                                  /* GetDisplayResolution -> i64 width, height */
        ipc_out64(call, DISPLAY_WIDTH);
        ipc_out64(call, DISPLAY_HEIGHT);
        break;
    case 2020:                                  /* OpenLayer(display name, layer id, aruid) -> parcel size */
        ipc_out64(call, write_native_window(call));
        break;
    case 2030:                                  /* CreateStrayLayer(flags, display) -> layer id, parcel size */
        ipc_out64(call, LAYER_ID);
        ipc_out64(call, write_native_window(call));
        break;
    case 2102:                                  /* ConvertScalingMode(mode) -> u64 */
        ipc_out64(call, ipc_in32(call, 0));
        break;
    case 5202:                                  /* GetDisplayVsyncEvent(display) */
        ipc_out_handle(call, kevent_readable_handle(vsync_event), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType app_display_type = { "vi:IApplicationDisplayService", app_display_dispatch, NULL };

/* vi:u cmd 0, vi:s cmd 1, vi:m cmd 2: GetDisplayService(policy) */
static void vi_dispatch(ServiceObject *self, IpcCall *call) {
    if (call->cmd <= 2) {
        ipc_out_object(call, ipc_new_object(&app_display_type, NULL));
        return;
    }
    ipc_unknown_command(self, call);
}
static const ServiceType vi_type = { "vi", vi_dispatch, NULL };

ServiceObject *service_vi_create(void) {
    start_vsync();
    return ipc_new_object(&vi_type, NULL);
}
