/*
 * hid.c - "hid", controllers and touch. The game reads input from a 0x40000-byte shared
 * memory block that the service keeps writing to; commands only switch devices on and say
 * which controller styles the game supports.
 *
 * For now the shared memory stays zero (no input at all). Filling it from real controllers
 * (SDL3) is phase 5. switchbrew.org/wiki/HID_services, switchbrew.org/wiki/HID_Shared_Memory
 */
#include "services.h"
#include "kernel.h"

#define HID_SHARED_MEMORY_SIZE 0x40000

static KSharedMemory *hid_shm;
static uint32_t supported_styles;
static uint32_t joy_hold_type;
static KEvent *style_set_update_event;

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
    case 102:                                     /* SetSupportedNpadIdType(list) */
    case 109:                                     /* ActivateNpadWithRevision */
    case 122: case 123: case 124:                 /* SetNpadJoyAssignmentMode Single/Dual */
    case 125:                                     /* MergeSingleJoyAsDualJoy */
    case 128:                                     /* SetNpadHandheldActivationMode */
    case 201: case 206:                           /* SendVibrationValue(s) */
    case 1000:                                    /* SetNpadCommunicationMode */
        break;
    case 100:                                     /* SetSupportedNpadStyleSet(styles) */
        supported_styles = ipc_in32(call, 0);
        break;
    case 101:                                     /* GetSupportedNpadStyleSet -> styles */
        ipc_out32(call, supported_styles);
        break;
    case 106:                                     /* AcquireNpadStyleSetUpdateEventHandle */
        if (!style_set_update_event) style_set_update_event = kevent_new("hid: npad style set update");
        ipc_out_handle(call, kevent_readable_handle(style_set_update_event), 0);
        break;
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
    if (!hid_shm) hid_shm = kshm_new(HID_SHARED_MEMORY_SIZE);
    return ipc_new_object(&hid_type, NULL);
}
