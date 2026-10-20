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
 *
 * switchbrew.org/wiki/Applet_Manager_services
 */
#include "services.h"
#include "kernel.h"
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
static const int operation_mode = OPERATION_MODE_HANDHELD;

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
typedef struct { uint8_t *data; uint64_t size; int refs; } Storage;   /* refs: storage + accessors */

static void storage_release(ServiceObject *self) {
    Storage *st = self->state;
    if (--st->refs == 0) { free(st->data); free(st); }
}

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
    if (!st || !(st->data = calloc(1, size))) exit(1);
    memcpy(st->data, data, size);
    st->size = size;
    st->refs = 1;
    return ipc_new_object(&storage_type, st);
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
        ipc_out32(call, (uint32_t)operation_mode);
        break;
    case 6:                                   /* GetPerformanceMode -> u32 (normal / boost) */
        ipc_out32(call, (uint32_t)operation_mode);
        break;
    case 8:                                   /* GetBootMode -> u8: normal */
        ipc_out32(call, 0);
        break;
    case 9:                                   /* GetCurrentFocusState -> u8: 1 = in focus */
        ipc_out32(call, 1);
        break;
    case 60:                                  /* GetDefaultDisplayResolution -> width, height */
        ipc_out32(call, operation_mode == OPERATION_MODE_HANDHELD ? 1280 : 1920);
        ipc_out32(call, operation_mode == OPERATION_MODE_HANDHELD ? 720 : 1080);
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
    case 91:                                  /* GetAccumulatedSuspendedTickChangedEvent */
        ipc_out_handle(call, kevent_readable_handle(kevent_new("am: library applet launchable / suspended tick")), 0);
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
static void applet_creator_dispatch(ServiceObject *self, IpcCall *call) { ipc_unknown_command(self, call); }
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
