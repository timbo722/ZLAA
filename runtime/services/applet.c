/*
 * applet.c - "appletOE", the applet manager (AM) as an application sees it.
 *
 * This is how a game talks to the system around it: is it in focus, docked or handheld,
 * which user launched it, what language to use. It is a tree of objects:
 *
 *   appletOE -> IApplicationProxy -> ICommonStateGetter    (messages, docked/handheld)
 *                                 -> ISelfController       (settings about this game)
 *                                 -> IWindowController, IAudioController, IDisplayController
 *                                 -> IApplicationFunctions (launch parameters, language...)
 *                                 -> ILibraryAppletCreator -> ILibraryAppletAccessor
 *
 * Library applets are small system programs a game opens over itself (controller setup,
 * keyboard, error screens). We have none: each one "runs" instantly in C and answers.
 *
 * switchbrew.org/wiki/Applet_Manager_services
 */
#include "services.h"
#include "kernel.h"
#include "nvn/nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AM_RESULT(desc) MAKE_RESULT(128, desc)
#define RESULT_NO_MESSAGES AM_RESULT(3)
#define RESULT_NO_DATA_IN_CHANNEL AM_RESULT(2)

/* Messages the system sends the game (AppletMessage) */
enum { MSG_FOCUS_STATE_CHANGED = 15, MSG_OPERATION_MODE_CHANGED = 30, MSG_PERFORMANCE_MODE_CHANGED = 31 };

/* Handheld for now: the game renders at 720p. (Docked = 1, 1080p.) */
#define OPERATION_MODE_HANDHELD 0
#define OPERATION_MODE_DOCKED   1
/* Docked: the game renders 1920x1080 (the Thor's screen is 1080p, and its GPU is far faster
 * than the Switch's). LAA_HANDHELD=1: handheld, 1280x720. */
static int operation_mode = -1;
int applet_handheld(void) {
    if (operation_mode < 0) operation_mode = getenv("LAA_HANDHELD") ? OPERATION_MODE_HANDHELD : OPERATION_MODE_DOCKED;
    return operation_mode == OPERATION_MODE_HANDHELD;
}

/* ---- the applet message queue: one per process ---- */
static uint32_t messages[16];
static int n_messages;
static KEvent *message_event;

static void push_message(uint32_t msg) {
    if (n_messages < 16) messages[n_messages++] = msg;
    kevent_signal(message_event);
}

static void out_new(IpcCall *call, const ServiceType *type) { ipc_out_object(call, ipc_new_object(type, NULL)); }

/* ---- IStorage: a block of bytes passed between applets (e.g. launch parameters) ---- */
typedef struct {
    uint8_t *data;
    uint64_t size;
    int refs;                                 /* the storage, its accessors, applets holding it */
    int guest;                                /* data is the game's transfer memory, not ours */
} Storage;

static void storage_unref(Storage *st) {
    if (--st->refs == 0) {
        if (!st->guest) free(st->data);
        free(st);
    }
}
static void storage_release(ServiceObject *self) { storage_unref(self->state); }

static void accessor_dispatch(ServiceObject *self, IpcCall *call) {
    Storage *st = self->state;
    switch (call->cmd) {
    case 0:                                   /* GetSize -> i64 */
        ipc_out64(call, st->size);
        break;
    case 10: case 11: {                       /* Write(offset, A buffer) / Read(offset, B buffer) */
        uint64_t off = ipc_in64(call, 0);
        IpcBuffer b = call->cmd == 10 ? ipc_in_buffer(call, 0) : ipc_out_buffer(call, 0);
        uint8_t *p = ipc_guest_ptr(b.addr, b.size);
        if (!p || off > st->size || b.size > st->size - off) { call->result = AM_RESULT(503); break; }
        if (call->cmd == 10) memcpy(st->data + off, p, b.size);
        else memcpy(p, st->data + off, b.size);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType accessor_type = { "am:IStorageAccessor", accessor_dispatch, storage_release };

static void storage_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                                 /* Open -> IStorageAccessor (shares the bytes) */
        Storage *st = self->state;
        st->refs++;
        ipc_out_object(call, ipc_new_object(&accessor_type, st));
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType storage_type = { "am:IStorage", storage_dispatch, storage_release };

static ServiceObject *new_storage(const void *data, uint64_t size) {
    Storage *st = calloc(1, sizeof *st);
    if (!st || !(st->data = calloc(1, size ? size : 1))) exit(1);
    if (data) memcpy(st->data, data, size);
    st->size = size;
    st->refs = 1;
    return ipc_new_object(&storage_type, st);
}

/* ---- library applets ----
 * The game creates one (CreateLibraryApplet), pushes its arguments as IStorages
 * (PushInData), starts it, waits for its state-changed event and pops the answer
 * (PopOutData). Ours finish inside Start. */
enum { APPLET_CONTROLLER = 0x0C, APPLET_SWKBD = 0x11 };   /* AppletId (switchbrew: Applet_Manager_services) */

/* The SDK waits for this before creating a library applet: "no other library applet runs". */
static KEvent *launchable_event;
static KEvent *library_applet_launchable(void) {
    if (!launchable_event) {
        launchable_event = kevent_new("am: library applet launchable");
        kevent_signal(launchable_event);
    }
    return launchable_event;
}

#define APPLET_QUEUE_MAX 4
typedef struct { struct { uint8_t *data; uint64_t size; } item[APPLET_QUEUE_MAX]; int n; KEvent *event; } OutQueue;

typedef struct {
    uint32_t id, mode;
    int started, completed;
    Storage *in[APPLET_QUEUE_MAX];            /* arguments the game pushed */
    int n_in;
    OutQueue out, interactive_out;            /* answers not popped yet */
    KEvent *state_changed;
    uint8_t name[0x7D4];                      /* swkbd: the text, waiting for the game's text check */
    uint32_t name_bytes;
} LibraryApplet;

static void queue_push(OutQueue *q, const void *data, uint64_t size) {
    if (q->n == APPLET_QUEUE_MAX) return;
    uint8_t *copy = malloc(size);
    if (!copy) exit(1);
    memcpy(copy, data, size);
    q->item[q->n].data = copy;
    q->item[q->n++].size = size;
    kevent_signal(q->event);
}

static void queue_pop(OutQueue *q, IpcCall *call) {
    if (q->n == 0) { call->result = RESULT_NO_DATA_IN_CHANNEL; return; }
    ipc_out_object(call, new_storage(q->item[0].data, q->item[0].size));
    free(q->item[0].data);
    memmove(q->item, q->item + 1, --q->n * sizeof q->item[0]);
    if (q->n == 0) kevent_clear(q->event);
}

static void applet_finish(LibraryApplet *a) {
    a->completed = 1;
    kevent_signal(a->state_changed);
}

/* ---- the software keyboard (swkbd) ----
 * In: CommonArguments, KeyboardConfig, a work buffer (transfer memory) holding the initial
 * text. Out: 0x7D8 bytes = u32 result (0 OK, 1 cancelled) + the text (UTF-16, or UTF-8 if
 * the config asks). We don't show a keyboard: the answer is $LAA_PLAYER_NAME, else "Link".
 * The game may ask for a "text check": it would be sent the text (interactive data) and its
 * callback would run the system's profanity filter (nn::ngc), which needs the console's
 * NgWord system archive. We have no system archives, so we skip the check (LAA_SWKBD_TEXT_CHECK=1
 * forces it, for testing).
 * KeyboardConfig offsets: yuzu (core/hle/service/am/frontend/applet_software_keyboard_types.h). */
enum { KBD_MAX_LENGTH = 0x3AC, KBD_USE_UTF8 = 0x3BD, KBD_USE_TEXT_CHECK = 0x3D0, KBD_CONFIG_MIN = 0x3D4 };
#define KBD_STRING_SIZE 0x7D4

static int utf8_to_utf16(const char *s, uint16_t *out, int max) {    /* returns code units written */
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
        if (c >= 0x10000) {
            if (n + 2 > max) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | c >> 10);
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

static void swkbd_answer(LibraryApplet *a) {          /* the final answer, then close */
    uint8_t out[4 + KBD_STRING_SIZE] = { 0 };          /* result 0 = OK */
    memcpy(out + 4, a->name, KBD_STRING_SIZE);
    queue_push(&a->out, out, sizeof out);
    applet_finish(a);
}

static void swkbd_run(LibraryApplet *a) {
    const Storage *config = a->n_in > 1 ? a->in[1] : NULL;
    if (!config || config->size < KBD_CONFIG_MIN) {
        fprintf(stderr, "am: swkbd: no keyboard config; cancelling\n");
        uint32_t cancelled = 1;
        queue_push(&a->out, &cancelled, 4);
        applet_finish(a);
        return;
    }
    uint32_t max_length;
    memcpy(&max_length, config->data + KBD_MAX_LENGTH, 4);
    if (max_length == 0 || max_length > 500) max_length = 500;
    int utf8 = config->data[KBD_USE_UTF8], text_check = config->data[KBD_USE_TEXT_CHECK];
    const char *name = getenv("LAA_PLAYER_NAME") ? getenv("LAA_PLAYER_NAME") : "Link";
    memset(a->name, 0, sizeof a->name);
    if (utf8) {
        size_t n = strlen(name);
        if (n > max_length * 4 || n > KBD_STRING_SIZE - 1) n = 0;
        memcpy(a->name, name, n);
        a->name_bytes = (uint32_t)n;
    } else {
        uint16_t text[KBD_STRING_SIZE / 2] = { 0 };
        int n = utf8_to_utf16(name, text, (int)max_length);
        memcpy(a->name, text, (size_t)n * 2);
        a->name_bytes = (uint32_t)n * 2;
    }
    if (!getenv("LAA_SWKBD_TEXT_CHECK")) text_check = 0;
    fprintf(stderr, "am: swkbd: answering \"%s\"%s\n", name, text_check ? " (game checks it first)" : "");
    if (!text_check) { swkbd_answer(a); return; }
    /* text check request: u64 size (this u64 + the text's bytes), then the text */
    uint8_t req[8 + KBD_STRING_SIZE] = { 0 };
    uint64_t size = 8 + a->name_bytes;
    memcpy(req, &size, 8);
    memcpy(req + 8, a->name, KBD_STRING_SIZE);
    queue_push(&a->interactive_out, req, sizeof req);
}

/* The game's text check verdict: u32 0 = accepted, else rejected / asks to confirm. */
static void swkbd_text_checked(LibraryApplet *a, const Storage *st) {
    uint32_t verdict = 0;
    if (st && st->size >= 4) memcpy(&verdict, st->data, 4);
    if (verdict != 0) fprintf(stderr, "am: swkbd: the game's text check said %u; answering anyway\n", verdict);
    swkbd_answer(a);
}

/* What the applet does when started. */
static void applet_run(LibraryApplet *a) {
    switch (a->id) {
    case APPLET_CONTROLLER: {
        /* "Controller support" (pick controllers, press L+R). Answer as if player 1 confirmed:
         * ControllerSupportResultInfo = { s8 player count, pad[3], u32 selected npad id, u32 result }. */
        uint8_t info[0xC] = { 1 };                    /* npad id 0: player 1's Pro Controller (hid.c) */
        fprintf(stderr, "am: controller support applet (frame %llu)\n", (unsigned long long)nvn_frame_count());
        hid_connect_controllers();
        queue_push(&a->out, info, sizeof info);
        applet_finish(a);
        break;
    }
    case APPLET_SWKBD:
        swkbd_run(a);
        break;
    default:
        fprintf(stderr, "am: library applet %#x (mode %u) isn't implemented: it closes with no answer\n", a->id, a->mode);
        applet_finish(a);
    }
}

static Storage *in_storage(IpcCall *call) {           /* the IStorage passed to a command */
    ServiceObject *o = call->n_in_objects ? call->in_objects[0] : NULL;
    if (!o || o->type != &storage_type) {
        fprintf(stderr, "am: expected an IStorage argument (outside a domain?)\n");
        return NULL;
    }
    return o->state;
}

static void library_applet_destroy(ServiceObject *self) {
    LibraryApplet *a = self->state;
    for (int i = 0; i < a->n_in; i++) storage_unref(a->in[i]);
    for (int i = 0; i < a->out.n; i++) free(a->out.item[i].data);
    for (int i = 0; i < a->interactive_out.n; i++) free(a->interactive_out.item[i].data);
    free(a);
}

static void library_applet_dispatch(ServiceObject *self, IpcCall *call) {
    LibraryApplet *a = self->state;
    switch (call->cmd) {
    case 0:                                   /* GetAppletStateChangedEvent */
        ipc_out_handle(call, kevent_readable_handle(a->state_changed), 0);
        break;
    case 1:                                   /* IsCompleted -> u8 */
        ipc_out32(call, a->completed);
        break;
    case 10:                                  /* Start */
        if (!a->started) { a->started = 1; applet_run(a); }
        break;
    case 20: case 25:                         /* RequestExit, Terminate */
        if (!a->completed) applet_finish(a);
        break;
    case 30:                                  /* GetResult: success */
    case 50:                                  /* SetOutOfFocusApplicationSuspendingEnabled */
    case 60:                                  /* PresetLibraryAppletGpuTimeSliceZero */
    case 102:                                 /* PushExtraStorage */
        break;
    case 100: {                               /* PushInData(IStorage) */
        Storage *st = in_storage(call);
        if (st && a->n_in < APPLET_QUEUE_MAX) { st->refs++; a->in[a->n_in++] = st; }
        break;
    }
    case 103:                                 /* PushInteractiveInData(IStorage) */
        if (a->id == APPLET_SWKBD && a->started && !a->completed) swkbd_text_checked(a, in_storage(call));
        break;
    case 101:                                 /* PopOutData -> IStorage */
        queue_pop(&a->out, call);
        break;
    case 104:                                 /* PopInteractiveOutData -> IStorage */
        queue_pop(&a->interactive_out, call);
        break;
    case 105:                                 /* GetPopOutDataEvent */
        ipc_out_handle(call, kevent_readable_handle(a->out.event), 0);
        break;
    case 106:                                 /* GetPopInteractiveOutDataEvent */
        ipc_out_handle(call, kevent_readable_handle(a->interactive_out.event), 0);
        break;
    case 110:                                 /* NeedsToExitProcess -> u8 */
        ipc_out32(call, 0);
        break;
    case 120: {                               /* GetLibraryAppletInfo -> applet id, mode */
        uint32_t info[2] = { a->id, a->mode };
        ipc_out_bytes(call, info, 8);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType library_applet_type = { "am:ILibraryAppletAccessor", library_applet_dispatch,
                                                  library_applet_destroy };

static void applet_creator_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                                 /* CreateLibraryApplet(applet id, mode) -> ILibraryAppletAccessor */
        LibraryApplet *a = calloc(1, sizeof *a);
        if (!a) exit(1);
        a->id = ipc_in32(call, 0);
        a->mode = ipc_in32(call, 4);
        a->state_changed = kevent_new("am: library applet state changed");
        a->out.event = kevent_new("am: library applet out data");
        a->interactive_out.event = kevent_new("am: library applet interactive out data");
        ipc_out_object(call, ipc_new_object(&library_applet_type, a));
        break;
    }
    case 10: {                                /* CreateStorage(size) -> IStorage, zeroed */
        uint64_t size = ipc_in64(call, 0);
        if (size > (64u << 20)) { call->result = AM_RESULT(503); break; }
        ipc_out_object(call, new_storage(NULL, size));
        break;
    }
    case 11: {                                /* CreateTransferMemoryStorage(transfer memory, writable, size) -> IStorage */
        KHandle *h = call->n_copy ? khandle_get(call->copy_handles[0]) : NULL;
        const KTransferMemory *t = h && h->type == KOBJ_TRANSFER_MEMORY ? h->obj : NULL;
        uint64_t size = ipc_in64(call, 8);
        uint8_t *p = t ? ipc_guest_ptr(t->addr, size) : NULL;
        if (!p || size > t->size) { call->result = AM_RESULT(503); break; }
        Storage *st = calloc(1, sizeof *st);
        if (!st) exit(1);
        st->data = p;                         /* the game's memory, shared */
        st->size = size;
        st->refs = 1;
        st->guest = 1;
        ipc_out_object(call, ipc_new_object(&storage_type, st));
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}

/* ---- ICommonStateGetter ---- */
static void common_state_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                   /* GetEventHandle -> event, signalled when a message waits */
        ipc_out_handle(call, kevent_readable_handle(message_event), 0);
        break;
    case 1:                                   /* ReceiveMessage -> AppletMessage */
        if (n_messages == 0) { call->result = RESULT_NO_MESSAGES; break; }
        ipc_out32(call, messages[0]);
        memmove(messages, messages + 1, --n_messages * sizeof *messages);
        if (n_messages == 0) kevent_clear(message_event);
        break;
    case 5:                                   /* GetOperationMode -> u8 */
        ipc_out32(call, applet_handheld() ? OPERATION_MODE_HANDHELD : OPERATION_MODE_DOCKED);
        break;
    case 6:                                   /* GetPerformanceMode -> u32 (normal / boost) */
        ipc_out32(call, applet_handheld() ? 0 : 1);
        break;
    case 8:                                   /* GetBootMode -> u8: normal */
        ipc_out32(call, 0);
        break;
    case 9:                                   /* GetCurrentFocusState -> u8: 1 = in focus */
        ipc_out32(call, 1);
        break;
    case 60:                                  /* GetDefaultDisplayResolution -> width, height */
        ipc_out32(call, applet_handheld() ? 1280 : 1920);
        ipc_out32(call, applet_handheld() ? 720 : 1080);
        break;
    case 10:                                  /* RequestToAcquireSleepLock */
    case 51:                                  /* SetVrModeEnabled */
    case 52:                                  /* SetLcdBacklighOffEnabled */
    case 53: case 54:                         /* BeginVrModeEx, EndVrModeEx */
    case 66:                                  /* SetCpuBoostMode: we have no clocks to change */
        break;
    case 50:                                  /* IsVrModeEnabled -> u8: no */
    case 55:                                  /* IsInControllerFirmwareUpdateSection -> u8: no */
        ipc_out32(call, 0);
        break;
    case 61:                                  /* GetDefaultDisplayResolutionChangeEvent */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("am: display resolution change")), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType common_state_type = { "am:ICommonStateGetter", common_state_dispatch, NULL };

/* ---- ISelfController: the game telling the system how it wants to be treated ---- */
static void self_controller_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1: case 2:                           /* LockExit, UnlockExit */
    case 3: case 4:                           /* EnterFatalSection, LeaveFatalSection */
    case 10:                                  /* SetScreenShotPermission */
    case 11: case 12:                         /* Set{Operation,Performance}ModeChangedNotification */
    case 13:                                  /* SetFocusHandlingMode */
    case 14:                                  /* SetRestartMessageEnabled */
    case 15:                                  /* SetScreenShotAppletIdentityInfo */
    case 16:                                  /* SetOutOfFocusSuspendingEnabled */
    case 19:                                  /* SetAlbumImageOrientation */
    case 50:                                  /* SetHandlesRequestToDisplay */
    case 62:                                  /* SetIdleTimeDetectionExtension */
    case 65:                                  /* ReportUserIsActive */
    case 68:                                  /* SetAutoSleepDisabled */
    case 100:                                 /* SetAlbumImageTakenNotificationEnabled */
    case 110:                                 /* SetApplicationAlbumUserData */
        break;
    case 9:                                   /* GetLibraryAppletLaunchableEvent */
        ipc_out_handle(call, kevent_readable_handle(library_applet_launchable()), 0);
        break;
    case 91:                                  /* GetAccumulatedSuspendedTickChangedEvent */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("am: suspended tick changed")), 0);
        break;
    case 69:                                  /* IsAutoSleepDisabled -> u8 */
        ipc_out32(call, 0);
        break;
    case 90:                                  /* GetAccumulatedSuspendedTickValue -> u64: never suspended */
        ipc_out64(call, 0);
        break;
    case 40:                                  /* CreateManagedDisplayLayer -> layer id */
        ipc_out64(call, 1);
        break;
    case 63:                                  /* GetIdleTimeDetectionExtension -> u32 */
        ipc_out32(call, 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType self_controller_type = { "am:ISelfController", self_controller_dispatch, NULL };

/* ---- IWindowController ---- */
static void window_controller_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1:                                   /* GetAppletResourceUserId -> u64 */
        ipc_out64(call, 0x51);                /* our process id stands in for it */
        break;
    case 10:                                  /* AcquireForegroundRights */
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType window_controller_type = { "am:IWindowController", window_controller_dispatch, NULL };

/* ---- IAudioController ---- */
static void audio_controller_dispatch(ServiceObject *self, IpcCall *call) {
    float one = 1.0f;
    switch (call->cmd) {
    case 0: case 3: case 4:                   /* SetExpectedMasterVolume, ChangeMainAppletMasterVolume, SetTransparentVolumeRate */
        break;
    case 1: case 2:                           /* Get{Main,Library}AppletExpectedMasterVolume -> f32 */
        ipc_out_bytes(call, &one, 4);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType audio_controller_type = { "am:IAudioController", audio_controller_dispatch, NULL };

static void display_controller_dispatch(ServiceObject *self, IpcCall *call) { ipc_unknown_command(self, call); }
static const ServiceType display_controller_type = { "am:IDisplayController", display_controller_dispatch, NULL };
static void debug_functions_dispatch(ServiceObject *self, IpcCall *call) { ipc_unknown_command(self, call); }
static const ServiceType debug_functions_type = { "am:IDebugFunctions", debug_functions_dispatch, NULL };
static const ServiceType applet_creator_type = { "am:ILibraryAppletCreator", applet_creator_dispatch, NULL };

/* ---- IApplicationFunctions ---- */
enum { LAUNCH_PARAM_USER_CHANNEL = 1, LAUNCH_PARAM_PRESELECTED_USER = 2 };

static void app_functions_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1: {                                 /* PopLaunchParameter(kind) -> IStorage */
        static int popped_user;
        if (ipc_in32(call, 0) != LAUNCH_PARAM_PRESELECTED_USER || popped_user) {
            call->result = RESULT_NO_DATA_IN_CHANNEL;
            break;
        }
        /* "this user started the game": magic, is-selected flag, then the 16-byte user id */
        uint8_t p[0x88] = { 0 };
        uint32_t magic = 0xC79497CA, selected = 1;
        uint64_t uid[2] = { LAA_USER_ID_LO, LAA_USER_ID_HI };
        memcpy(p, &magic, 4);
        memcpy(p + 4, &selected, 1);
        memcpy(p + 8, uid, 16);
        popped_user = 1;
        ipc_out_object(call, new_storage(p, sizeof p));
        break;
    }
    case 20:                                  /* EnsureSaveData(user id) -> u64 extra space needed: none */
        ipc_out64(call, 0);
        break;
    case 21: {                                /* GetDesiredLanguage -> language code */
        char lang[8] = "en-US";
        ipc_out_bytes(call, lang, 8);
        break;
    }
    case 22:                                  /* SetTerminateResult */
    case 66:                                  /* InitializeGamePlayRecording */
    case 67:                                  /* SetGamePlayRecordingState */
        break;
    case 40:                                  /* NotifyRunning -> u8 */
        ipc_out32(call, 0);
        break;
    case 50: {                                /* GetPseudoDeviceId -> 16 bytes */
        uint64_t id[2] = { 0x4C41415245434F4Dull, 1 };
        ipc_out_bytes(call, id, 16);
        break;
    }
    case 130:                                 /* GetGpuErrorDetectedSystemEvent */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("am: gpu error detected")), 0);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType app_functions_type = { "am:IApplicationFunctions", app_functions_dispatch, NULL };

/* ---- IApplicationProxy: hands out the objects above ---- */
static void app_proxy_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:    out_new(call, &common_state_type); break;
    case 1:    out_new(call, &self_controller_type); break;
    case 2:    out_new(call, &window_controller_type); break;
    case 3:    out_new(call, &audio_controller_type); break;
    case 4:    out_new(call, &display_controller_type); break;
    case 11:   out_new(call, &applet_creator_type); break;
    case 20:   out_new(call, &app_functions_type); break;
    case 1000: out_new(call, &debug_functions_type); break;
    default:   ipc_unknown_command(self, call);
    }
}
static const ServiceType app_proxy_type = { "am:IApplicationProxy", app_proxy_dispatch, NULL };

/* ---- appletOE ---- */
static void applet_oe_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0:                                   /* OpenApplicationProxy(pid, process handle) */
        out_new(call, &app_proxy_type);
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType applet_oe_type = { "appletOE", applet_oe_dispatch, NULL };

ServiceObject *service_applet_oe_create(void) {
    if (!message_event) {
        message_event = kevent_new("am: applet message");
        push_message(MSG_FOCUS_STATE_CHANGED);   /* the game starts in focus */
    }
    return ipc_new_object(&applet_oe_type, NULL);
}
