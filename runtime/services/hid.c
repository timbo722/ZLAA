/*
 * hid.c - "hid", controllers and touch. The game reads input from a 0x40000-byte shared
 * memory block that the service keeps writing to; commands only switch devices on and say
 * which controller styles the game supports.
 *
 * A host thread fills it 200 times a second from input.c (keyboard/gamepad/script): we show a
 * Pro Controller as player 1 and the console's attached Joy-Cons ("handheld"), both reporting
 * the same buttons. switchbrew.org/wiki/HID_services, switchbrew.org/wiki/HID_Shared_Memory
 */
#include "services.h"
#include "kernel.h"
#include "input.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HID_SHARED_MEMORY_SIZE 0x40000

/* Shared memory layout: 10 controllers ("npads") of 0x5000 bytes from 0x9A00. In each:
 * style (u32), Joy-Con assignment (u32), colours, then input histories ("LIFOs") per style:
 * a 0x20-byte header {-, buffer count, newest index, count} and 17 entries of
 * {sampling number, NpadCommonState: sampling, buttons, left stick, right stick, attributes}. */
#define NPAD_BASE         0x9A00
#define NPAD_SIZE         0x5000
#define NPAD_LIFO_FULLKEY 0x28
#define NPAD_LIFO_HANDHELD 0x378
#define NPAD_DEVICE_TYPE  0x4188
#define NPAD_SYSTEM_PROPS 0x4190
#define NPAD_BATTERY      0x419C
#define LIFO_ENTRIES      17
#define LIFO_ENTRY_SIZE   0x30
enum { NPAD_PLAYER1 = 0, NPAD_HANDHELD = 8 };
enum { STYLE_FULLKEY = 1, STYLE_HANDHELD = 2 };
enum { ATTR_CONNECTED = 1, ATTR_WIRED = 2, ATTR_LEFT_CONNECTED = 4, ATTR_LEFT_WIRED = 8, ATTR_RIGHT_CONNECTED = 16,
       ATTR_RIGHT_WIRED = 32 };

static KSharedMemory *hid_shm;
static uint32_t supported_styles;
static uint32_t joy_hold_type;
static KEvent *style_set_update_event[10];
static volatile int disconnected[10];               /* the game disconnected this npad */
static volatile int connect_request;                /* hid_connect_controllers was called */

/* npad id -> its index in shared memory: players 1-8 are 0-7, handheld (0x20) 8, other (0x10) 9 */
static int npad_index(uint32_t id) { return id < 8 ? (int)id : id == 0x20 ? 8 : id == 0x10 ? 9 : -1; }

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* Add one sample to a LIFO: write the entry first, then publish it by moving the index */
static void lifo_write(uint8_t *lifo, uint64_t sampling, const InputState *s, uint32_t attributes) {
    uint64_t index, count;
    memcpy(&index, lifo + 0x10, 8);
    memcpy(&count, lifo + 0x18, 8);
    index = (index + 1) % LIFO_ENTRIES;
    uint8_t *e = lifo + 0x20 + index * LIFO_ENTRY_SIZE;
    uint64_t buttons = s->buttons;
    /* the stick's direction also shows as buttons, past about half way */
    if (s->lx < -16384) buttons |= 1 << 16;
    if (s->ly > 16384) buttons |= 1 << 17;
    if (s->lx > 16384) buttons |= 1 << 18;
    if (s->ly < -16384) buttons |= 1 << 19;
    if (s->rx < -16384) buttons |= 1 << 20;
    if (s->ry > 16384) buttons |= 1 << 21;
    if (s->rx > 16384) buttons |= 1 << 22;
    if (s->ry < -16384) buttons |= 1 << 23;
    put64(e, sampling);
    put64(e + 0x08, sampling);
    put64(e + 0x10, buttons);
    put32(e + 0x18, (uint32_t)s->lx); put32(e + 0x1C, (uint32_t)s->ly);
    put32(e + 0x20, (uint32_t)s->rx); put32(e + 0x24, (uint32_t)s->ry);
    put32(e + 0x28, attributes);
    put64(lifo + 0x08, LIFO_ENTRIES);
    __atomic_store_n((uint64_t *)(lifo + 0x10), index, __ATOMIC_RELEASE);
    if (count < LIFO_ENTRIES - 1) __atomic_store_n((uint64_t *)(lifo + 0x18), count + 1, __ATOMIC_RELEASE);
}

static void npad_setup(uint8_t *npad, uint32_t style, uint32_t device_type) {
    put32(npad, style);
    put32(npad + 0x04, 0);                               /* Joy-Cons assigned as a pair */
    put32(npad + 0x08, 0);                               /* colours valid: dark grey body, white buttons */
    put32(npad + 0x0C, 0xFF323232); put32(npad + 0x10, 0xFFFFFFFF);
    put32(npad + 0x14, 0);
    put32(npad + 0x18, 0xFF323232); put32(npad + 0x1C, 0xFFFFFFFF);
    put32(npad + 0x20, 0xFF323232); put32(npad + 0x24, 0xFFFFFFFF);
    put32(npad + NPAD_DEVICE_TYPE, device_type);
    /* powered, A/B/X/Y layout, +, - and the d-pad available */
    put64(npad + NPAD_SYSTEM_PROPS, 1 << 3 | 1 << 4 | 1 << 5 | 1 << 11 | 1 << 13 | 1 << 14 | 1 << 15);
    for (int i = 0; i < 3; i++) put32(npad + NPAD_BATTERY + 4 * i, 4);   /* full */
}

static void *input_thread(void *arg) {
    (void)arg;
    uint8_t *mem = hid_shm->host;
    npad_setup(mem + NPAD_BASE + NPAD_PLAYER1 * NPAD_SIZE, STYLE_FULLKEY, 1);
    npad_setup(mem + NPAD_BASE + NPAD_HANDHELD * NPAD_SIZE, STYLE_HANDHELD, 4 | 8);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (uint64_t sampling = 1;; sampling++) {
        InputState s = input_get();
        /* a disconnected controller comes back when one of its buttons is pressed, as on hardware,
         * or when the controller applet connects it */
        int request = connect_request;
        connect_request = 0;
        if ((s.buttons || request) && (disconnected[NPAD_PLAYER1] || disconnected[NPAD_HANDHELD])) {
            if (disconnected[NPAD_PLAYER1]) npad_setup(mem + NPAD_BASE + NPAD_PLAYER1 * NPAD_SIZE, STYLE_FULLKEY, 1);
            if (disconnected[NPAD_HANDHELD]) npad_setup(mem + NPAD_BASE + NPAD_HANDHELD * NPAD_SIZE, STYLE_HANDHELD, 4 | 8);
            disconnected[NPAD_PLAYER1] = disconnected[NPAD_HANDHELD] = 0;
            for (int i = 0; i < 10; i++) if (style_set_update_event[i]) kevent_signal(style_set_update_event[i]);
            fprintf(stderr, "hid: controllers reconnected by %s\n", request ? "the controller applet" : "a button press");
        }
        if (!disconnected[0])
            lifo_write(mem + NPAD_BASE + NPAD_PLAYER1 * NPAD_SIZE + NPAD_LIFO_FULLKEY, sampling, &s,
                       ATTR_CONNECTED | ATTR_WIRED);
        if (!disconnected[NPAD_HANDHELD])
            lifo_write(mem + NPAD_BASE + NPAD_HANDHELD * NPAD_SIZE + NPAD_LIFO_HANDHELD, sampling, &s,
                       ATTR_CONNECTED | ATTR_WIRED | ATTR_LEFT_CONNECTED | ATTR_LEFT_WIRED | ATTR_RIGHT_CONNECTED | ATTR_RIGHT_WIRED);
        next.tv_nsec += 5000000;                         /* 200 Hz */
        if (next.tv_nsec >= 1000000000) { next.tv_sec++; next.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

void hid_connect_controllers(void) { connect_request = 1; }

static void applet_resource_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                       /* GetSharedMemoryHandle */
        ipc_out_handle(call, kshm_handle(hid_shm), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType applet_resource_type = { "hid:IAppletResource", applet_resource_dispatch, NULL };

/* ---- vibration: accepted and ignored for now ---- */
static void vibration_list_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                       /* InitializeVibrationDevice */
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType vibration_list_type = { "hid:IActiveVibrationDeviceList", vibration_list_dispatch, NULL };

static void hid_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                       /* CreateAppletResource(aruid) */
        ipc_out_object(call, ipc_new_object(&applet_resource_type, NULL));
        break;
    case 1: case 11: case 21: case 31:            /* Activate DebugPad / TouchScreen / Mouse / Keyboard */
    case 40: case 41:                             /* AcquireXpad..., ActivateXpad */
    case 66: case 67:                             /* Start/StopSixAxisSensor */
    case 79: case 80:                             /* SetGyroscopeZeroDriftMode... */
    case 82:                                      /* IsSixAxisSensorAtRest (stub) */
    case 103: case 104:                           /* ActivateNpad, DeactivateNpad */
    case 102: {                                   /* SetSupportedNpadIdType(list) */
        IpcBuffer b = ipc_in_buffer(call, 0);
        const uint32_t *ids = ipc_guest_ptr(b.addr, b.size);
        fprintf(stderr, "hid: supported npad ids:");
        for (uint64_t i = 0; ids && i < b.size / 4; i++) fprintf(stderr, " %#x", ids[i]);
        fprintf(stderr, "\n");
        break;
    }
    case 109:                                     /* ActivateNpadWithRevision */
    case 122: case 123: case 124:                 /* SetNpadJoyAssignmentMode Single/Dual */
    case 125:                                     /* MergeSingleJoyAsDualJoy */
    case 128:                                     /* SetNpadHandheldActivationMode */
    case 201: case 206:                           /* SendVibrationValue(s) */
    case 1000:                                    /* SetNpadCommunicationMode */
        break;
    case 100:                                     /* SetSupportedNpadStyleSet(styles) */
        supported_styles = ipc_in32(call, 0);
        fprintf(stderr, "hid: supported styles %#x\n", supported_styles);
        break;
    case 101:                                     /* GetSupportedNpadStyleSet -> styles */
        ipc_out32(call, supported_styles);
        break;
    case 106:                                     /* AcquireNpadStyleSetUpdateEventHandle */
    {
        /* one event per controller */
        int i = npad_index(ipc_in32(call, 0));
        if (i < 0) { ipc_out_handle(call, 0, 0); break; }
        if (!style_set_update_event[i]) {
            style_set_update_event[i] = kevent_new("hid: npad style set update");
            kevent_signal(style_set_update_event[i]);    /* our controllers are already connected */
        }
        ipc_out_handle(call, kevent_readable_handle(style_set_update_event[i]), 0);
    }
        break;
    case 107: {                                   /* DisconnectNpad(npad id, aruid) */
        int i = npad_index(ipc_in32(call, 0));
        if (i < 0 || disconnected[i]) break;
        disconnected[i] = 1;
        uint8_t *npad = hid_shm->host + NPAD_BASE + (size_t)i * NPAD_SIZE;
        if (i == NPAD_PLAYER1 || i == NPAD_HANDHELD) {
            put32(npad, 0);                               /* no style, no device */
            put32(npad + NPAD_DEVICE_TYPE, 0);
        }
        if (style_set_update_event[i]) kevent_signal(style_set_update_event[i]);
        fprintf(stderr, "hid: the game disconnected npad %d\n", i);
        break;
    }
    case 108:                                     /* GetPlayerLedPattern -> u64 */
        ipc_out64(call, 1);
        break;
    case 120:                                     /* SetNpadJoyHoldType */
        joy_hold_type = (uint32_t)ipc_in64(call, 8);
        break;
    case 121:                                     /* GetNpadJoyHoldType -> u64 */
        ipc_out64(call, joy_hold_type);
        break;
    case 200:                                     /* GetVibrationDeviceInfo -> type, position */
        ipc_out32(call, 0);
        ipc_out32(call, 0);
        break;
    case 203:                                     /* CreateActiveVibrationDeviceList */
        ipc_out_object(call, ipc_new_object(&vibration_list_type, NULL));
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType hid_type = { "hid", hid_dispatch, NULL };

ServiceObject *service_hid_create(void) {
    if (!hid_shm) {
        hid_shm = kshm_new(HID_SHARED_MEMORY_SIZE);
        pthread_t t;
        pthread_create(&t, NULL, input_thread, NULL);
        pthread_detach(t);
    }
    return ipc_new_object(&hid_type, NULL);
}
